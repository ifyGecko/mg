/* This file is in the public domain. */

/*
 * term.c -- M-x term for mg.
 *
 * A terminal-emulator buffer: spawns $SHELL on a PTY with TERM=ansi,
 * drains its output asynchronously into a mg buffer, and forwards
 * keystrokes back one byte at a time. Unlike M-x shell (comint-style,
 * TERM=dumb, line-mode input, SGR-stripped), M-x term implements an
 * ANSI/VT100 subset with a fixed rows x cols screen so it can host
 * full-screen curses apps (vi, less, htop, top).
 *
 * Character mode is the default: nearly every key you press is written
 * straight to the PTY. C-c is the term-command prefix (C-c C-c sends
 * SIGINT to the shell, C-c C-k terminates the process). C-x is left
 * bound to mg's usual prefix so you can still `C-x b` or `C-x k` out
 * of a term buffer.
 *
 * Screen model. The buffer's last `rows` lines are the terminal grid;
 * everything above is scrollback. A "home" line pointer anchors the
 * top-left of the grid; the cursor tracks (row, col) within it. Full-
 * screen scrolls advance the home pointer forward (the discarded top
 * line becomes scrollback); scrolls within a smaller DECSTBM region
 * discard the top region line outright.
 *
 * Not implemented (deliberate MVP cuts): colors (SGR is parsed and
 * dropped), the alternate screen buffer (`?1049h/l` is tracked but
 * ignored -- curses apps like vim just write into the main buffer),
 * G0/G1 line-drawing charsets (swallowed), mouse tracking, bracketed
 * paste, and term line-mode. Output arrives via poll(2) from ttgetc()
 * exactly the same way shell.c does.
 */

#include <sys/ioctl.h>
#include <sys/queue.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <fcntl.h>
#include <paths.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#if HAVE_PTY_H
#include <pty.h>
#include <utmp.h>
#elif HAVE_UTIL_H
#include <util.h>
#elif HAVE_LIBUTIL_H
#include <libutil.h>
#endif

#include "def.h"
#include "funmap.h"
#include "kbd.h"
#include "key.h"

#define TERM_BUFNAME       "*term*"
#define TERM_MAX_PROCS     8
#define TERM_READ_BUFSIZE  4096
#define TERM_CSI_BUFSIZE   64
#define TERM_MAX_PARAMS    16
#define TERM_MAX_ARGV      32
#define TERM_DEFAULT_ROWS  24
#define TERM_DEFAULT_COLS  80

extern int changemode(int, int, char *);
extern struct KEYMAPE (7) cXmap;

enum term_state {
	TS_NORMAL = 0,		/* plain output */
	TS_ESC,			/* saw ESC */
	TS_CSI,			/* inside ESC[... */
	TS_OSC,			/* inside ESC]...  (swallow until BEL/ST) */
	TS_OSC_ESC,		/* inside OSC and saw ESC (look for \\) */
	TS_CHARSET		/* swallow one byte after ESC ( or ESC ) */
};

struct termproc {
	pid_t              pid;
	int                fd;
	struct buffer     *bp;

	/*
	 * Screen anchor. home_lp is the buffer line that holds row 0 of
	 * the terminal grid; home_line is its 1-based buffer-line number
	 * (matches struct mgwin::w_dotline conventions).
	 */
	struct line       *home_lp;
	int                home_line;

	unsigned short     rows;
	unsigned short     cols;

	/* Cursor position within screen: 0 <= cur_row < rows, 0 <= cur_col < cols */
	int                cur_row;
	int                cur_col;

	/* Saved cursor for ESC 7 / ESC 8 */
	int                saved_row;
	int                saved_col;

	/* Scroll region (inclusive rows), defaults to 0..rows-1 */
	int                scroll_top;
	int                scroll_bot;

	/*
	 * Auto-wrap deferred flag. Real terminals do NOT wrap when the
	 * cursor lands at column `cols` -- they wait until the *next*
	 * printable byte, so writing exactly `cols` characters onto the
	 * last line does not scroll. This mirrors the DEC "pending wrap"
	 * bit.
	 */
	int                wrap_pending;

	/* ANSI parser state */
	enum term_state    state;
	char               csi_buf[TERM_CSI_BUFSIZE];
	int                csi_len;
	int                csi_priv;	/* '?' seen after CSI */

	struct termproc   *next;
};

static struct termproc *terms;
static volatile sig_atomic_t term_chld_flag;
static int term_initialized;

void	term_init(void);
int	term(int, int);
int	term_send_char(int, int);
int	term_interrupt(int, int);
int	term_kill_process(int, int);
void	term_wait_for_input(void);
void	term_buffer_killed(struct buffer *);
void	term_kill_all(void);
void	term_notify_resize(void);

static void term_sigchld(int);
static void term_reap(void);
static void term_remove(struct termproc *);
static struct termproc *term_find_by_bp(struct buffer *);
static struct termproc *term_find_by_pid(pid_t);

static int  term_read(struct termproc *);
static void term_feed(struct termproc *, unsigned char);
static void term_dispatch_csi(struct termproc *, unsigned char final);

static void term_init_screen(struct termproc *);
static struct line *term_line_at(struct termproc *, int row);
static void term_pad_line(struct line *, int col);
static void term_put_char(struct termproc *, int c);
static void term_advance_cursor(struct termproc *);
static void term_newline(struct termproc *);
static void term_reverse_index(struct termproc *);
static void term_scroll_up(struct termproc *, int top, int bot, int n);
static void term_scroll_down(struct termproc *, int top, int bot, int n);
static void term_erase_line(struct termproc *, int row, int start, int end);
static void term_erase_display(struct termproc *, int mode);
static void term_insert_lines(struct termproc *, int n);
static void term_delete_lines(struct termproc *, int n);
static void term_insert_chars(struct termproc *, int n);
static void term_delete_chars(struct termproc *, int n);
static void term_place_cursor(struct termproc *, int row, int col);

static void term_sync_windows(struct termproc *);
static void term_touch_windows(struct buffer *);
static void term_finish_message(struct buffer *);
static struct line *term_bp_ensure_line(struct buffer *);
static void term_terminate(struct termproc *);

/*
 * Char-mode keymap.
 *
 *   C-c          -> term-cc-map prefix
 *     C-c C-c    -> term-interrupt (SIGINT to fg process group)
 *     C-c C-k    -> term-kill-process (SIGHUP + close)
 *   C-x          -> mg's standard C-x prefix (escape hatch)
 *   anything else-> term-send-char (raw byte to PTY)
 *
 * map_default handles the "anything else" case. Note that mg's
 * getkey() splits high-bit bytes into ESC + low-7-bits when use_metakey
 * is on -- that arrives as two consecutive term_send_char calls, which
 * is exactly the standard meta encoding a terminal app expects.
 */

static PF term_cc_subs[] = {
	term_interrupt,		/* C-c C-c */
	rescan,			/* C-c C-d */
	rescan,			/* C-c C-e */
	rescan,			/* C-c C-f */
	rescan,			/* C-c C-g */
	rescan,			/* C-c C-h */
	rescan,			/* C-c C-i */
	rescan,			/* C-c C-j (reserved for future line-mode) */
	term_kill_process,	/* C-c C-k */
};

static struct KEYMAPE (1) term_cc_map = {
	1, 1, rescan,
	{
		{ CCHR('C'), CCHR('K'), term_cc_subs, NULL }
	}
};

static PF term_cc_prefix[]  = { NULL };
static PF term_cx_prefix[]  = { NULL };

static struct KEYMAPE (2) termmap = {
	2, 2, term_send_char,
	{
		{ CCHR('C'), CCHR('C'), term_cc_prefix, (KEYMAP *)&term_cc_map },
		{ CCHR('X'), CCHR('X'), term_cx_prefix, (KEYMAP *)&cXmap }
	}
};

void
term_init(void)
{
	struct sigaction sa;

	if (term_initialized)
		return;
	term_initialized = 1;

	funmap_add(term, "term", 0);
	funmap_add(term_send_char, "term-send-char", 0);
	funmap_add(term_interrupt, "term-interrupt", 0);
	funmap_add(term_kill_process, "term-kill-process", 0);
	maps_add((KEYMAP *)&termmap, "term");

	/*
	 * We share SIGCHLD with shell.c. Whichever module's handler runs
	 * first sets its flag; each module's reaper waitpid()s only for
	 * pids it owns, so we can safely install a handler here too and
	 * let both flags get polled independently.
	 */
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = term_sigchld;
	sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
	sigemptyset(&sa.sa_mask);
	(void)sigaction(SIGCHLD, &sa, NULL);
}

static void
term_sigchld(int signo)
{
	(void)signo;
	term_chld_flag = 1;
}

/*
 * M-x term -- start (or switch to) an interactive terminal in the
 * current window. Prompts for the program to run (default $SHELL),
 * accepting space-separated argv. Reuses the *term* buffer when its
 * process is alive.
 */
int
term(int f, int n)
{
	struct buffer	*bp;
	struct termproc *sp;
	struct winsize	 ws;
	int		 master;
	pid_t		 pid;
	char		 progbuf[NFILEN];
	char		*shellp;
	unsigned short	 rows, cols;

	/*
	 * Reuse a live *term* buffer verbatim -- no re-prompt when the
	 * user's just switching back to the running shell.
	 */
	bp = bfind(TERM_BUFNAME, FALSE);
	if (bp != NULL) {
		sp = term_find_by_bp(bp);
		if (sp != NULL) {
			showbuffer(bp, curwp, WFFRAME | WFFULL);
			curbp = bp;
			return (TRUE);
		}
	}

	/*
	 * Fresh process: prompt for the program with $SHELL as the
	 * default, matching real emacs `M-x term`. eread with EFDEF
	 * pre-fills the minibuffer so RET accepts the default.
	 */
	if ((shellp = getenv("SHELL")) == NULL || shellp[0] == '\0')
		shellp = _PATH_BSHELL;
	if (strlcpy(progbuf, shellp, sizeof(progbuf)) >= sizeof(progbuf)) {
		dobeep();
		ewprintf("term: program path too long");
		return (FALSE);
	}
	if (eread("Run program: ", progbuf, sizeof(progbuf),
	    EFDEF | EFNEW | EFCR | EFFILE) == NULL)
		return (ABORT);
	if (progbuf[0] == '\0')
		return (FALSE);

	/* Create the buffer (or clear a stale one from a prior dead term). */
	if (bp != NULL) {
		if (bclear(bp) != TRUE)
			return (FALSE);
	} else {
		bp = bfind(TERM_BUFNAME, TRUE);
		if (bp == NULL)
			return (FALSE);
	}

	{
		struct buffer *saved_bp = curbp;
		curbp = bp;
		(void)changemode(FFARG, 1, "term");
		curbp = saved_bp;
	}

	showbuffer(bp, curwp, WFFRAME | WFFULL);
	curbp = bp;

	rows = curwp->w_ntrows > 0 ? (unsigned short)curwp->w_ntrows
	    : TERM_DEFAULT_ROWS;
	cols = curwp->w_ncols  > 0 ? (unsigned short)curwp->w_ncols
	    : TERM_DEFAULT_COLS;
	memset(&ws, 0, sizeof(ws));
	ws.ws_row = rows;
	ws.ws_col = cols;

	pid = forkpty(&master, NULL, NULL, &ws);
	if (pid == -1) {
		dobeep();
		ewprintf("term: forkpty: %s", strerror(errno));
		return (FALSE);
	}

	if (pid == 0) {
		char	*argv[TERM_MAX_ARGV + 1];
		int	 argc = 0;
		char	*tok, *save = NULL;

		/*
		 * Simple whitespace split for argv. progbuf is a copy in
		 * the forked address space, so strtok_r's in-place mutation
		 * is safe. Quoting/backslash isn't handled -- users with
		 * spaces in paths can point $SHELL at a symlink.
		 */
		for (tok = strtok_r(progbuf, " \t", &save);
		     tok != NULL && argc < TERM_MAX_ARGV;
		     tok = strtok_r(NULL, " \t", &save))
			argv[argc++] = tok;
		argv[argc] = NULL;
		if (argc == 0)
			_exit(127);

		setenv("TERM", "ansi", 1);
		setenv("INSIDE_EMACS", "mg,term", 1);
		unsetenv("COLUMNS");
		unsetenv("LINES");
		signal(SIGCHLD, SIG_DFL);
		signal(SIGINT,  SIG_DFL);
		signal(SIGQUIT, SIG_DFL);
		signal(SIGPIPE, SIG_DFL);
		execvp(argv[0], argv);
		_exit(127);
	}

	fcntl(master, F_SETFL, O_NONBLOCK);

	if ((sp = calloc(1, sizeof(*sp))) == NULL) {
		dobeep();
		ewprintf("term: out of memory");
		kill(pid, SIGHUP);
		close(master);
		return (FALSE);
	}
	sp->pid        = pid;
	sp->fd         = master;
	sp->bp         = bp;
	sp->rows       = rows;
	sp->cols       = cols;
	sp->scroll_top = 0;
	sp->scroll_bot = rows - 1;
	sp->state      = TS_NORMAL;
	sp->next       = terms;
	terms          = sp;

	term_init_screen(sp);

	ewprintf("Started %s (pid %d)", progbuf, (int)pid);
	return (TRUE);
}

/*
 * Seed the buffer with `rows` blank lines so the emulator can always
 * find every screen row without allocating on the hot output path.
 * Cursor lands at (0, 0).
 */
static void
term_init_screen(struct termproc *sp)
{
	struct buffer *bp = sp->bp;
	struct line   *lp;

	sp->home_lp = term_bp_ensure_line(bp);
	if (sp->home_lp == NULL)
		return;
	sp->home_line = 1;

	/*
	 * Extend downward until the buffer holds at least `rows` lines
	 * from home. b_lines is 1 after ensure_line; grow to rows.
	 */
	while (bp->b_lines < sp->rows) {
		if ((lp = lalloc(0)) == NULL)
			return;
		lp->l_used = 0;
		lp->l_fp = bp->b_headp;
		lp->l_bp = bp->b_headp->l_bp;
		bp->b_headp->l_bp->l_fp = lp;
		bp->b_headp->l_bp = lp;
		bp->b_lines++;
	}

	sp->cur_row = 0;
	sp->cur_col = 0;
	sp->saved_row = 0;
	sp->saved_col = 0;
	term_sync_windows(sp);
}

static struct termproc *
term_find_by_bp(struct buffer *bp)
{
	struct termproc *sp;

	for (sp = terms; sp != NULL; sp = sp->next)
		if (sp->bp == bp)
			return (sp);
	return (NULL);
}

static struct termproc *
term_find_by_pid(pid_t pid)
{
	struct termproc *sp;

	for (sp = terms; sp != NULL; sp = sp->next)
		if (sp->pid == pid)
			return (sp);
	return (NULL);
}

static void
term_remove(struct termproc *sp)
{
	struct termproc **pp;

	for (pp = &terms; *pp != NULL; pp = &(*pp)->next) {
		if (*pp == sp) {
			*pp = sp->next;
			free(sp);
			return;
		}
	}
}

/*
 * Reap any dead children flagged by term_sigchld. For each of our
 * terms that died, drain remaining bytes, close, and drop a marker
 * line.
 */
static void
term_reap(void)
{
	int		 status;
	pid_t		 pid;
	struct termproc	*sp;

	term_chld_flag = 0;
	while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
		sp = term_find_by_pid(pid);
		if (sp == NULL)
			continue;
		(void)term_read(sp);
		if (sp->fd >= 0) {
			close(sp->fd);
			sp->fd = -1;
		}
		term_finish_message(sp->bp);
		term_touch_windows(sp->bp);
		term_remove(sp);
	}
}

static void
term_finish_message(struct buffer *bp)
{
	addlinef(bp, "");
	addlinef(bp, "Process term finished");
}

static int
term_read(struct termproc *sp)
{
	char	buf[TERM_READ_BUFSIZE];
	ssize_t	n;
	int	i, changed = 0;

	if (sp->fd < 0)
		return (-1);
	n = read(sp->fd, buf, sizeof(buf));
	if (n == 0)
		return (-1);
	if (n < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
			return (0);
		return (-1);
	}
	for (i = 0; i < n; i++) {
		term_feed(sp, (unsigned char)buf[i]);
		changed = 1;
	}
	if (changed) {
		term_sync_windows(sp);
		term_touch_windows(sp->bp);
	}
	return (changed);
}

/*
 * The ANSI/VT100 parser. Drives sp->state one byte at a time. In the
 * normal state, non-control bytes turn into cursor writes via
 * term_put_char().
 */
static void
term_feed(struct termproc *sp, unsigned char c)
{
	switch (sp->state) {
	case TS_NORMAL:
		switch (c) {
		case 0x00:
			return;
		case 0x07:		/* BEL */
			dobeep();
			return;
		case 0x08:		/* BS */
			sp->wrap_pending = 0;
			if (sp->cur_col > 0)
				sp->cur_col--;
			return;
		case 0x09: {		/* HT */
			int next = (sp->cur_col + 8) & ~7;
			sp->wrap_pending = 0;
			if (next >= sp->cols)
				next = sp->cols - 1;
			sp->cur_col = next;
			return;
		}
		case 0x0A:		/* LF */
		case 0x0B:		/* VT */
		case 0x0C:		/* FF */
			sp->wrap_pending = 0;
			term_newline(sp);
			return;
		case 0x0D:		/* CR */
			sp->wrap_pending = 0;
			sp->cur_col = 0;
			return;
		case 0x1B:		/* ESC */
			sp->state = TS_ESC;
			return;
		default:
			if (c < 0x20)
				return;
			term_put_char(sp, (int)c);
			return;
		}
	case TS_ESC:
		switch (c) {
		case '[':
			sp->state = TS_CSI;
			sp->csi_len = 0;
			sp->csi_priv = 0;
			return;
		case ']':
			sp->state = TS_OSC;
			return;
		case '(':
		case ')':
		case '*':
		case '+':
			sp->state = TS_CHARSET;
			return;
		case '7':		/* DECSC */
			sp->saved_row = sp->cur_row;
			sp->saved_col = sp->cur_col;
			sp->state = TS_NORMAL;
			return;
		case '8':		/* DECRC */
			term_place_cursor(sp, sp->saved_row, sp->saved_col);
			sp->state = TS_NORMAL;
			return;
		case 'D':		/* IND: index (down + scroll) */
			term_newline(sp);
			sp->state = TS_NORMAL;
			return;
		case 'E':		/* NEL: next line (CR + LF) */
			sp->cur_col = 0;
			term_newline(sp);
			sp->state = TS_NORMAL;
			return;
		case 'M':		/* RI: reverse index */
			term_reverse_index(sp);
			sp->state = TS_NORMAL;
			return;
		case 'c':		/* RIS: full reset */
			sp->cur_row = 0;
			sp->cur_col = 0;
			sp->saved_row = 0;
			sp->saved_col = 0;
			sp->scroll_top = 0;
			sp->scroll_bot = sp->rows - 1;
			sp->wrap_pending = 0;
			term_erase_display(sp, 2);
			sp->state = TS_NORMAL;
			return;
		case 0x1B:
			return;		/* stay in ESC */
		default:
			sp->state = TS_NORMAL;
			return;
		}
	case TS_CSI:
		if (c == '?' && sp->csi_len == 0) {
			sp->csi_priv = 1;
			return;
		}
		if (c >= 0x40 && c <= 0x7E) {
			term_dispatch_csi(sp, c);
			sp->state = TS_NORMAL;
			return;
		}
		if (sp->csi_len < (int)sizeof(sp->csi_buf) - 1)
			sp->csi_buf[sp->csi_len++] = (char)c;
		else
			sp->state = TS_NORMAL;	/* runaway; abort */
		return;
	case TS_OSC:
		if (c == 0x07) {
			sp->state = TS_NORMAL;
			return;
		}
		if (c == 0x1B) {
			sp->state = TS_OSC_ESC;
			return;
		}
		return;
	case TS_OSC_ESC:
		/* ESC \\ ends the string (ST), anything else keeps swallowing */
		if (c == '\\')
			sp->state = TS_NORMAL;
		else
			sp->state = TS_OSC;
		return;
	case TS_CHARSET:
		sp->state = TS_NORMAL;
		return;
	}
}

/*
 * Parse the collected numeric parameters out of csi_buf, then hand off
 * to the right screen primitive. `final` is the terminator byte (the
 * command letter). csi_priv is set if we saw '?' after CSI (DEC private).
 */
static void
term_dispatch_csi(struct termproc *sp, unsigned char final)
{
	int params[TERM_MAX_PARAMS];
	int nparams = 0;
	int i, cur, saw_digit;

	sp->csi_buf[sp->csi_len] = '\0';
	cur = 0;
	saw_digit = 0;
	for (i = 0; i < sp->csi_len; i++) {
		char ch = sp->csi_buf[i];
		if (ch >= '0' && ch <= '9') {
			cur = cur * 10 + (ch - '0');
			saw_digit = 1;
		} else if (ch == ';') {
			if (nparams < TERM_MAX_PARAMS)
				params[nparams++] = saw_digit ? cur : 0;
			cur = 0;
			saw_digit = 0;
		}
	}
	if (nparams < TERM_MAX_PARAMS && (saw_digit || nparams > 0))
		params[nparams++] = cur;

#define	P(i, dflt)	((i) < nparams ? (params[(i)] > 0 ? params[(i)] : (dflt)) : (dflt))

	if (sp->csi_priv) {
		/*
		 * DEC private modes. We deliberately track very little:
		 * apps rely on us not choking when they set them.
		 */
		switch (final) {
		case 'h':
		case 'l':
			return;
		}
		return;
	}

	switch (final) {
	case 'A':		/* CUU */
		term_place_cursor(sp, sp->cur_row - P(0, 1), sp->cur_col);
		return;
	case 'B':		/* CUD */
	case 'e':		/* VPR */
		term_place_cursor(sp, sp->cur_row + P(0, 1), sp->cur_col);
		return;
	case 'C':		/* CUF */
	case 'a':		/* HPR */
		term_place_cursor(sp, sp->cur_row, sp->cur_col + P(0, 1));
		return;
	case 'D':		/* CUB */
		term_place_cursor(sp, sp->cur_row, sp->cur_col - P(0, 1));
		return;
	case 'E':		/* CNL */
		term_place_cursor(sp, sp->cur_row + P(0, 1), 0);
		return;
	case 'F':		/* CPL */
		term_place_cursor(sp, sp->cur_row - P(0, 1), 0);
		return;
	case 'G':		/* CHA */
	case '`':		/* HPA */
		term_place_cursor(sp, sp->cur_row, P(0, 1) - 1);
		return;
	case 'H':		/* CUP */
	case 'f':		/* HVP */
		term_place_cursor(sp, P(0, 1) - 1, P(1, 1) - 1);
		return;
	case 'd':		/* VPA */
		term_place_cursor(sp, P(0, 1) - 1, sp->cur_col);
		return;
	case 'J':		/* ED */
		term_erase_display(sp, P(0, 0));
		return;
	case 'K':		/* EL */
		switch (P(0, 0)) {
		case 0:
			term_erase_line(sp, sp->cur_row, sp->cur_col,
			    sp->cols - 1);
			break;
		case 1:
			term_erase_line(sp, sp->cur_row, 0, sp->cur_col);
			break;
		case 2:
			term_erase_line(sp, sp->cur_row, 0, sp->cols - 1);
			break;
		}
		return;
	case 'L':		/* IL */
		term_insert_lines(sp, P(0, 1));
		return;
	case 'M':		/* DL */
		term_delete_lines(sp, P(0, 1));
		return;
	case '@':		/* ICH */
		term_insert_chars(sp, P(0, 1));
		return;
	case 'P':		/* DCH */
		term_delete_chars(sp, P(0, 1));
		return;
	case 'S':		/* SU */
		term_scroll_up(sp, sp->scroll_top, sp->scroll_bot, P(0, 1));
		return;
	case 'T':		/* SD */
		term_scroll_down(sp, sp->scroll_top, sp->scroll_bot, P(0, 1));
		return;
	case 'r': {		/* DECSTBM */
		int top = P(0, 1) - 1;
		int bot = nparams > 1 ? P(1, sp->rows) - 1 : sp->rows - 1;
		if (top < 0)
			top = 0;
		if (bot >= sp->rows)
			bot = sp->rows - 1;
		if (top < bot) {
			sp->scroll_top = top;
			sp->scroll_bot = bot;
			term_place_cursor(sp, 0, 0);
		}
		return;
	}
	case 'm':		/* SGR -- strip */
		return;
	case 'h':		/* SM */
	case 'l':		/* RM */
	case 'n':		/* DSR -- ignore (host expects a reply we
				 * cannot inject cleanly) */
	case 'c':		/* DA -- ignore */
	case 'g':		/* TBC -- ignore */
	case 't':		/* window ops -- ignore */
		return;
	}
#undef P
}

/*
 * Return the buffer line at screen row `row`. If we somehow overran
 * the tail of the buffer, extend it in place -- callers count on this
 * always returning something usable.
 */
static struct line *
term_line_at(struct termproc *sp, int row)
{
	struct line *lp;
	struct buffer *bp = sp->bp;
	struct line *newlp;
	int i;

	if (row < 0)
		row = 0;
	lp = sp->home_lp;
	for (i = 0; i < row; i++) {
		if (lp->l_fp == bp->b_headp) {
			if ((newlp = lalloc(0)) == NULL)
				return (lp);
			newlp->l_used = 0;
			newlp->l_fp = bp->b_headp;
			newlp->l_bp = lp;
			lp->l_fp = newlp;
			bp->b_headp->l_bp = newlp;
			bp->b_lines++;
		}
		lp = lp->l_fp;
	}
	return (lp);
}

/*
 * Ensure lp has at least `col` bytes -- right-pad with spaces if not.
 * The terminal grid needs positional writes at any (row, col), so
 * every "column" must map to a real byte offset before we overwrite.
 */
static void
term_pad_line(struct line *lp, int col)
{
	int need = col - lp->l_used;
	int newsize;

	if (need <= 0)
		return;
	if (lp->l_used + need > lp->l_size) {
		newsize = lp->l_size == 0 ? 16 : lp->l_size * 2;
		while (newsize < lp->l_used + need)
			newsize *= 2;
		if (lrealloc(lp, newsize) == FALSE)
			return;
	}
	memset(lp->l_text + lp->l_used, ' ', need);
	lp->l_used += need;
}

/*
 * Place a printable byte at the cursor position, then advance.
 * Implements DEC-style pending wrap: if we're at column cols and
 * another char arrives, wrap-then-write instead of write-then-scroll.
 */
static void
term_put_char(struct termproc *sp, int c)
{
	struct line *lp;
	int newsize;

	if (sp->wrap_pending) {
		sp->cur_col = 0;
		term_newline(sp);
		sp->wrap_pending = 0;
	}

	lp = term_line_at(sp, sp->cur_row);
	term_pad_line(lp, sp->cur_col);

	if (sp->cur_col < lp->l_used) {
		lp->l_text[sp->cur_col] = (char)c;
	} else {
		if (lp->l_used + 1 > lp->l_size) {
			newsize = lp->l_size == 0 ? 16 : lp->l_size * 2;
			while (newsize < lp->l_used + 1)
				newsize *= 2;
			if (lrealloc(lp, newsize) == FALSE)
				return;
		}
		lp->l_text[lp->l_used++] = (char)c;
	}
	term_advance_cursor(sp);
}

static void
term_advance_cursor(struct termproc *sp)
{
	sp->cur_col++;
	if (sp->cur_col >= sp->cols) {
		sp->cur_col = sp->cols;	/* held at margin */
		sp->wrap_pending = 1;
	}
}

/*
 * LF/IND semantics: move down one row. At scroll_bot, scroll the
 * region up by 1 instead of moving past the region.
 */
static void
term_newline(struct termproc *sp)
{
	if (sp->cur_row == sp->scroll_bot) {
		term_scroll_up(sp, sp->scroll_top, sp->scroll_bot, 1);
		return;
	}
	if (sp->cur_row < sp->rows - 1)
		sp->cur_row++;
}

static void
term_reverse_index(struct termproc *sp)
{
	if (sp->cur_row == sp->scroll_top) {
		term_scroll_down(sp, sp->scroll_top, sp->scroll_bot, 1);
		return;
	}
	if (sp->cur_row > 0)
		sp->cur_row--;
}

/*
 * Scroll the region [top, bot] up by n lines. When the region covers
 * the whole screen we roll the top line into scrollback (advance
 * home_lp); otherwise we discard it (real terminals lose data outside
 * the scroll region).
 */
static void
term_scroll_up(struct termproc *sp, int top, int bot, int n)
{
	struct buffer *bp = sp->bp;
	struct line *first, *last, *newlp;
	int k;
	int full_screen;

	if (n <= 0 || top > bot)
		return;
	if (n > bot - top + 1)
		n = bot - top + 1;
	full_screen = (top == 0 && bot == sp->rows - 1);

	for (k = 0; k < n; k++) {
		first = term_line_at(sp, top);
		last  = term_line_at(sp, bot);

		if (full_screen) {
			/*
			 * Advance home_lp; the old top line becomes the
			 * newest scrollback entry above the screen.
			 */
			sp->home_lp = sp->home_lp->l_fp;
			sp->home_line++;

			/* Add a blank line at the (new) bottom. */
			if ((newlp = lalloc(0)) == NULL)
				return;
			newlp->l_used = 0;
			newlp->l_fp = last->l_fp;
			newlp->l_bp = last;
			last->l_fp->l_bp = newlp;
			last->l_fp = newlp;
			bp->b_lines++;
		} else {
			/*
			 * Detach `first` from its position, reinsert as a
			 * blank line after `last`. This shifts all lines
			 * between (top+1..bot) up by one row, and reuses
			 * the top line's allocation for the new bottom.
			 */
			first->l_bp->l_fp = first->l_fp;
			first->l_fp->l_bp = first->l_bp;

			first->l_used = 0;
			first->l_fp = last->l_fp;
			first->l_bp = last;
			last->l_fp->l_bp = first;
			last->l_fp = first;
		}
	}

	/*
	 * Fixup window and buffer dots. Any window on this buffer whose
	 * dot line is above the (new) home_lp needs its dotline count
	 * decremented on a full-screen scroll (dot didn't move, but the
	 * line numbers shifted). Rather than tracking each case, we let
	 * term_sync_windows re-anchor the dot on the emulator cursor,
	 * which is the only position we actually care about visually.
	 */
}

static void
term_scroll_down(struct termproc *sp, int top, int bot, int n)
{
	struct line *first, *last;
	int k;

	if (n <= 0 || top > bot)
		return;
	if (n > bot - top + 1)
		n = bot - top + 1;

	for (k = 0; k < n; k++) {
		first = term_line_at(sp, top);
		last  = term_line_at(sp, bot);

		/*
		 * Insert a blank line before `first`; drop `last`. The
		 * old last is unlinked and freed via reuse -- but simpler:
		 * unlink last, reinsert before first with used=0.
		 */
		if (first == last) {
			last->l_used = 0;
			continue;
		}

		last->l_bp->l_fp = last->l_fp;
		last->l_fp->l_bp = last->l_bp;

		last->l_used = 0;
		last->l_fp = first;
		last->l_bp = first->l_bp;
		first->l_bp->l_fp = last;
		first->l_bp = last;

		if (top == 0) {
			sp->home_lp = last;
		}
	}
}

/*
 * Erase columns [start, end] of the given screen row, in place.
 */
static void
term_erase_line(struct termproc *sp, int row, int start, int end)
{
	struct line *lp;
	int i;

	if (start < 0)
		start = 0;
	if (end >= sp->cols)
		end = sp->cols - 1;
	if (start > end)
		return;
	lp = term_line_at(sp, row);
	term_pad_line(lp, end + 1);
	for (i = start; i <= end && i < lp->l_used; i++)
		lp->l_text[i] = ' ';
	/*
	 * If the erase covers the tail of the line, trim trailing spaces.
	 * Keeps memory use bounded when apps repeatedly clear-to-EOL.
	 */
	if (end == sp->cols - 1) {
		while (lp->l_used > start && lp->l_text[lp->l_used - 1] == ' ')
			lp->l_used--;
	}
}

/*
 * ED: erase in display.
 *   0 -> cursor to end of screen
 *   1 -> beginning of screen to cursor
 *   2 -> whole screen
 */
static void
term_erase_display(struct termproc *sp, int mode)
{
	int r;

	switch (mode) {
	case 0:
		term_erase_line(sp, sp->cur_row, sp->cur_col, sp->cols - 1);
		for (r = sp->cur_row + 1; r < sp->rows; r++)
			term_erase_line(sp, r, 0, sp->cols - 1);
		break;
	case 1:
		for (r = 0; r < sp->cur_row; r++)
			term_erase_line(sp, r, 0, sp->cols - 1);
		term_erase_line(sp, sp->cur_row, 0, sp->cur_col);
		break;
	case 2:
	case 3:
		for (r = 0; r < sp->rows; r++)
			term_erase_line(sp, r, 0, sp->cols - 1);
		break;
	}
}

static void
term_insert_lines(struct termproc *sp, int n)
{
	if (sp->cur_row < sp->scroll_top || sp->cur_row > sp->scroll_bot)
		return;
	term_scroll_down(sp, sp->cur_row, sp->scroll_bot, n);
}

static void
term_delete_lines(struct termproc *sp, int n)
{
	if (sp->cur_row < sp->scroll_top || sp->cur_row > sp->scroll_bot)
		return;
	term_scroll_up(sp, sp->cur_row, sp->scroll_bot, n);
}

static void
term_insert_chars(struct termproc *sp, int n)
{
	struct line *lp;
	int room;

	if (n <= 0)
		return;
	if (n > sp->cols - sp->cur_col)
		n = sp->cols - sp->cur_col;
	lp = term_line_at(sp, sp->cur_row);
	term_pad_line(lp, sp->cur_col);

	room = sp->cols - sp->cur_col - n;
	if (room < 0)
		room = 0;

	if (lp->l_used > sp->cur_col + room)
		lp->l_used = sp->cur_col + room;

	term_pad_line(lp, sp->cur_col + room);

	if (lp->l_used + n > lp->l_size) {
		int newsize = lp->l_size == 0 ? 16 : lp->l_size * 2;
		while (newsize < lp->l_used + n)
			newsize *= 2;
		if (lrealloc(lp, newsize) == FALSE)
			return;
	}
	memmove(lp->l_text + sp->cur_col + n, lp->l_text + sp->cur_col,
	    lp->l_used - sp->cur_col);
	memset(lp->l_text + sp->cur_col, ' ', n);
	lp->l_used += n;
}

static void
term_delete_chars(struct termproc *sp, int n)
{
	struct line *lp;
	int keep;

	if (n <= 0)
		return;
	lp = term_line_at(sp, sp->cur_row);
	if (sp->cur_col >= lp->l_used)
		return;
	if (n > lp->l_used - sp->cur_col)
		n = lp->l_used - sp->cur_col;
	keep = lp->l_used - sp->cur_col - n;
	if (keep > 0)
		memmove(lp->l_text + sp->cur_col,
		    lp->l_text + sp->cur_col + n, keep);
	lp->l_used -= n;
}

static void
term_place_cursor(struct termproc *sp, int row, int col)
{
	if (row < 0)
		row = 0;
	if (row >= sp->rows)
		row = sp->rows - 1;
	if (col < 0)
		col = 0;
	if (col >= sp->cols)
		col = sp->cols - 1;
	sp->cur_row = row;
	sp->cur_col = col;
	sp->wrap_pending = 0;
}

/*
 * Snap every window on this buffer's dot to the emulator's cursor
 * position. In char mode we own the cursor -- the user cannot move it
 * with mg motion commands (those keys go to the PTY), so re-anchoring
 * here every refresh keeps the visible cursor accurate.
 */
static void
term_sync_windows(struct termproc *sp)
{
	struct buffer *bp = sp->bp;
	struct mgwin  *wp;
	struct line   *lp;
	int	       col;

	lp = term_line_at(sp, sp->cur_row);
	term_pad_line(lp, sp->cur_col);
	col = sp->cur_col;
	if (col > lp->l_used)
		col = lp->l_used;

	for (wp = wheadp; wp != NULL; wp = wp->w_wndp) {
		if (wp->w_bufp != bp)
			continue;
		/*
		 * Anchor the window's top-of-frame at the terminal's row 0
		 * (home_lp), not wherever mg's normal reframe would center
		 * it. Otherwise a `clear` erases the emulator's screen but
		 * mg keeps showing scrollback above -- so leftover output
		 * from top/less/etc. would still fill the top of the frame.
		 * With this anchor, scrollback lives strictly above the
		 * window; only the current terminal grid is on-screen.
		 */
		wp->w_linep = sp->home_lp;
		wp->w_dotp = lp;
		wp->w_doto = col;
		wp->w_dotline = sp->home_line + sp->cur_row;
		wp->w_rflag |= WFFULL | WFMOVE;
	}
	if (bp->b_nwnd == 0) {
		bp->b_dotp = lp;
		bp->b_doto = col;
		bp->b_dotline = sp->home_line + sp->cur_row;
	}
}

static void
term_touch_windows(struct buffer *bp)
{
	struct mgwin *wp;

	for (wp = wheadp; wp != NULL; wp = wp->w_wndp)
		if (wp->w_bufp == bp)
			wp->w_rflag |= WFFULL | WFMOVE;
}

static struct line *
term_bp_ensure_line(struct buffer *bp)
{
	struct line *lp;

	if (bp->b_headp->l_fp != bp->b_headp)
		return (bp->b_headp->l_fp);

	if ((lp = lalloc(0)) == NULL)
		return (NULL);
	lp->l_used = 0;
	lp->l_fp = bp->b_headp;
	lp->l_bp = bp->b_headp;
	bp->b_headp->l_fp = lp;
	bp->b_headp->l_bp = lp;
	bp->b_lines++;
	return (lp);
}

/*
 * Called from ttgetc(). Polls stdin plus every live term fd. Drains
 * whatever's available, redisplays, and loops back to poll(2) until
 * stdin has a keystroke queued or a signal fires.
 */
void
term_wait_for_input(void)
{
	struct pollfd	 pfds[1 + TERM_MAX_PROCS];
	struct termproc *sp, *sps[TERM_MAX_PROCS];
	int		 nfds, i, r, changed, rc;

	if (term_chld_flag)
		term_reap();
	if (terms == NULL)
		return;

	for (;;) {
		pfds[0].fd = STDIN_FILENO;
		pfds[0].events = POLLIN;
		pfds[0].revents = 0;
		nfds = 0;
		for (sp = terms;
		     sp != NULL && nfds < TERM_MAX_PROCS;
		     sp = sp->next) {
			if (sp->fd < 0)
				continue;
			sps[nfds] = sp;
			pfds[1 + nfds].fd = sp->fd;
			pfds[1 + nfds].events = POLLIN;
			pfds[1 + nfds].revents = 0;
			nfds++;
		}
		r = poll(pfds, 1 + nfds, -1);
		if (r == -1) {
			if (errno == EINTR) {
				if (term_chld_flag)
					term_reap();
				return;
			}
			return;
		}
		if (pfds[0].revents & (POLLIN | POLLHUP | POLLERR))
			return;
		changed = 0;
		for (i = 0; i < nfds; i++) {
			if (!(pfds[1 + i].revents &
			    (POLLIN | POLLHUP | POLLERR)))
				continue;
			rc = term_read(sps[i]);
			if (rc > 0)
				changed = 1;
			if (rc < 0) {
				if (sps[i]->fd >= 0) {
					close(sps[i]->fd);
					sps[i]->fd = -1;
				}
				changed = 1;
			}
		}
		if (term_chld_flag)
			term_reap();
		if (changed) {
			update(CMODE);
			ttflush();
		}
		if (terms == NULL)
			return;
	}
}

/*
 * Char-mode default handler. Writes the byte we were dispatched on
 * straight to the PTY. `key.k_chars[key.k_count - 1]` holds the byte
 * (mg's doin loop populated it just before dispatching us).
 */
int
term_send_char(int f, int n)
{
	struct termproc *sp;
	unsigned char	 c;
	ssize_t		 w;
	int		 count = n > 0 ? n : 1;
	int		 i;

	sp = term_find_by_bp(curbp);
	if (sp == NULL || sp->fd < 0)
		return (FALSE);
	if (key.k_count <= 0)
		return (FALSE);
	c = (unsigned char)key.k_chars[key.k_count - 1];

	for (i = 0; i < count; i++) {
		for (;;) {
			w = write(sp->fd, &c, 1);
			if (w == 1)
				break;
			if (w == -1 && errno == EINTR)
				continue;
			if (w == -1 &&
			    (errno == EAGAIN || errno == EWOULDBLOCK)) {
				struct pollfd pfd;
				pfd.fd = sp->fd;
				pfd.events = POLLOUT;
				pfd.revents = 0;
				(void)poll(&pfd, 1, 100);
				continue;
			}
			return (FALSE);
		}
	}
	return (TRUE);
}

int
term_interrupt(int f, int n)
{
	struct termproc *sp;
	unsigned char	 intr = 0x03;	/* C-c, VINTR */

	sp = term_find_by_bp(curbp);
	if (sp == NULL || sp->fd < 0)
		return (FALSE);
	/*
	 * Write the interrupt byte to the PTY master. The line
	 * discipline turns it into SIGINT for the foreground process
	 * group -- cleaner than kill(pid, SIGINT), which would only
	 * hit the shell and miss any child (e.g. vi) that's actually
	 * in the foreground.
	 */
	(void)write(sp->fd, &intr, 1);
	return (TRUE);
}

int
term_kill_process(int f, int n)
{
	struct termproc *sp;

	sp = term_find_by_bp(curbp);
	if (sp == NULL)
		return (FALSE);
	term_terminate(sp);
	term_finish_message(sp->bp);
	term_touch_windows(sp->bp);
	term_remove(sp);
	return (TRUE);
}

static void
term_terminate(struct termproc *sp)
{
	int i;

	if (sp->fd >= 0) {
		close(sp->fd);
		sp->fd = -1;
	}
	if (sp->pid <= 0)
		return;
	kill(sp->pid, SIGHUP);
	for (i = 0; i < 25; i++) {
		if (waitpid(sp->pid, NULL, WNOHANG) == sp->pid) {
			sp->pid = -1;
			return;
		}
		usleep(10 * 1000);
	}
	kill(sp->pid, SIGKILL);
	(void)waitpid(sp->pid, NULL, 0);
	sp->pid = -1;
}

void
term_buffer_killed(struct buffer *bp)
{
	struct termproc *sp;

	sp = term_find_by_bp(bp);
	if (sp == NULL)
		return;
	term_terminate(sp);
	term_remove(sp);
}

void
term_kill_all(void)
{
	struct termproc *sp, *next;

	for (sp = terms; sp != NULL; sp = next) {
		next = sp->next;
		term_terminate(sp);
		free(sp);
	}
	terms = NULL;
}

/*
 * Push new TIOCSWINSZ to each PTY whose window resized. We also
 * update the emulator's own rows/cols so cursor clamping tracks the
 * new grid; we do not re-flow existing content (curses apps redraw
 * on SIGWINCH anyway).
 */
void
term_notify_resize(void)
{
	struct termproc *sp;
	struct mgwin	*wp;
	struct winsize	 ws;
	unsigned short	 rows, cols;

	for (sp = terms; sp != NULL; sp = sp->next) {
		if (sp->fd < 0)
			continue;
		rows = 0;
		cols = 0;
		for (wp = wheadp; wp != NULL; wp = wp->w_wndp) {
			if (wp->w_bufp != sp->bp)
				continue;
			if ((unsigned short)wp->w_ntrows > rows)
				rows = (unsigned short)wp->w_ntrows;
			if ((unsigned short)wp->w_ncols > cols)
				cols = (unsigned short)wp->w_ncols;
		}
		if (rows == 0 || cols == 0)
			continue;
		if (rows == sp->rows && cols == sp->cols)
			continue;
		memset(&ws, 0, sizeof(ws));
		ws.ws_row = rows;
		ws.ws_col = cols;
		if (ioctl(sp->fd, TIOCSWINSZ, &ws) == 0) {
			sp->rows = rows;
			sp->cols = cols;
			if (sp->scroll_bot >= rows)
				sp->scroll_bot = rows - 1;
			if (sp->scroll_top >= sp->scroll_bot)
				sp->scroll_top = 0;
			if (sp->cur_row >= rows)
				sp->cur_row = rows - 1;
			if (sp->cur_col >= cols)
				sp->cur_col = cols - 1;
		}
	}
}
