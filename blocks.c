/* This file is in the public domain. */

/*
 * M-x blocks for mg.
 *
 * A falling-block puzzle played in the "*Blocks*" buffer.  Pieces made
 * of four cells drop into a 10-wide well; fill a row completely and it
 * is cleared.  Like M-x snake, the game runs its own event loop: input
 * is polled with a timeout, gravity moves the piece down when the
 * timer expires, and the board is redrawn by rewriting the buffer.
 *
 * Controls:
 *   left/right, C-b/C-f, h/l, a/d	move
 *   up, C-p, k, w, x			rotate clockwise
 *   z					rotate counter-clockwise
 *   down, C-n, j, s			soft drop
 *   SPC				hard drop
 *   p					pause / resume
 *   q or C-g				quit
 *   r					new game (after game over)
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

#define BLK_BUFNAME	"*Blocks*"

#define BLK_W		10	/* well width in cells */
#define BLK_MAX_H	20	/* well height in cells */
#define BLK_MIN_H	12
#define BLK_NTYPES	7

#define BLK_GRAV0	800	/* ms per row at level 1 */
#define BLK_GRAV_STEP	70	/* faster by this much per level */
#define BLK_GRAV_MIN	80

enum blk_cmd {
	BC_NONE, BC_LEFT, BC_RIGHT, BC_ROT_CW, BC_ROT_CCW, BC_SOFT,
	BC_HARD, BC_PAUSE, BC_QUIT, BC_RESTART
};

struct blk_piece {
	int	type;		/* 0..BLK_NTYPES-1 */
	int	rot;		/* 0..3 */
	int	x, y;		/* top-left of the piece's box in the well */
};

struct blk_game {
	int		 h;		/* well height */
	unsigned char	 well[BLK_MAX_H][BLK_W];	/* 0 empty, else type+1 */
	struct blk_piece cur;
	int		 next;
	int		 bag[BLK_NTYPES];
	int		 bagn;		/* pieces left in bag */
	int		 score, lines, level;
	int		 paused, over;
};

/*
 * Piece shapes in spawn orientation, as (x, y) cells inside an n-by-n
 * box.  The other three orientations are derived by rotating the box.
 */
static const struct {
	int	n;
	int	c[4][2];
	char	name;
} blk_shapes[BLK_NTYPES] = {
	{ 4, { {0, 1}, {1, 1}, {2, 1}, {3, 1} }, 'I' },
	{ 2, { {0, 0}, {1, 0}, {0, 1}, {1, 1} }, 'O' },
	{ 3, { {1, 0}, {0, 1}, {1, 1}, {2, 1} }, 'T' },
	{ 3, { {1, 0}, {2, 0}, {0, 1}, {1, 1} }, 'S' },
	{ 3, { {0, 0}, {1, 0}, {1, 1}, {2, 1} }, 'Z' },
	{ 3, { {0, 0}, {0, 1}, {1, 1}, {2, 1} }, 'J' },
	{ 3, { {2, 0}, {0, 1}, {1, 1}, {2, 1} }, 'L' },
};

/* blk_cells[type][rot][cell] = {x, y}; filled in by blk_init_shapes(). */
static int	 blk_cells[BLK_NTYPES][4][4][2];
static int	 blk_hiscore;

int		 blocks(int, int);
void		 blocks_init(void);

static void	 blk_init_shapes(void);
static int	 blk_draw_from_bag(struct blk_game *);
static void	 blk_reset(struct blk_game *, int);
static int	 blk_fits(struct blk_game *, struct blk_piece *);
static int	 blk_spawn(struct blk_game *);
static int	 blk_move(struct blk_game *, int, int);
static void	 blk_rotate(struct blk_game *, int);
static void	 blk_lock(struct blk_game *);
static int	 blk_gravity_ms(struct blk_game *);
static int	 blk_render(struct blk_game *, struct buffer *);
static enum blk_cmd blk_readkey(void);
static int	 blk_poll(int);
static long	 blk_now(void);

void
blocks_init(void)
{
	funmap_add(blocks, "blocks", 0);
	blk_init_shapes();
}

static void
blk_init_shapes(void)
{
	int t, r, i, n, x, y, nx;

	for (t = 0; t < BLK_NTYPES; t++) {
		n = blk_shapes[t].n;
		for (i = 0; i < 4; i++) {
			blk_cells[t][0][i][0] = blk_shapes[t].c[i][0];
			blk_cells[t][0][i][1] = blk_shapes[t].c[i][1];
		}
		/* Rotate the box clockwise: (x, y) -> (n-1-y, x). */
		for (r = 1; r < 4; r++) {
			for (i = 0; i < 4; i++) {
				x = blk_cells[t][r - 1][i][0];
				y = blk_cells[t][r - 1][i][1];
				nx = n - 1 - y;
				blk_cells[t][r][i][0] = nx;
				blk_cells[t][r][i][1] = x;
			}
		}
	}
}

static long
blk_now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (ts.tv_sec * 1000L + ts.tv_nsec / 1000000L);
}

/*
 * Hand out pieces from a shuffled bag of all seven, so droughts of any
 * one piece are bounded.
 */
static int
blk_draw_from_bag(struct blk_game *g)
{
	int i, j, t;

	if (g->bagn == 0) {
		for (i = 0; i < BLK_NTYPES; i++)
			g->bag[i] = i;
		for (i = BLK_NTYPES - 1; i > 0; i--) {
			j = (int)(random() % (i + 1));
			t = g->bag[i];
			g->bag[i] = g->bag[j];
			g->bag[j] = t;
		}
		g->bagn = BLK_NTYPES;
	}
	return (g->bag[--g->bagn]);
}

static void
blk_reset(struct blk_game *g, int h)
{
	memset(g, 0, sizeof(*g));
	g->h = h;
	g->level = 1;
	g->next = blk_draw_from_bag(g);
	(void)blk_spawn(g);
}

/*
 * Does the piece fit at its position?  Cells above the top of the well
 * (y < 0) are allowed so the I piece can spawn and rotate there.
 */
static int
blk_fits(struct blk_game *g, struct blk_piece *p)
{
	int i, x, y;

	for (i = 0; i < 4; i++) {
		x = p->x + blk_cells[p->type][p->rot][i][0];
		y = p->y + blk_cells[p->type][p->rot][i][1];
		if (x < 0 || x >= BLK_W || y >= g->h)
			return (FALSE);
		if (y >= 0 && g->well[y][x] != 0)
			return (FALSE);
	}
	return (TRUE);
}

static int
blk_spawn(struct blk_game *g)
{
	g->cur.type = g->next;
	g->cur.rot = 0;
	g->cur.x = (BLK_W - blk_shapes[g->cur.type].n) / 2;
	/* The I piece sits on row 1 of its box; lift it to the top. */
	g->cur.y = (g->cur.type == 0) ? -1 : 0;
	g->next = blk_draw_from_bag(g);
	if (!blk_fits(g, &g->cur)) {
		g->over = 1;
		return (FALSE);
	}
	return (TRUE);
}

static int
blk_move(struct blk_game *g, int dx, int dy)
{
	struct blk_piece p = g->cur;

	p.x += dx;
	p.y += dy;
	if (!blk_fits(g, &p))
		return (FALSE);
	g->cur = p;
	return (TRUE);
}

/*
 * Rotate, nudging sideways (and up, for pieces jammed against the
 * floor) when the plain rotation would overlap something.
 */
static void
blk_rotate(struct blk_game *g, int dir)
{
	static const int kicks[][2] = {
		{0, 0}, {-1, 0}, {1, 0}, {-2, 0}, {2, 0}, {0, -1}
	};
	struct blk_piece p;
	size_t i;

	if (g->cur.type == 1)		/* O does not rotate */
		return;
	for (i = 0; i < sizeof(kicks) / sizeof(kicks[0]); i++) {
		p = g->cur;
		p.rot = (p.rot + (dir > 0 ? 1 : 3)) % 4;
		p.x += kicks[i][0];
		p.y += kicks[i][1];
		if (blk_fits(g, &p)) {
			g->cur = p;
			return;
		}
	}
}

/*
 * Freeze the current piece into the well, clear full rows, score them
 * and bring in the next piece.
 */
static void
blk_lock(struct blk_game *g)
{
	static const int line_score[] = { 0, 100, 300, 500, 800 };
	int i, x, y, full, cleared = 0;

	for (i = 0; i < 4; i++) {
		x = g->cur.x + blk_cells[g->cur.type][g->cur.rot][i][0];
		y = g->cur.y + blk_cells[g->cur.type][g->cur.rot][i][1];
		if (y < 0) {
			/* Locked above the top of the well. */
			g->over = 1;
			continue;
		}
		g->well[y][x] = (unsigned char)(g->cur.type + 1);
	}

	for (y = g->h - 1; y >= 0; y--) {
		full = 1;
		for (x = 0; x < BLK_W; x++)
			if (g->well[y][x] == 0) {
				full = 0;
				break;
			}
		if (!full)
			continue;
		memmove(&g->well[1][0], &g->well[0][0],
		    (size_t)y * BLK_W);
		memset(&g->well[0][0], 0, BLK_W);
		cleared++;
		y++;		/* re-check the row that just moved down */
	}

	if (cleared > 0) {
		g->score += line_score[cleared] * g->level;
		g->lines += cleared;
		g->level = g->lines / 10 + 1;
	}
	if (g->score > blk_hiscore)
		blk_hiscore = g->score;
	if (!g->over)
		(void)blk_spawn(g);
}

static int
blk_gravity_ms(struct blk_game *g)
{
	int ms = BLK_GRAV0 - (g->level - 1) * BLK_GRAV_STEP;

	return (ms < BLK_GRAV_MIN ? BLK_GRAV_MIN : ms);
}

/* Is (x, y) covered by piece p? */
static int
blk_piece_at(struct blk_piece *p, int x, int y)
{
	int i;

	for (i = 0; i < 4; i++)
		if (p->x + blk_cells[p->type][p->rot][i][0] == x &&
		    p->y + blk_cells[p->type][p->rot][i][1] == y)
			return (TRUE);
	return (FALSE);
}

/*
 * Build the screen: the well on the left (two characters per cell so
 * the blocks look roughly square) and a status panel on the right.
 */
static int
blk_render(struct blk_game *g, struct buffer *bp)
{
	char		 line[128], side[48];
	struct blk_piece ghost;
	int		 x, y, i, n, row, col;

	bp->b_flag |= BFIGNDIRTY;
	if (bclear(bp) != TRUE)
		return (FALSE);

	/* Where the current piece would land. */
	ghost = g->cur;
	while (blk_fits(g, &ghost))
		ghost.y++;
	ghost.y--;

	for (y = 0; y < g->h; y++) {
		n = 0;
		line[n++] = ' ';
		line[n++] = ' ';
		line[n++] = '|';
		for (x = 0; x < BLK_W; x++) {
			if (g->well[y][x] != 0 ||
			    (!g->over && blk_piece_at(&g->cur, x, y))) {
				line[n++] = '[';
				line[n++] = ']';
			} else if (!g->over && !g->paused &&
			    blk_piece_at(&ghost, x, y)) {
				line[n++] = '.';
				line[n++] = '.';
			} else {
				line[n++] = ' ';
				line[n++] = ' ';
			}
		}
		line[n++] = '|';
		line[n] = '\0';

		side[0] = '\0';
		switch (y) {
		case 0:
			snprintf(side, sizeof(side), "Score: %d", g->score);
			break;
		case 1:
			snprintf(side, sizeof(side), "Lines: %d", g->lines);
			break;
		case 2:
			snprintf(side, sizeof(side), "Level: %d", g->level);
			break;
		case 3:
			snprintf(side, sizeof(side), "High:  %d",
			    blk_hiscore);
			break;
		case 5:
			snprintf(side, sizeof(side), "Next:");
			break;
		case 6: case 7:
			/* Two rows are enough for every piece's spawn shape. */
			row = y - 6 + (g->next == 0 ? 1 : 0);
			n = 0;
			for (col = 0; col < 4; col++) {
				int filled = 0;

				for (i = 0; i < 4; i++)
					if (blk_cells[g->next][0][i][0] == col &&
					    blk_cells[g->next][0][i][1] == row)
						filled = 1;
				side[n++] = filled ? '[' : ' ';
				side[n++] = filled ? ']' : ' ';
			}
			side[n] = '\0';
			break;
		case 9:
			if (g->over)
				snprintf(side, sizeof(side), "GAME OVER");
			else if (g->paused)
				snprintf(side, sizeof(side), "PAUSED");
			break;
		case 11:
			snprintf(side, sizeof(side), "<- ->  move");
			break;
		case 12:
			snprintf(side, sizeof(side), "up x z rotate");
			break;
		case 13:
			snprintf(side, sizeof(side), "down   soft drop");
			break;
		case 14:
			snprintf(side, sizeof(side), "SPC    hard drop");
			break;
		case 15:
			snprintf(side, sizeof(side), "p pause  q quit");
			break;
		case 16:
			if (g->over)
				snprintf(side, sizeof(side), "r new game");
			break;
		}
		addlinef(bp, "%s   %s", line, side);
	}

	n = 0;
	line[n++] = ' ';
	line[n++] = ' ';
	line[n++] = '+';
	for (x = 0; x < BLK_W * 2; x++)
		line[n++] = '-';
	line[n++] = '+';
	line[n] = '\0';
	addline(bp, line);

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

/* Wait up to msec for input; TRUE if a key is ready. */
static int
blk_poll(int msec)
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

static enum blk_cmd
blk_readkey(void)
{
	int c;

	c = getkey(FALSE);
	if (c == CCHR('[')) {
		if (!blk_poll(50))
			return (BC_NONE);
		c = getkey(FALSE);
		if (c != '[' && c != 'O')
			return (BC_NONE);
		if (!blk_poll(50))
			return (BC_NONE);
		switch (getkey(FALSE)) {
		case 'A':	return (BC_ROT_CW);
		case 'B':	return (BC_SOFT);
		case 'C':	return (BC_RIGHT);
		case 'D':	return (BC_LEFT);
		default:	return (BC_NONE);
		}
	}

	switch (c) {
	case CCHR('B'): case 'h': case 'a':
		return (BC_LEFT);
	case CCHR('F'): case 'l': case 'd':
		return (BC_RIGHT);
	case CCHR('P'): case 'k': case 'w': case 'x':
		return (BC_ROT_CW);
	case 'z':
		return (BC_ROT_CCW);
	case CCHR('N'): case 'j': case 's':
		return (BC_SOFT);
	case ' ':
		return (BC_HARD);
	case 'p': case 'P':
		return (BC_PAUSE);
	case 'q': case 'Q': case CCHR('G'):
		return (BC_QUIT);
	case 'r': case 'R':
		return (BC_RESTART);
	default:
		return (BC_NONE);
	}
}

/*
 * M-x blocks: play the falling-block game in *Blocks* until the player
 * quits.
 */
int
blocks(int f, int n)
{
	static int	 seeded;
	struct blk_game	 g;
	struct buffer	*bp;
	enum blk_cmd	 cmd;
	long		 deadline, now;
	int		 h, dist, redraw_now;

	if (!seeded) {
		srandom((unsigned int)(time(NULL) ^ (getpid() << 8)));
		seeded = 1;
	}

	if ((bp = bfind(BLK_BUFNAME, TRUE)) == NULL)
		return (FALSE);
	curbp = bp;
	if (showbuffer(bp, curwp, WFFRAME | WFFULL) != TRUE)
		return (FALSE);

	/* The well plus its floor line must fit in the window. */
	h = curwp->w_ntrows - 1;
	if (h > BLK_MAX_H)
		h = BLK_MAX_H;
	if (h < BLK_MIN_H || curwp->w_ncols < 45)
		return (dobeep_msg("Window too small for blocks"));

	blk_reset(&g, h);
	eerase();
	if (blk_render(&g, bp) != TRUE)
		return (FALSE);

	deadline = blk_now() + blk_gravity_ms(&g);
	for (;;) {
		if (g.over || g.paused) {
			cmd = blk_readkey();
		} else {
			now = blk_now();
			if (now >= deadline ||
			    !blk_poll((int)(deadline - now))) {
				/* Gravity tick. */
				if (!blk_move(&g, 0, 1))
					blk_lock(&g);
				deadline = blk_now() + blk_gravity_ms(&g);
				if (blk_render(&g, bp) != TRUE)
					return (FALSE);
				continue;
			}
			cmd = blk_readkey();
		}

		redraw_now = 1;
		switch (cmd) {
		case BC_LEFT:
			if (!g.over && !g.paused)
				(void)blk_move(&g, -1, 0);
			break;
		case BC_RIGHT:
			if (!g.over && !g.paused)
				(void)blk_move(&g, 1, 0);
			break;
		case BC_ROT_CW:
			if (!g.over && !g.paused)
				blk_rotate(&g, 1);
			break;
		case BC_ROT_CCW:
			if (!g.over && !g.paused)
				blk_rotate(&g, -1);
			break;
		case BC_SOFT:
			if (!g.over && !g.paused) {
				if (blk_move(&g, 0, 1))
					g.score += 1;
				deadline = blk_now() + blk_gravity_ms(&g);
			}
			break;
		case BC_HARD:
			if (!g.over && !g.paused) {
				dist = 0;
				while (blk_move(&g, 0, 1))
					dist++;
				g.score += 2 * dist;
				blk_lock(&g);
				deadline = blk_now() + blk_gravity_ms(&g);
			}
			break;
		case BC_PAUSE:
			if (!g.over) {
				g.paused = !g.paused;
				deadline = blk_now() + blk_gravity_ms(&g);
			}
			break;
		case BC_RESTART:
			if (g.over) {
				blk_reset(&g, h);
				deadline = blk_now() + blk_gravity_ms(&g);
			}
			break;
		case BC_QUIT:
			goto done;
		default:
			redraw_now = 0;
			break;
		}
		if (g.score > blk_hiscore)
			blk_hiscore = g.score;
		if (redraw_now && blk_render(&g, bp) != TRUE)
			return (FALSE);
	}
done:
	ewprintf("Blocks: final score %d, %d lines, high score %d",
	    g.score, g.lines, blk_hiscore);
	return (TRUE);
}
