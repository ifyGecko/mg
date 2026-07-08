/* This file is in the public domain. */

/*
 * shell.c -- M-x shell for mg.
 *
 * A comint-style shell buffer: spawns $SHELL on a PTY with TERM=dumb,
 * drains its output asynchronously into a mg buffer, and writes
 * user-typed lines back on RET. Output arrives while mg is idle waiting
 * for a key -- ttgetc() calls shell_wait_for_input() to poll(2) stdin
 * plus every live shell fd together.
 *
 * Not a terminal emulator: SGR (colour) escapes are stripped; other
 * escape sequences pass through raw. For full-screen curses apps use
 * a future M-x term instead.
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

#define SHELL_BUFNAME       "*shell*"
#define SHELL_MAX_PROCS     16
#define SHELL_READ_BUFSIZE  4096
#define SHELL_CSI_BUFSIZE   32

extern int changemode(int, int, char *);

struct shellproc {
	pid_t              pid;
	int                fd;
	struct buffer     *bp;
	struct line       *mark_lp;	/* process-mark: where output goes */
	int                mark_off;
	int                mark_line;
	int                sgr_state;	/* 0=normal, 1=saw ESC, 2=in CSI */
	int                csi_len;
	char               csi_buf[SHELL_CSI_BUFSIZE];
	unsigned short     rows;	/* last winsize we pushed to the PTY */
	unsigned short     cols;
	struct shellproc  *next;
};

static struct shellproc *shells;
static volatile sig_atomic_t shell_chld_flag;
static int shell_initialized;

void	shell_init(void);
int	shell(int, int);
int	shell_send_input(int, int);
int	shell_interrupt(int, int);
int	shell_send_eof(int, int);
void	shell_wait_for_input(void);
void	shell_buffer_killed(struct buffer *);
void	shell_kill_all(void);
void	shell_notify_resize(void);

static void shell_sigchld(int);
static void shell_reap(void);
static void shell_remove(struct shellproc *);
static struct shellproc *shell_find_by_bp(struct buffer *);
static struct shellproc *shell_find_by_pid(pid_t);
static int  shell_read(struct shellproc *);
static void shell_process_byte(struct shellproc *, unsigned char);
static void shell_emit_char(struct shellproc *, int);
static int  shell_line_reachable(struct buffer *, struct line *);
static void shell_reset_mark_eob(struct shellproc *);
static struct line *shell_bp_ensure_line(struct buffer *);
static void shell_touch_windows(struct buffer *);
static void shell_finish_message(struct buffer *);

/*
 * Shell-mode keymap.
 *
 *   RET        -> shell-send-input
 *   C-c C-c    -> shell-interrupt
 *   C-c C-d    -> shell-send-eof
 *
 * C-c is a prefix; the range C-c..C-m puts the send-input binding on
 * RET (C-m) and hands C-c off to the sub-keymap below.
 */

static PF shell_cc_subs[] = {
	shell_interrupt,	/* C-c C-c */
	shell_send_eof,		/* C-c C-d */
};

static struct KEYMAPE (1) shell_cc_map = {
	1, 1, rescan,
	{
		{ CCHR('C'), CCHR('D'), shell_cc_subs, NULL }
	}
};

static PF shell_range[] = {
	NULL,			/* C-c (prefix into shell_cc_map) */
	rescan,			/* C-d */
	rescan,			/* C-e */
	rescan,			/* C-f */
	rescan,			/* C-g */
	rescan,			/* C-h */
	rescan,			/* C-i */
	rescan,			/* C-j */
	rescan,			/* C-k */
	rescan,			/* C-l */
	shell_send_input,	/* C-m (RET) */
};

static struct KEYMAPE (1) shellmap = {
	1, 1, rescan,
	{
		{ CCHR('C'), CCHR('M'), shell_range, (KEYMAP *)&shell_cc_map }
	}
};

void
shell_init(void)
{
	struct sigaction sa;

	if (shell_initialized)
		return;
	shell_initialized = 1;

	funmap_add(shell, "shell", 0);
	funmap_add(shell_send_input, "shell-send-input", 0);
	funmap_add(shell_interrupt, "shell-interrupt", 0);
	funmap_add(shell_send_eof, "shell-send-eof", 0);
	maps_add((KEYMAP *)&shellmap, "shell");

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = shell_sigchld;
	sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
	sigemptyset(&sa.sa_mask);
	(void)sigaction(SIGCHLD, &sa, NULL);
}

static void
shell_sigchld(int signo)
{
	(void)signo;
	shell_chld_flag = 1;
}

/*
 * M-x shell -- start (or switch to) an interactive shell in the current
 * window. Reuses the *shell* buffer when an existing shell is still alive.
 */
int
shell(int f, int n)
{
	struct buffer	*bp, *saved_bp;
	struct shellproc *sp;
	struct winsize	 ws;
	int		 master;
	pid_t		 pid;
	char		*shellp, *argv0;
	unsigned short	 rows, cols;

	if ((bp = bfind(SHELL_BUFNAME, FALSE)) != NULL) {
		sp = shell_find_by_bp(bp);
		if (sp != NULL) {
			showbuffer(bp, curwp, WFFRAME | WFFULL);
			curbp = bp;
			return (TRUE);
		}
		if (bclear(bp) != TRUE)
			return (FALSE);
	} else {
		bp = bfind(SHELL_BUFNAME, TRUE);
		if (bp == NULL)
			return (FALSE);
	}

	saved_bp = curbp;
	curbp = bp;
	(void)changemode(FFARG, 1, "shell");
	curbp = saved_bp;

	showbuffer(bp, curwp, WFFRAME | WFFULL);
	curbp = bp;

	if ((shellp = getenv("SHELL")) == NULL || shellp[0] == '\0')
		shellp = _PATH_BSHELL;
	argv0 = strrchr(shellp, '/');
	if (argv0 != NULL)
		argv0++;
	else
		argv0 = shellp;

	rows = curwp->w_ntrows > 0 ? (unsigned short)curwp->w_ntrows : 24;
	cols = curwp->w_ncols  > 0 ? (unsigned short)curwp->w_ncols  : 80;
	memset(&ws, 0, sizeof(ws));
	ws.ws_row = rows;
	ws.ws_col = cols;

	pid = forkpty(&master, NULL, NULL, &ws);
	if (pid == -1) {
		dobeep();
		ewprintf("shell: forkpty: %s", strerror(errno));
		return (FALSE);
	}

	if (pid == 0) {
		struct termios tios;

		/*
		 * We drive line input ourselves and write whole lines from
		 * RET, so turn off the tty's echo and canonical processing.
		 * Leaving them on would double every keystroke (mg's insert
		 * + kernel echo) and buffer our writes until a newline in
		 * the wrong direction.
		 */
		if (tcgetattr(STDIN_FILENO, &tios) == 0) {
			tios.c_lflag &= ~(ECHO | ECHOE | ECHOK | ECHONL |
			    ICANON);
			tios.c_iflag &= ~(ICRNL | INLCR | IXON);
			tios.c_oflag &= ~ONLCR;
			tios.c_cc[VMIN]  = 1;
			tios.c_cc[VTIME] = 0;
			(void)tcsetattr(STDIN_FILENO, TCSANOW, &tios);
		}
		setenv("TERM", "dumb", 1);
		setenv("PAGER", "cat", 1);
		setenv("INSIDE_EMACS", "mg", 1);
		signal(SIGCHLD, SIG_DFL);
		signal(SIGINT,  SIG_DFL);
		signal(SIGQUIT, SIG_DFL);
		signal(SIGPIPE, SIG_DFL);
		execl(shellp, argv0, "-i", (char *)NULL);
		_exit(127);
	}

	fcntl(master, F_SETFL, O_NONBLOCK);

	if ((sp = calloc(1, sizeof(*sp))) == NULL) {
		dobeep();
		ewprintf("shell: out of memory");
		kill(pid, SIGHUP);
		close(master);
		return (FALSE);
	}
	sp->pid       = pid;
	sp->fd        = master;
	sp->bp        = bp;
	sp->mark_lp   = shell_bp_ensure_line(bp);
	sp->mark_off  = sp->mark_lp->l_used;
	sp->mark_line = bp->b_lines;
	sp->sgr_state = 0;
	sp->csi_len   = 0;
	sp->rows      = rows;
	sp->cols      = cols;
	sp->next      = shells;
	shells        = sp;

	ewprintf("Started %s (pid %d)", shellp, (int)pid);
	return (TRUE);
}

static struct shellproc *
shell_find_by_bp(struct buffer *bp)
{
	struct shellproc *sp;

	for (sp = shells; sp != NULL; sp = sp->next)
		if (sp->bp == bp)
			return (sp);
	return (NULL);
}

static struct shellproc *
shell_find_by_pid(pid_t pid)
{
	struct shellproc *sp;

	for (sp = shells; sp != NULL; sp = sp->next)
		if (sp->pid == pid)
			return (sp);
	return (NULL);
}

static void
shell_remove(struct shellproc *sp)
{
	struct shellproc **pp;

	for (pp = &shells; *pp != NULL; pp = &(*pp)->next) {
		if (*pp == sp) {
			*pp = sp->next;
			free(sp);
			return;
		}
	}
}

/*
 * Reap any children flagged by shell_sigchld. For each of our shells that
 * died, drain remaining bytes from its fd, close it, and drop a "finished"
 * marker into the buffer.
 */
static void
shell_reap(void)
{
	int		  status;
	pid_t		  pid;
	struct shellproc *sp;

	shell_chld_flag = 0;
	while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
		sp = shell_find_by_pid(pid);
		if (sp == NULL)
			continue;
		(void)shell_read(sp);
		if (sp->fd >= 0) {
			close(sp->fd);
			sp->fd = -1;
		}
		shell_finish_message(sp->bp);
		shell_touch_windows(sp->bp);
		shell_remove(sp);
	}
}

static void
shell_finish_message(struct buffer *bp)
{
	addlinef(bp, "");
	addlinef(bp, "Process shell finished");
}

/*
 * Read whatever is currently available on sp->fd and feed each byte
 * through the SGR-strip state machine. Returns 1 if buffer changed, 0
 * for a benign EAGAIN, -1 for EOF/error (caller should close and remove).
 */
static int
shell_read(struct shellproc *sp)
{
	char	 buf[SHELL_READ_BUFSIZE];
	ssize_t	 n;
	int	 i, changed = 0;

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

	if (!shell_line_reachable(sp->bp, sp->mark_lp))
		shell_reset_mark_eob(sp);

	for (i = 0; i < n; i++) {
		shell_process_byte(sp, (unsigned char)buf[i]);
		changed = 1;
	}
	shell_touch_windows(sp->bp);
	return (changed);
}

/*
 * Drive the SGR/CSI stripping state machine one byte at a time.
 * "Strip SGR only" means we swallow ESC[...m sequences entirely but
 * emit any other CSI (or bare ESC) as-is.
 */
static void
shell_process_byte(struct shellproc *sp, unsigned char c)
{
	int k;

	switch (sp->sgr_state) {
	case 0:
		if (c == 0x1B) {
			sp->sgr_state = 1;
			return;
		}
		if (c == '\r')
			return;
		shell_emit_char(sp, c);
		return;
	case 1:
		if (c == '[') {
			sp->sgr_state = 2;
			sp->csi_len = 0;
			return;
		}
		/* Not CSI: emit the swallowed ESC, then reprocess c. */
		sp->sgr_state = 0;
		shell_emit_char(sp, 0x1B);
		if (c == 0x1B) {
			sp->sgr_state = 1;
			return;
		}
		if (c == '\r')
			return;
		shell_emit_char(sp, c);
		return;
	case 2:
		if (c >= 0x40 && c <= 0x7E) {
			sp->sgr_state = 0;
			if (c == 'm')
				return;
			shell_emit_char(sp, 0x1B);
			shell_emit_char(sp, '[');
			for (k = 0; k < sp->csi_len; k++)
				shell_emit_char(sp,
				    (unsigned char)sp->csi_buf[k]);
			shell_emit_char(sp, c);
			return;
		}
		if (sp->csi_len < (int)sizeof(sp->csi_buf))
			sp->csi_buf[sp->csi_len++] = (char)c;
		else
			sp->sgr_state = 0;	/* runaway; give up */
		return;
	}
}

/*
 * Insert one character (byte or newline) at the process-mark, advance
 * the mark past it, and shift any window/buffer dot/mark that was sitting
 * past our insertion point. This is the low-level primitive that lets
 * background shell output arrive without disturbing user-typed input
 * that is queued past the process-mark.
 */
static void
shell_emit_char(struct shellproc *sp, int c)
{
	struct buffer *bp = sp->bp;
	struct line   *lp;
	struct line   *newlp;
	struct mgwin  *wp;
	int	       old_mark_off;
	int	       tail_used;
	int	       newsize;

	if (sp->mark_lp == bp->b_headp) {
		lp = shell_bp_ensure_line(bp);
		if (lp == NULL || lp == bp->b_headp)
			return;
		sp->mark_lp = lp;
		sp->mark_off = 0;
	}
	lp = sp->mark_lp;

	/*
	 * If any window on this buffer has its dot sitting on the EOB
	 * marker (b_headp), snap it down onto the current mark. Otherwise
	 * user keystrokes after unnoticed background output would land on
	 * a fresh line past our prompt, orphaning the input from the mark.
	 */
	for (wp = wheadp; wp != NULL; wp = wp->w_wndp) {
		if (wp->w_bufp == bp && wp->w_dotp == bp->b_headp) {
			wp->w_dotp = sp->mark_lp;
			wp->w_doto = sp->mark_off;
			wp->w_dotline = sp->mark_line;
		}
	}

	if (c == '\n') {
		old_mark_off = sp->mark_off;
		tail_used = lp->l_used - old_mark_off;
		if (tail_used < 0)
			tail_used = 0;
		newlp = lalloc(tail_used > 0 ? tail_used : 0);
		if (newlp == NULL)
			return;
		if (tail_used > 0)
			memcpy(newlp->l_text,
			    lp->l_text + old_mark_off, tail_used);
		newlp->l_used = tail_used;
		lp->l_used = old_mark_off;

		newlp->l_fp = lp->l_fp;
		newlp->l_bp = lp;
		lp->l_fp->l_bp = newlp;
		lp->l_fp = newlp;
		bp->b_lines++;

		for (wp = wheadp; wp != NULL; wp = wp->w_wndp) {
			if (wp->w_bufp != bp)
				continue;
			if (wp->w_dotp == lp && wp->w_doto >= old_mark_off) {
				wp->w_doto -= old_mark_off;
				wp->w_dotp = newlp;
				wp->w_dotline++;
			}
			if (wp->w_markp == lp && wp->w_marko >= old_mark_off) {
				wp->w_marko -= old_mark_off;
				wp->w_markp = newlp;
				wp->w_markline++;
			}
		}
		if (bp->b_nwnd == 0) {
			if (bp->b_dotp == lp && bp->b_doto >= old_mark_off) {
				bp->b_doto -= old_mark_off;
				bp->b_dotp = newlp;
				bp->b_dotline++;
			}
			if (bp->b_markp == lp && bp->b_marko >= old_mark_off) {
				bp->b_marko -= old_mark_off;
				bp->b_markp = newlp;
				bp->b_markline++;
			}
		}

		sp->mark_lp = newlp;
		sp->mark_off = 0;
		sp->mark_line++;
		return;
	}

	if (lp->l_used + 1 > lp->l_size) {
		newsize = lp->l_size == 0 ? 16 : lp->l_size * 2;
		while (newsize < lp->l_used + 1)
			newsize *= 2;
		if (lrealloc(lp, newsize) == FALSE)
			return;
	}
	if (sp->mark_off < lp->l_used)
		memmove(lp->l_text + sp->mark_off + 1,
		    lp->l_text + sp->mark_off,
		    lp->l_used - sp->mark_off);
	lp->l_text[sp->mark_off] = (char)c;
	lp->l_used++;

	for (wp = wheadp; wp != NULL; wp = wp->w_wndp) {
		if (wp->w_bufp != bp)
			continue;
		if (wp->w_dotp == lp && wp->w_doto >= sp->mark_off)
			wp->w_doto++;
		if (wp->w_markp == lp && wp->w_marko >= sp->mark_off)
			wp->w_marko++;
	}
	if (bp->b_nwnd == 0) {
		if (bp->b_dotp == lp && bp->b_doto >= sp->mark_off)
			bp->b_doto++;
		if (bp->b_markp == lp && bp->b_marko >= sp->mark_off)
			bp->b_marko++;
	}
	sp->mark_off++;
}

/*
 * A fresh or cleared buffer has b_headp -> b_headp (self-loop, no real
 * lines). Fabricate a first empty line so we always have somewhere to
 * insert. Returns the (new or existing) last real line.
 */
static struct line *
shell_bp_ensure_line(struct buffer *bp)
{
	struct line *lp;

	if (bp->b_headp->l_fp != bp->b_headp)
		return (bp->b_headp->l_bp);

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

static int
shell_line_reachable(struct buffer *bp, struct line *target)
{
	struct line *lp = bp->b_headp;

	if (target == bp->b_headp)
		return (1);
	do {
		if (lp == target)
			return (1);
		lp = lforw(lp);
	} while (lp != bp->b_headp);
	return (0);
}

static void
shell_reset_mark_eob(struct shellproc *sp)
{
	struct buffer *bp = sp->bp;

	sp->mark_lp = bp->b_headp->l_bp;
	sp->mark_off = sp->mark_lp == bp->b_headp ? 0 : sp->mark_lp->l_used;
	sp->mark_line = bp->b_lines;
}

static void
shell_touch_windows(struct buffer *bp)
{
	struct mgwin *wp;

	for (wp = wheadp; wp != NULL; wp = wp->w_wndp)
		if (wp->w_bufp == bp)
			wp->w_rflag |= WFFULL | WFMOVE;
}

/*
 * Called from ttgetc(). Polls stdin plus every live shell fd. When a
 * shell fd is readable we drain it, redisplay, and go back to poll(2).
 * Returns as soon as stdin has a character queued, or on EINTR so the
 * caller can react to SIGWINCH.
 */
void
shell_wait_for_input(void)
{
	struct pollfd	  pfds[1 + SHELL_MAX_PROCS];
	struct shellproc *sp, *sps[SHELL_MAX_PROCS];
	int		  nfds, i, r, changed, rc;

	if (shell_chld_flag)
		shell_reap();
	if (shells == NULL)
		return;

	for (;;) {
		pfds[0].fd = STDIN_FILENO;
		pfds[0].events = POLLIN;
		pfds[0].revents = 0;
		nfds = 0;
		for (sp = shells;
		     sp != NULL && nfds < SHELL_MAX_PROCS;
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
				if (shell_chld_flag)
					shell_reap();
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
			rc = shell_read(sps[i]);
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
		if (shell_chld_flag)
			shell_reap();
		if (changed) {
			update(CMODE);
			ttflush();
		}
		if (shells == NULL)
			return;
	}
}

/*
 * RET binding in shell-mode. Sends everything between the process-mark
 * and end-of-buffer to the shell's stdin, terminated by \n, then drops
 * a fresh newline into the buffer and re-anchors the process-mark at
 * the new end-of-buffer.
 */
int
shell_send_input(int f, int n)
{
	struct shellproc *sp;
	struct line	 *lp;
	int		  off;
	char		 *text = NULL;
	size_t		  tlen = 0, cap = 0;
	ssize_t		  w;
	size_t		  sent;
	int		  need;

	sp = shell_find_by_bp(curbp);
	if (sp == NULL || sp->fd < 0)
		return (lnewline());

	if (!shell_line_reachable(sp->bp, sp->mark_lp))
		shell_reset_mark_eob(sp);

	lp = sp->mark_lp;
	off = sp->mark_off;
	while (lp != sp->bp->b_headp) {
		need = lp->l_used - off + 1;
		if (tlen + (size_t)need > cap) {
			size_t newcap = cap == 0 ? 256 : cap * 2;
			char *nt;
			while (newcap < tlen + (size_t)need)
				newcap *= 2;
			nt = realloc(text, newcap);
			if (nt == NULL) {
				free(text);
				dobeep();
				ewprintf("shell-send-input: out of memory");
				return (FALSE);
			}
			text = nt;
			cap = newcap;
		}
		if (lp->l_used - off > 0) {
			memcpy(text + tlen, lp->l_text + off,
			    lp->l_used - off);
			tlen += lp->l_used - off;
		}
		lp = lforw(lp);
		if (lp != sp->bp->b_headp)
			text[tlen++] = '\n';
		off = 0;
	}

	if (tlen + 1 > cap) {
		char *nt = realloc(text, tlen + 1);
		if (nt == NULL) {
			free(text);
			dobeep();
			ewprintf("shell-send-input: out of memory");
			return (FALSE);
		}
		text = nt;
		cap = tlen + 1;
	}
	text[tlen++] = '\n';

	curwp->w_dotp = blastlp(sp->bp);
	curwp->w_doto = llength(curwp->w_dotp);
	curwp->w_dotline = sp->bp->b_lines;
	curwp->w_rflag |= WFMOVE;
	if (lnewline() == FALSE) {
		free(text);
		return (FALSE);
	}
	sp->mark_lp = curwp->w_dotp;
	sp->mark_off = curwp->w_doto;
	sp->mark_line = curwp->w_dotline;

	sent = 0;
	while (sent < tlen) {
		w = write(sp->fd, text + sent, tlen - sent);
		if (w == -1) {
			struct pollfd pfd;

			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				pfd.fd = sp->fd;
				pfd.events = POLLOUT;
				pfd.revents = 0;
				(void)poll(&pfd, 1, 100);
				continue;
			}
			dobeep();
			ewprintf("shell-send-input: write: %s",
			    strerror(errno));
			break;
		}
		sent += (size_t)w;
	}
	free(text);
	return (TRUE);
}

int
shell_interrupt(int f, int n)
{
	struct shellproc *sp;

	sp = shell_find_by_bp(curbp);
	if (sp == NULL || sp->pid <= 0)
		return (FALSE);
	if (kill(sp->pid, SIGINT) == -1) {
		dobeep();
		ewprintf("shell-interrupt: kill: %s", strerror(errno));
		return (FALSE);
	}
	return (TRUE);
}

int
shell_send_eof(int f, int n)
{
	struct shellproc *sp;
	char		  eof = 0x04;	/* C-d, VEOF */

	sp = shell_find_by_bp(curbp);
	if (sp == NULL || sp->fd < 0)
		return (FALSE);
	(void)write(sp->fd, &eof, 1);
	return (TRUE);
}

/*
 * Send SIGHUP, close our end, and reap. If SIGHUP alone doesn't shake the
 * child loose within ~250ms (bash mid-.bashrc, for example), escalate to
 * SIGKILL rather than leave a zombie.
 */
static void
shell_terminate(struct shellproc *sp)
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
shell_buffer_killed(struct buffer *bp)
{
	struct shellproc *sp;

	sp = shell_find_by_bp(bp);
	if (sp == NULL)
		return;
	shell_terminate(sp);
	shell_remove(sp);
}

void
shell_kill_all(void)
{
	struct shellproc *sp, *next;

	for (sp = shells; sp != NULL; sp = next) {
		next = sp->next;
		shell_terminate(sp);
		free(sp);
	}
	shells = NULL;
}

/*
 * For each live shell whose window has changed dimensions since we last
 * pushed a TIOCSWINSZ, re-issue one. Called after any operation that
 * could resize a window: SIGWINCH redraw, splitwind, enlargewind,
 * shrinkwind, delwind, onlywind.
 */
void
shell_notify_resize(void)
{
	struct shellproc *sp;
	struct mgwin	 *wp;
	struct winsize	  ws;
	unsigned short	  rows, cols;

	for (sp = shells; sp != NULL; sp = sp->next) {
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
		}
	}
}
