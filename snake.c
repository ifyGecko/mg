/* This file is in the public domain. */

/*
 * M-x snake for mg.
 *
 * A small game of snake played in the "*Snake*" buffer, in the spirit
 * of the one that ships with GNU Emacs.  The game runs its own little
 * event loop: it waits for input with a timeout, and every time the
 * timer expires the snake moves one cell.  The board is redrawn by
 * rewriting the buffer and calling update().
 *
 * Controls:
 *   arrows, C-f/C-b/C-n/C-p, h/j/k/l, w/a/s/d	steer
 *   p or SPC					pause / resume
 *   q or C-g					quit
 *   r						new game (after game over)
 */

#include <sys/queue.h>
#include <sys/time.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "def.h"
#include "funmap.h"
#include "kbd.h"

#define SNAKE_BUFNAME	"*Snake*"

#define SNAKE_MAX_W	60	/* playfield size, excluding the border */
#define SNAKE_MAX_H	22
#define SNAKE_MIN_W	10
#define SNAKE_MIN_H	5

#define SNAKE_START_LEN	4
#define SNAKE_TICK0	150	/* initial milliseconds per move */
#define SNAKE_TICK_MIN	55
#define SNAKE_TICK_STEP	4	/* speed-up per food eaten */

#define SNAKE_QLEN	4	/* buffered direction changes */

#define CELL_EMPTY	' '
#define CELL_WALL	'#'
#define CELL_BODY	'o'
#define CELL_HEAD	'@'
#define CELL_FOOD	'*'

enum snake_dir { DIR_UP, DIR_DOWN, DIR_LEFT, DIR_RIGHT, DIR_NONE };

/* What a single key press means to the game. */
enum snake_cmd {
	CMD_NONE, CMD_UP, CMD_DOWN, CMD_LEFT, CMD_RIGHT,
	CMD_PAUSE, CMD_QUIT, CMD_RESTART
};

struct snake_game {
	int	 w, h;			/* playfield dimensions */
	char	*grid;			/* w * h cells */
	int	*xs, *ys;		/* ring buffer of body cells */
	int	 head, len, cap;	/* head index, length, capacity */
	enum snake_dir dir;
	enum snake_dir queue[SNAKE_QLEN];
	int	 qlen;
	int	 foodx, foody;
	int	 score;
	int	 tick;			/* ms per move */
	int	 paused;
	int	 over;
	int	 won;
};

static int	 snake_hiscore;

int		 snake(int, int);
void		 snake_init(void);

static int	 snake_alloc(struct snake_game *, int, int);
static void	 snake_free(struct snake_game *);
static void	 snake_reset(struct snake_game *);
static void	 snake_place_food(struct snake_game *);
static void	 snake_step(struct snake_game *);
static void	 snake_turn(struct snake_game *, enum snake_dir);
static int	 snake_render(struct snake_game *, struct buffer *);
static enum snake_cmd snake_readkey(void);
static int	 snake_poll(int);
static long	 snake_now(void);

#define CELL(g, x, y)	((g)->grid[(y) * (g)->w + (x)])

void
snake_init(void)
{
	funmap_add(snake, "snake", 0);
}

static long
snake_now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (ts.tv_sec * 1000L + ts.tv_nsec / 1000000L);
}

static int
snake_alloc(struct snake_game *g, int w, int h)
{
	memset(g, 0, sizeof(*g));
	g->w = w;
	g->h = h;
	g->cap = w * h;
	g->grid = malloc((size_t)g->cap);
	g->xs = calloc((size_t)g->cap, sizeof(int));
	g->ys = calloc((size_t)g->cap, sizeof(int));
	if (g->grid == NULL || g->xs == NULL || g->ys == NULL) {
		snake_free(g);
		return (FALSE);
	}
	return (TRUE);
}

static void
snake_free(struct snake_game *g)
{
	free(g->grid);
	free(g->xs);
	free(g->ys);
	g->grid = NULL;
	g->xs = g->ys = NULL;
}

static void
snake_reset(struct snake_game *g)
{
	int i, x0, y0;

	memset(g->grid, CELL_EMPTY, (size_t)g->cap);
	x0 = g->w / 2 - SNAKE_START_LEN / 2;
	y0 = g->h / 2;

	/* Lay the snake out horizontally, tail on the left, heading right. */
	g->len = SNAKE_START_LEN;
	for (i = 0; i < g->len; i++) {
		g->xs[i] = x0 + i;
		g->ys[i] = y0;
		CELL(g, x0 + i, y0) = CELL_BODY;
	}
	g->head = g->len - 1;
	CELL(g, g->xs[g->head], y0) = CELL_HEAD;

	g->dir = DIR_RIGHT;
	g->qlen = 0;
	g->score = 0;
	g->tick = SNAKE_TICK0;
	g->paused = 0;
	g->over = 0;
	g->won = 0;
	snake_place_food(g);
}

/*
 * Drop food on a random empty cell.  Picks the n'th empty cell so that
 * placement is uniform and never loops forever on a crowded board.
 */
static void
snake_place_food(struct snake_game *g)
{
	int i, free_cells = 0, pick;

	for (i = 0; i < g->cap; i++)
		if (g->grid[i] == CELL_EMPTY)
			free_cells++;
	if (free_cells == 0) {
		g->foodx = g->foody = -1;
		g->over = g->won = 1;
		return;
	}
	pick = (int)(random() % free_cells);
	for (i = 0; i < g->cap; i++) {
		if (g->grid[i] != CELL_EMPTY)
			continue;
		if (pick-- == 0) {
			g->grid[i] = CELL_FOOD;
			g->foodx = i % g->w;
			g->foody = i / g->w;
			return;
		}
	}
}

static int
snake_opposite(enum snake_dir a, enum snake_dir b)
{
	return ((a == DIR_UP && b == DIR_DOWN) ||
	    (a == DIR_DOWN && b == DIR_UP) ||
	    (a == DIR_LEFT && b == DIR_RIGHT) ||
	    (a == DIR_RIGHT && b == DIR_LEFT));
}

/*
 * Queue a direction change.  Compared against the last queued direction
 * (not the current one) so that quick double turns, e.g. up-then-left
 * within a single tick, are honoured instead of dropped.
 */
static void
snake_turn(struct snake_game *g, enum snake_dir d)
{
	enum snake_dir last;

	last = g->qlen > 0 ? g->queue[g->qlen - 1] : g->dir;
	if (d == last || snake_opposite(d, last))
		return;
	if (g->qlen < SNAKE_QLEN)
		g->queue[g->qlen++] = d;
}

static void
snake_step(struct snake_game *g)
{
	int hx, hy, nx, ny, tail, grow;

	if (g->qlen > 0) {
		g->dir = g->queue[0];
		memmove(g->queue, g->queue + 1,
		    (size_t)(g->qlen - 1) * sizeof(g->queue[0]));
		g->qlen--;
	}

	hx = g->xs[g->head];
	hy = g->ys[g->head];
	nx = hx;
	ny = hy;
	switch (g->dir) {
	case DIR_UP:	ny--; break;
	case DIR_DOWN:	ny++; break;
	case DIR_LEFT:	nx--; break;
	case DIR_RIGHT:	nx++; break;
	default:	return;
	}

	if (nx < 0 || ny < 0 || nx >= g->w || ny >= g->h) {
		g->over = 1;
		return;
	}

	grow = (nx == g->foodx && ny == g->foody);

	/* The tail moves out of the way first, so chasing it is legal. */
	if (!grow) {
		tail = (g->head - g->len + 1 + g->cap) % g->cap;
		CELL(g, g->xs[tail], g->ys[tail]) = CELL_EMPTY;
		g->len--;
	}

	if (CELL(g, nx, ny) == CELL_BODY || CELL(g, nx, ny) == CELL_HEAD) {
		g->over = 1;
		return;
	}

	CELL(g, hx, hy) = CELL_BODY;
	g->head = (g->head + 1) % g->cap;
	g->xs[g->head] = nx;
	g->ys[g->head] = ny;
	g->len++;
	CELL(g, nx, ny) = CELL_HEAD;

	if (grow) {
		g->score++;
		if (g->score > snake_hiscore)
			snake_hiscore = g->score;
		if (g->tick > SNAKE_TICK_MIN)
			g->tick -= SNAKE_TICK_STEP;
		g->foodx = g->foody = -1;
		snake_place_food(g);
	}
}

/*
 * Rewrite the *Snake* buffer with the current board and refresh the
 * screen.
 */
static int
snake_render(struct snake_game *g, struct buffer *bp)
{
	char	*row;
	int	 x, y;

	if ((row = malloc((size_t)g->w + 3)) == NULL)
		return (FALSE);

	bp->b_flag |= BFIGNDIRTY;
	if (bclear(bp) != TRUE) {
		free(row);
		return (FALSE);
	}

	addlinef(bp, " Score: %-5d Length: %-5d High score: %d",
	    g->score, g->len, snake_hiscore);

	memset(row, CELL_WALL, (size_t)g->w + 2);
	row[g->w + 2] = '\0';
	addline(bp, row);
	for (y = 0; y < g->h; y++) {
		row[0] = CELL_WALL;
		for (x = 0; x < g->w; x++)
			row[x + 1] = CELL(g, x, y);
		row[g->w + 1] = CELL_WALL;
		addline(bp, row);
	}
	memset(row, CELL_WALL, (size_t)g->w + 2);
	addline(bp, row);
	free(row);

	if (g->won)
		addline(bp, " You filled the board -- you win!  "
		    "r: new game  q: quit");
	else if (g->over)
		addline(bp, " Game over!  r: new game  q: quit");
	else if (g->paused)
		addline(bp, " Paused.  p/SPC: resume  q: quit");
	else
		addline(bp, " arrows/hjkl/wasd/C-npfb: steer  "
		    "p: pause  q: quit");

	bp->b_flag &= ~BFCHG;
	bp->b_dotp = bfirstlp(bp);
	bp->b_doto = 0;
	bp->b_dotline = 1;
	if (curwp->w_bufp == bp) {
		curwp->w_dotp = bfirstlp(bp);
		curwp->w_doto = 0;
		curwp->w_dotline = 1;
		curwp->w_linep = bfirstlp(bp);
		mark_clear_wp(curwp);
		curwp->w_rflag |= WFFULL | WFMODE;
	}
	update(CMODE);
	return (TRUE);
}

/*
 * Wait up to msec milliseconds for keyboard input.  Returns TRUE if a
 * key is ready.  Unlike ttwait(), a SIGWINCH does not make us block
 * waiting for a real key press.
 */
static int
snake_poll(int msec)
{
	struct pollfd pfd;
	int r;

	if (charswaiting() > 0)
		return (TRUE);
	pfd.fd = STDIN_FILENO;
	pfd.events = POLLIN;
	r = poll(&pfd, 1, msec < 0 ? 0 : msec);
	if (r == -1 && errno == EINTR) {
		if (winch_flag) {
			redraw(0, 0);
			winch_flag = 0;
		}
		return (FALSE);
	}
	return (r > 0);
}

/*
 * Read one key and translate it, decoding the ESC [ X / ESC O X arrow
 * key sequences by hand.
 */
static enum snake_cmd
snake_readkey(void)
{
	int c;

	c = getkey(FALSE);
	if (c == CCHR('[')) {
		if (!snake_poll(50))
			return (CMD_NONE);	/* lone ESC */
		c = getkey(FALSE);
		if (c != '[' && c != 'O')
			return (CMD_NONE);
		if (!snake_poll(50))
			return (CMD_NONE);
		c = getkey(FALSE);
		switch (c) {
		case 'A':	return (CMD_UP);
		case 'B':	return (CMD_DOWN);
		case 'C':	return (CMD_RIGHT);
		case 'D':	return (CMD_LEFT);
		default:	return (CMD_NONE);
		}
	}

	switch (c) {
	case CCHR('P'): case 'k': case 'w': case 'K': case 'W':
		return (CMD_UP);
	case CCHR('N'): case 'j': case 's': case 'J': case 'S':
		return (CMD_DOWN);
	case CCHR('B'): case 'h': case 'a': case 'H': case 'A':
		return (CMD_LEFT);
	case CCHR('F'): case 'l': case 'd': case 'L': case 'D':
		return (CMD_RIGHT);
	case 'p': case 'P': case ' ':
		return (CMD_PAUSE);
	case 'q': case 'Q': case CCHR('G'):
		return (CMD_QUIT);
	case 'r': case 'R':
		return (CMD_RESTART);
	default:
		return (CMD_NONE);
	}
}

/*
 * M-x snake: play snake in the *Snake* buffer until the player quits.
 */
int
snake(int f, int n)
{
	static int	 seeded;
	struct snake_game g;
	struct buffer	*bp;
	enum snake_cmd	 cmd;
	long		 deadline, now;
	int		 w, h, ret = TRUE;

	if (!seeded) {
		srandom((unsigned int)(time(NULL) ^ getpid()));
		seeded = 1;
	}

	if ((bp = bfind(SNAKE_BUFNAME, TRUE)) == NULL)
		return (FALSE);
	curbp = bp;
	if (showbuffer(bp, curwp, WFFRAME | WFFULL) != TRUE)
		return (FALSE);

	/* Fit the board to the window: border + score + help lines. */
	w = curwp->w_ncols - 3;
	h = curwp->w_ntrows - 4;
	if (w > SNAKE_MAX_W)
		w = SNAKE_MAX_W;
	if (h > SNAKE_MAX_H)
		h = SNAKE_MAX_H;
	if (w < SNAKE_MIN_W || h < SNAKE_MIN_H)
		return (dobeep_msg("Window too small for snake"));

	if (snake_alloc(&g, w, h) != TRUE)
		return (dobeep_msg("Out of memory"));
	snake_reset(&g);

	eerase();
	if (snake_render(&g, bp) != TRUE) {
		snake_free(&g);
		return (FALSE);
	}

	deadline = snake_now() + g.tick;
	for (;;) {
		if (g.over || g.paused) {
			/* Nothing moves; just block for the next key. */
			cmd = snake_readkey();
		} else {
			now = snake_now();
			if (now >= deadline || !snake_poll((int)(deadline -
			    now))) {
				snake_step(&g);
				deadline = snake_now() + g.tick;
				if (snake_render(&g, bp) != TRUE) {
					ret = FALSE;
					break;
				}
				continue;
			}
			cmd = snake_readkey();
		}

		switch (cmd) {
		case CMD_UP:
			snake_turn(&g, DIR_UP);
			break;
		case CMD_DOWN:
			snake_turn(&g, DIR_DOWN);
			break;
		case CMD_LEFT:
			snake_turn(&g, DIR_LEFT);
			break;
		case CMD_RIGHT:
			snake_turn(&g, DIR_RIGHT);
			break;
		case CMD_PAUSE:
			if (!g.over) {
				g.paused = !g.paused;
				deadline = snake_now() + g.tick;
				(void)snake_render(&g, bp);
			}
			break;
		case CMD_RESTART:
			if (g.over) {
				snake_reset(&g);
				deadline = snake_now() + g.tick;
				(void)snake_render(&g, bp);
			}
			break;
		case CMD_QUIT:
			goto done;
		default:
			break;
		}
	}
done:
	snake_free(&g);
	ewprintf("Snake: final score %d, high score %d", g.score,
	    snake_hiscore);
	return (ret);
}
