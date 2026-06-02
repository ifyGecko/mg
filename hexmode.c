/* This file is in the public domain. */

/*
 * M-x hex-mode for mg.
 *
 * Toggles the current buffer between raw text view and a classic
 * 16-byte-per-row hex-dump layout (offset / hex columns / ASCII gutter).
 * Editing happens via custom bindings that snap to byte boundaries and
 * keep the hex pair and ASCII gutter consistent. Saving rewrites the
 * raw bytes back to disk via a hook in writeout() (see file.c).
 *
 * Row layout (one line per 16 bytes; total line length is 78 cols):
 *
 *   00000000: 48 65 6c 6c 6f 20 77 6f  72 6c 64 0a 00 00 00 00  |Hello world.....|
 *   ^^^^^^^^                                                    ^^^^^^^^^^^^^^^^^^
 *   |0..9    |10..58 hex pairs (with mid-gap at 33-34)   |60 |61..76 ASCII |77|
 */

#include <sys/queue.h>
#include <ctype.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "def.h"
#include "funmap.h"
#include "kbd.h"
#include "key.h"

extern int changemode(int, int, char *);

#define HEX_BYTES_PER_ROW    16
#define HEX_OFFSET_LEN       8
#define HEX_FIRST_HEX_COL    10
#define HEX_ASCII_FIRST_COL  61
#define HEX_ASCII_LAST_COL   76
#define HEX_GUTTER_OPEN_COL  60
#define HEX_GUTTER_CLOSE_COL 77
#define HEX_LINE_LEN         78

#define HEX_REG_OFFSET 0
#define HEX_REG_HEX    1
#define HEX_REG_GAP    2
#define HEX_REG_ASCII  3

static int hexmode_toggle(int, int);
static int hex_self_insert(int, int);
static int hex_delete_byte(int, int);
static int hex_delete_byte_forward(int, int);
static int hex_forw_byte(int, int);
static int hex_back_byte(int, int);
static int hex_bol(int, int);
static int hex_eol(int, int);
static int hex_kill_region(int, int);
static int hex_copy_region(int, int);
static int hex_yank(int, int);

void hexmode_init(void);
int  hex_write_raw(struct buffer *, FILE *);
int  hex_buffer_is_hex(struct buffer *);

static int  hex_is_hex_digit(int);
static int  hex_val(int);
static char hex_digit(int);
static int  hex_col_for_byte(int, int);
static int  hex_ascii_col_for_byte(int);
static void hex_locate(int, int *, int *, int *);
static uint64_t hex_offset_of_line(struct line *);
static int  hex_row_byte_count(struct line *);
static int  hex_parse_byte(struct line *, int, unsigned char *);
static int  hex_collect_bytes(struct buffer *, unsigned char **, size_t *);
static int  hex_format_layout(const unsigned char *, size_t, char **, size_t *);
static int  hex_install_layout(struct buffer *, const unsigned char *, size_t, int);
static int  hex_byte_under_cursor(uint64_t *, int *);
static void hex_goto_byte(uint64_t, int);
static int  hex_delete_one(uint64_t, int);
static int  hex_region_active(uint64_t *, uint64_t *);

/* Range 0x01..0x19 inclusive: control-key bindings. */
static PF hex_ctl_pf[] = {
	hex_bol,			/* 0x01  C-a */
	hex_back_byte,			/* 0x02  C-b */
	rescan,				/* 0x03  C-c */
	hex_delete_byte_forward,	/* 0x04  C-d */
	hex_eol,			/* 0x05  C-e */
	hex_forw_byte,			/* 0x06  C-f */
	rescan,				/* 0x07  C-g */
	hex_delete_byte,		/* 0x08  C-h / BS */
	rescan,				/* 0x09  C-i */
	rescan,				/* 0x0a  C-j */
	rescan,				/* 0x0b  C-k */
	rescan,				/* 0x0c  C-l */
	rescan,				/* 0x0d  C-m / RET */
	rescan,				/* 0x0e  C-n */
	rescan,				/* 0x0f  C-o */
	rescan,				/* 0x10  C-p */
	rescan,				/* 0x11  C-q */
	rescan,				/* 0x12  C-r */
	rescan,				/* 0x13  C-s */
	rescan,				/* 0x14  C-t */
	rescan,				/* 0x15  C-u */
	rescan,				/* 0x16  C-v */
	hex_kill_region,		/* 0x17  C-w */
	rescan,				/* 0x18  C-x */
	hex_yank			/* 0x19  C-y */
};

/* Range 0x20..0x7E: every printable maps to hex_self_insert. */
#define S5  hex_self_insert, hex_self_insert, hex_self_insert, hex_self_insert, hex_self_insert
static PF hex_print_pf[] = {
	S5, S5, S5, S5, S5,
	S5, S5, S5, S5, S5,
	S5, S5, S5, S5, S5,
	S5, S5, S5, S5
};
#undef S5
/* 5*19 = 95 = 0x7E - 0x20 + 1. */

/* Range 0x7F..0x7F: DEL is a delete. */
static PF hex_del_pf[] = {
	hex_delete_byte			/* 0x7F  DEL */
};

/*
 * ESC prefix: bind only 'w' (M-w -> byte-aware copy-region). All other
 * meta-bindings fall through to the global metamap via rescan.
 */
static PF hex_meta_w_pf[] = {
	hex_copy_region			/* 'w' */
};

static struct KEYMAPE (1) hex_meta_map = {
	1,
	1,
	rescan,
	{
		{ 'w', 'w', hex_meta_w_pf, NULL }
	}
};

/*
 * ESC handler: NULL marks ESC as a prefix that consults hex_meta_map
 * for the next keystroke.
 */
static PF hex_esc_pf[] = {
	NULL				/* 0x1B  ESC */
};

static struct KEYMAPE (4) hexmodemap = {
	4,
	4,
	rescan,
	{
		{ 0x01, 0x19, hex_ctl_pf,   NULL },
		{ 0x1B, 0x1B, hex_esc_pf,   (KEYMAP *)&hex_meta_map },
		{ 0x20, 0x7E, hex_print_pf, NULL },
		{ 0x7F, 0x7F, hex_del_pf,   NULL }
	}
};

void
hexmode_init(void)
{
	funmap_add(hexmode_toggle, "hex-mode", 0);
	funmap_add(hex_self_insert, "hex-self-insert", 0);
	funmap_add(hex_delete_byte, "hex-delete-byte", 0);
	funmap_add(hex_delete_byte_forward, "hex-delete-byte-forward", 0);
	funmap_add(hex_forw_byte, "hex-forw-byte", 0);
	funmap_add(hex_back_byte, "hex-back-byte", 0);
	funmap_add(hex_bol, "hex-beginning-of-line", 0);
	funmap_add(hex_eol, "hex-end-of-line", 0);
	funmap_add(hex_kill_region, "hex-kill-region", 0);
	funmap_add(hex_copy_region, "hex-copy-region", 0);
	funmap_add(hex_yank, "hex-yank", 0);
	maps_add((KEYMAP *)&hexmodemap, "hex-mode");
}

/* ---------- predicates ---------- */

static int
hex_is_hex_digit(int c)
{
	return ((c >= '0' && c <= '9') ||
	    (c >= 'a' && c <= 'f') ||
	    (c >= 'A' && c <= 'F'));
}

static int
hex_val(int c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return 10 + c - 'a';
	if (c >= 'A' && c <= 'F')
		return 10 + c - 'A';
	return -1;
}

static char
hex_digit(int v)
{
	static const char digits[] = "0123456789abcdef";
	return digits[v & 0xf];
}

static int
hex_col_for_byte(int idx, int nibble)
{
	int base = HEX_FIRST_HEX_COL + 3 * idx + (idx >= 8 ? 1 : 0);
	return base + nibble;
}

static int
hex_ascii_col_for_byte(int idx)
{
	return HEX_ASCII_FIRST_COL + idx;
}

/*
 * Given a column within a hex-mode line, classify which logical region
 * the position is in (offset / hex / gap / ASCII). If it lands on a
 * byte (hex or ASCII), out *byte_in_row is set to that byte index
 * (0..15) and *nibble (0 high / 1 low) is set for hex positions.
 */
static void
hex_locate(int col, int *byte_in_row, int *nibble, int *region)
{
	int rel, idx, sub, rel2;

	*byte_in_row = -1;
	*nibble = 0;

	if (col < HEX_FIRST_HEX_COL) {
		*region = HEX_REG_OFFSET;
		return;
	}
	if (col >= HEX_ASCII_FIRST_COL && col <= HEX_ASCII_LAST_COL) {
		*region = HEX_REG_ASCII;
		*byte_in_row = col - HEX_ASCII_FIRST_COL;
		return;
	}
	if (col == HEX_GUTTER_OPEN_COL || col == HEX_GUTTER_CLOSE_COL) {
		*region = HEX_REG_GAP;
		return;
	}
	rel = col - HEX_FIRST_HEX_COL;
	if (rel <= 23) {
		idx = rel / 3;
		sub = rel % 3;
	} else if (rel == 24) {
		*region = HEX_REG_GAP;
		return;
	} else {
		rel2 = rel - 25;
		idx = 8 + rel2 / 3;
		sub = rel2 % 3;
	}
	if (sub == 2 || idx >= HEX_BYTES_PER_ROW) {
		*region = HEX_REG_GAP;
	} else {
		*region = HEX_REG_HEX;
		*byte_in_row = idx;
		*nibble = sub;
	}
}

/* ---------- per-line parsing ---------- */

static uint64_t
hex_offset_of_line(struct line *lp)
{
	uint64_t off = 0;
	int i, v;

	if (llength(lp) < HEX_OFFSET_LEN)
		return 0;
	for (i = 0; i < HEX_OFFSET_LEN; i++) {
		v = hex_val(lgetc(lp, i));
		if (v < 0)
			return 0;
		off = (off << 4) | (uint64_t)v;
	}
	return off;
}

static int
hex_row_byte_count(struct line *lp)
{
	int i, hi, lo;
	int hi_col, lo_col;

	for (i = 0; i < HEX_BYTES_PER_ROW; i++) {
		hi_col = hex_col_for_byte(i, 0);
		lo_col = hex_col_for_byte(i, 1);
		if (hi_col >= llength(lp) || lo_col >= llength(lp))
			return i;
		hi = lgetc(lp, hi_col);
		lo = lgetc(lp, lo_col);
		if (!hex_is_hex_digit(hi) || !hex_is_hex_digit(lo))
			return i;
	}
	return HEX_BYTES_PER_ROW;
}

static int
hex_parse_byte(struct line *lp, int idx, unsigned char *out)
{
	int hi_col = hex_col_for_byte(idx, 0);
	int lo_col = hex_col_for_byte(idx, 1);
	int hi, lo;

	if (hi_col >= llength(lp) || lo_col >= llength(lp))
		return FALSE;
	hi = hex_val(lgetc(lp, hi_col));
	lo = hex_val(lgetc(lp, lo_col));
	if (hi < 0 || lo < 0)
		return FALSE;
	*out = (unsigned char)((hi << 4) | lo);
	return TRUE;
}

/*
 * Walk a hex-mode buffer and extract the raw bytes it represents.
 * Caller frees *out.
 */
static int
hex_collect_bytes(struct buffer *bp, unsigned char **out, size_t *out_len)
{
	struct line *lp;
	unsigned char *buf;
	size_t cap = 1024, n = 0;
	int row_bytes, i;
	unsigned char b;

	if ((buf = malloc(cap)) == NULL)
		return FALSE;
	for (lp = bfirstlp(bp); lp != bp->b_headp; lp = lforw(lp)) {
		row_bytes = hex_row_byte_count(lp);
		for (i = 0; i < row_bytes; i++) {
			if (hex_parse_byte(lp, i, &b) != TRUE) {
				free(buf);
				return FALSE;
			}
			if (n >= cap) {
				unsigned char *nb;
				size_t newcap = cap * 2;
				if ((nb = realloc(buf, newcap)) == NULL) {
					free(buf);
					return FALSE;
				}
				buf = nb;
				cap = newcap;
			}
			buf[n++] = b;
		}
	}
	*out = buf;
	*out_len = n;
	return TRUE;
}

/*
 * Build the full hex-layout text from raw bytes. Returned string is
 * heap-allocated and is *out_len bytes long with embedded '\n'
 * separators between rows but no trailing newline.
 */
static int
hex_format_layout(const unsigned char *data, size_t len, char **out,
    size_t *out_len)
{
	size_t rows, r, pos = 0, cap;
	size_t this_row;
	char row[HEX_LINE_LEN + 1];
	char *buf;
	int i;
	uint64_t offset;
	unsigned char b;
	int hi_col, lo_col, a_col;

	rows = (len + HEX_BYTES_PER_ROW - 1) / HEX_BYTES_PER_ROW;
	if (rows == 0)
		rows = 1;
	cap = rows * (HEX_LINE_LEN + 1) + 8;
	if ((buf = malloc(cap)) == NULL)
		return FALSE;

	for (r = 0; r < rows; r++) {
		offset = (uint64_t)r * HEX_BYTES_PER_ROW;
		if (len >= (size_t)offset + HEX_BYTES_PER_ROW)
			this_row = HEX_BYTES_PER_ROW;
		else if (len > offset)
			this_row = len - offset;
		else
			this_row = 0;

		memset(row, ' ', HEX_LINE_LEN);
		row[HEX_LINE_LEN] = '\0';
		for (i = 0; i < HEX_OFFSET_LEN; i++) {
			int shift = (HEX_OFFSET_LEN - 1 - i) * 4;
			row[i] = hex_digit((int)((offset >> shift) & 0xf));
		}
		row[HEX_OFFSET_LEN] = ':';
		row[HEX_GUTTER_OPEN_COL] = '|';
		row[HEX_GUTTER_CLOSE_COL] = '|';

		for (i = 0; i < (int)this_row; i++) {
			b = data[offset + (uint64_t)i];
			hi_col = hex_col_for_byte(i, 0);
			lo_col = hex_col_for_byte(i, 1);
			a_col  = hex_ascii_col_for_byte(i);
			row[hi_col] = hex_digit((b >> 4) & 0xf);
			row[lo_col] = hex_digit(b & 0xf);
			row[a_col] = (b >= 0x20 && b < 0x7f) ?
			    (char)b : '.';
		}

		if (pos + HEX_LINE_LEN + 1 > cap) {
			char *nb;
			size_t newcap = cap * 2;
			if ((nb = realloc(buf, newcap)) == NULL) {
				free(buf);
				return FALSE;
			}
			buf = nb;
			cap = newcap;
		}
		memcpy(buf + pos, row, HEX_LINE_LEN);
		pos += HEX_LINE_LEN;
		if (r + 1 < rows)
			buf[pos++] = '\n';
	}

	*out = buf;
	*out_len = pos;
	return TRUE;
}

/*
 * Replace bp's contents with the hex layout for `data`. If undoable
 * is TRUE, perform via ldelete+linsert so undo can roll back; otherwise
 * use the bclear+addline fast path (used only by the initial toggle).
 */
static int
hex_install_layout(struct buffer *bp, const unsigned char *data, size_t len,
    int undoable)
{
	char *layout = NULL;
	size_t layout_len, i;
	struct mgwin *wp;
	struct line *first;

	if (hex_format_layout(data, len, &layout, &layout_len) != TRUE)
		return FALSE;

	bp->b_flag |= BFIGNDIRTY;

	for (wp = wheadp; wp != NULL; wp = wp->w_wndp) {
		if (wp->w_bufp == bp) {
			wp->w_dotp = bp->b_headp;
			wp->w_doto = 0;
			wp->w_markp = NULL;
			wp->w_marko = 0;
			wp->w_dotline = 1;
			wp->w_rflag |= WFFULL | WFMODE;
		}
	}
	bp->b_dotp = bp->b_headp;
	bp->b_doto = 0;
	bp->b_markp = NULL;
	bp->b_marko = 0;
	bp->b_dotline = 1;

	if (undoable) {
		RSIZE total = 0;
		struct line *lp;

		for (lp = bfirstlp(bp); lp != bp->b_headp; lp = lforw(lp)) {
			total += llength(lp);
			if (lforw(lp) != bp->b_headp)
				total++;
		}
		curwp->w_dotp = bfirstlp(bp);
		curwp->w_doto = 0;
		if (total > 0) {
			if (ldelete(total, KFORW | KNOTKILL) == FALSE) {
				free(layout);
				bp->b_flag &= ~BFIGNDIRTY;
				return FALSE;
			}
		}
		for (i = 0; i < layout_len; i++) {
			if (layout[i] == '\n') {
				if (lnewline() == FALSE) {
					free(layout);
					bp->b_flag &= ~BFIGNDIRTY;
					return FALSE;
				}
			} else {
				if (linsert(1, (unsigned char)layout[i])
				    == FALSE) {
					free(layout);
					bp->b_flag &= ~BFIGNDIRTY;
					return FALSE;
				}
			}
		}
	} else {
		char *p, *end, *nl, *ltext;
		size_t lsz;

		if (bclear(bp) != TRUE) {
			free(layout);
			bp->b_flag &= ~BFIGNDIRTY;
			return FALSE;
		}
		p = layout;
		end = layout + layout_len;
		while (p < end) {
			nl = memchr(p, '\n', (size_t)(end - p));
			lsz = (nl != NULL) ? (size_t)(nl - p) :
			    (size_t)(end - p);
			if ((ltext = malloc(lsz + 1)) == NULL) {
				free(layout);
				bp->b_flag &= ~BFIGNDIRTY;
				return FALSE;
			}
			if (lsz > 0)
				memcpy(ltext, p, lsz);
			ltext[lsz] = '\0';
			if (addline(bp, ltext) == FALSE) {
				free(ltext);
				free(layout);
				bp->b_flag &= ~BFIGNDIRTY;
				return FALSE;
			}
			free(ltext);
			if (nl == NULL)
				break;
			p = nl + 1;
		}
	}

	first = bfirstlp(bp);
	for (wp = wheadp; wp != NULL; wp = wp->w_wndp) {
		if (wp->w_bufp == bp) {
			wp->w_dotp = first;
			wp->w_doto = HEX_FIRST_HEX_COL;
			wp->w_dotline = 1;
			wp->w_linep = first;
			wp->w_markp = NULL;
			wp->w_marko = 0;
			wp->w_rflag |= WFFULL | WFMODE;
		}
	}
	bp->b_dotp = first;
	bp->b_doto = HEX_FIRST_HEX_COL;
	bp->b_dotline = 1;

	bp->b_flag &= ~BFIGNDIRTY;
	free(layout);
	return TRUE;
}

/* ---------- mode detection and toggle ---------- */

int
hex_buffer_is_hex(struct buffer *bp)
{
	int i;
	struct maps_s *m;

	if (bp == NULL)
		return FALSE;
	if ((m = name_mode("hex-mode")) == NULL)
		return FALSE;
	for (i = 0; i <= bp->b_nmodes; i++)
		if (bp->b_modes[i] == m)
			return TRUE;
	return FALSE;
}

static int
hex_buffer_to_raw_text(struct buffer *bp, unsigned char *data, size_t data_len)
{
	char nlc = (bp->b_nlchr != NULL) ? *bp->b_nlchr : '\n';
	size_t start = 0, i;
	char *ltext;
	size_t lsz;

	bp->b_flag |= BFIGNDIRTY;
	if (bclear(bp) != TRUE) {
		bp->b_flag &= ~BFIGNDIRTY;
		return FALSE;
	}
	for (i = 0; i <= data_len; i++) {
		if (i == data_len || data[i] == (unsigned char)nlc) {
			lsz = i - start;
			if ((ltext = malloc(lsz + 1)) == NULL) {
				bp->b_flag &= ~BFIGNDIRTY;
				return FALSE;
			}
			if (lsz > 0)
				memcpy(ltext, data + start, lsz);
			ltext[lsz] = '\0';
			if (addline(bp, ltext) == FALSE) {
				free(ltext);
				bp->b_flag &= ~BFIGNDIRTY;
				return FALSE;
			}
			free(ltext);
			start = i + 1;
		}
	}
	bp->b_flag &= ~BFIGNDIRTY;
	return TRUE;
}

static int
hexmode_toggle(int f, int n)
{
	struct buffer *bp = curbp;
	struct mgwin *wp = curwp;
	unsigned char *data;
	size_t data_len = 0;

	if (hex_buffer_is_hex(bp)) {
		if (hex_collect_bytes(bp, &data, &data_len) != TRUE)
			return (dobeep_msg("Failed to parse hex layout"));

		(void)changemode(FFRAND, -1, "hex-mode");

		if (hex_buffer_to_raw_text(bp, data, data_len) != TRUE) {
			free(data);
			return (FALSE);
		}
		free(data);

		bp->b_dotp = bfirstlp(bp);
		bp->b_doto = 0;
		bp->b_dotline = 1;
		wp->w_dotp = bfirstlp(bp);
		wp->w_doto = 0;
		wp->w_dotline = 1;
		wp->w_markp = NULL;
		wp->w_marko = 0;
		wp->w_linep = bfirstlp(bp);
		wp->w_rflag |= WFFULL | WFMODE;
		ewprintf("Exited hex-mode");
		return (TRUE);
	} else {
		struct line *lp;
		unsigned char *buf;
		size_t cap = 1024, nb = 0;
		char nlc = (bp->b_nlchr != NULL) ? *bp->b_nlchr : '\n';
		int len;
		int chg_was_set = (bp->b_flag & BFCHG) != 0;

		if ((buf = malloc(cap)) == NULL)
			return (FALSE);
		for (lp = bfirstlp(bp); lp != bp->b_headp; lp = lforw(lp)) {
			len = llength(lp);
			if (nb + (size_t)len + 1 > cap) {
				unsigned char *nx;
				size_t newcap = cap;
				while (nb + (size_t)len + 1 > newcap)
					newcap *= 2;
				if ((nx = realloc(buf, newcap)) == NULL) {
					free(buf);
					return (FALSE);
				}
				buf = nx;
				cap = newcap;
			}
			if (len > 0)
				memcpy(buf + nb, ltext(lp), (size_t)len);
			nb += (size_t)len;
			if (lforw(lp) != bp->b_headp)
				buf[nb++] = (unsigned char)nlc;
		}

		if (hex_install_layout(bp, buf, nb, FALSE) != TRUE) {
			free(buf);
			return (FALSE);
		}
		free(buf);

		(void)changemode(FFRAND, 1, "hex-mode");

		if (chg_was_set)
			bp->b_flag |= BFCHG;
		else
			bp->b_flag &= ~BFCHG;
		wp->w_rflag |= WFFULL | WFMODE;
		ewprintf("Entered hex-mode");
		return (TRUE);
	}
}

/* ---------- cursor helpers ---------- */

static int
hex_byte_under_cursor(uint64_t *out_byte, int *out_in_ascii)
{
	int byte_in_row, nibble, region;
	uint64_t row_off;

	hex_locate(curwp->w_doto, &byte_in_row, &nibble, &region);
	if (region != HEX_REG_HEX && region != HEX_REG_ASCII)
		return FALSE;
	if (byte_in_row < 0)
		return FALSE;
	row_off = hex_offset_of_line(curwp->w_dotp);
	*out_byte = row_off + (uint64_t)byte_in_row;
	*out_in_ascii = (region == HEX_REG_ASCII);
	return TRUE;
}

static void
hex_goto_byte(uint64_t byte_idx, int in_ascii)
{
	struct line *lp;
	uint64_t target_row_off;
	int byte_in_row;
	int lineno = 1;
	int col;

	target_row_off = byte_idx & ~(uint64_t)(HEX_BYTES_PER_ROW - 1);
	byte_in_row = (int)(byte_idx - target_row_off);
	if (in_ascii)
		col = hex_ascii_col_for_byte(byte_in_row);
	else
		col = hex_col_for_byte(byte_in_row, 0);

	for (lp = bfirstlp(curbp); lp != curbp->b_headp; lp = lforw(lp)) {
		if (hex_offset_of_line(lp) == target_row_off) {
			curwp->w_dotp = lp;
			curwp->w_doto = col;
			curwp->w_dotline = lineno;
			curwp->w_rflag |= WFMOVE;
			return;
		}
		lineno++;
	}
	/* Target past EOB: clamp to last row's last byte. */
	lp = lback(curbp->b_headp);
	if (lp != curbp->b_headp) {
		int row_bytes = hex_row_byte_count(lp);
		int idx = (row_bytes > 0) ? row_bytes - 1 : 0;
		if (in_ascii)
			curwp->w_doto = hex_ascii_col_for_byte(idx);
		else
			curwp->w_doto = hex_col_for_byte(idx, 0);
		curwp->w_dotp = lp;
		curwp->w_dotline = lineno - 1;
		curwp->w_rflag |= WFMOVE;
	}
}

/* ---------- self-insert ---------- */

static int
hex_self_insert(int f, int n)
{
	int c = key.k_chars[key.k_count - 1];
	int byte_in_row, nibble, region;
	uint64_t row_off;
	unsigned char old_byte = 0;
	int hi_col, lo_col, a_col;
	int v;

	if (curbp->b_flag & BFREADONLY)
		return (dobeep_msg("Buffer is read only"));

	hex_locate(curwp->w_doto, &byte_in_row, &nibble, &region);
	row_off = hex_offset_of_line(curwp->w_dotp);

	if (region == HEX_REG_HEX) {
		if (!hex_is_hex_digit(c))
			return (dobeep_msg("Not a hex digit"));
		if (hex_parse_byte(curwp->w_dotp, byte_in_row, &old_byte)
		    != TRUE)
			return (dobeep_msg("Cursor not on a byte"));

		v = hex_val(c);
		if (nibble == 0)
			old_byte = (unsigned char)((v << 4) | (old_byte & 0xf));
		else
			old_byte = (unsigned char)((old_byte & 0xf0) | v);

		undo_boundary_enable(FFRAND, 0);

		hi_col = hex_col_for_byte(byte_in_row, 0);
		lo_col = hex_col_for_byte(byte_in_row, 1);
		a_col  = hex_ascii_col_for_byte(byte_in_row);

		if (nibble == 0) {
			undo_add_change(curwp->w_dotp, hi_col, 1);
			lputc(curwp->w_dotp, hi_col,
			    hex_digit((old_byte >> 4) & 0xf));
		} else {
			undo_add_change(curwp->w_dotp, lo_col, 1);
			lputc(curwp->w_dotp, lo_col,
			    hex_digit(old_byte & 0xf));
		}
		if (a_col < llength(curwp->w_dotp)) {
			char a_char = (old_byte >= 0x20 && old_byte < 0x7f) ?
			    (char)old_byte : '.';
			undo_add_change(curwp->w_dotp, a_col, 1);
			lputc(curwp->w_dotp, a_col, a_char);
		}

		curbp->b_flag |= BFCHG;
		lchange(WFEDIT);

		if (nibble == 0)
			curwp->w_doto = lo_col;
		else
			(void)hex_forw_byte(FFRAND, 1);

		undo_boundary_enable(FFRAND, 1);
		return (TRUE);
	} else if (region == HEX_REG_ASCII) {
		uint64_t next;

		if (c < 0x20 || c > 0x7e)
			return (dobeep_msg("Non-printable in ASCII column"));
		if (byte_in_row >= hex_row_byte_count(curwp->w_dotp))
			return (dobeep_msg("Past end of buffer"));

		old_byte = (unsigned char)c;
		hi_col = hex_col_for_byte(byte_in_row, 0);
		lo_col = hex_col_for_byte(byte_in_row, 1);
		a_col  = hex_ascii_col_for_byte(byte_in_row);

		undo_boundary_enable(FFRAND, 0);

		undo_add_change(curwp->w_dotp, hi_col, 1);
		lputc(curwp->w_dotp, hi_col,
		    hex_digit((old_byte >> 4) & 0xf));
		undo_add_change(curwp->w_dotp, lo_col, 1);
		lputc(curwp->w_dotp, lo_col, hex_digit(old_byte & 0xf));
		undo_add_change(curwp->w_dotp, a_col, 1);
		lputc(curwp->w_dotp, a_col, (char)c);

		curbp->b_flag |= BFCHG;
		lchange(WFEDIT);

		next = row_off + (uint64_t)byte_in_row + 1;
		hex_goto_byte(next, TRUE);

		undo_boundary_enable(FFRAND, 1);
		return (TRUE);
	}

	return (dobeep_msg("Cursor not on an editable position"));
}

/* ---------- delete ---------- */

static int
hex_delete_one(uint64_t byte_idx, int in_ascii)
{
	unsigned char *data;
	size_t data_len;

	if (curbp->b_flag & BFREADONLY)
		return (dobeep_msg("Buffer is read only"));
	if (hex_collect_bytes(curbp, &data, &data_len) != TRUE)
		return (FALSE);
	if (byte_idx >= data_len) {
		free(data);
		return (dobeep_msg("Past end of buffer"));
	}
	if (data_len > byte_idx + 1)
		memmove(data + byte_idx, data + byte_idx + 1,
		    data_len - byte_idx - 1);
	data_len--;

	undo_boundary_enable(FFRAND, 0);
	if (hex_install_layout(curbp, data, data_len, TRUE) != TRUE) {
		free(data);
		undo_boundary_enable(FFRAND, 1);
		return (FALSE);
	}
	undo_boundary_enable(FFRAND, 1);

	curbp->b_flag |= BFCHG;
	if (data_len == 0) {
		curwp->w_dotp = bfirstlp(curbp);
		curwp->w_doto = HEX_FIRST_HEX_COL;
		curwp->w_dotline = 1;
	} else if (byte_idx < data_len) {
		hex_goto_byte(byte_idx, in_ascii);
	} else {
		hex_goto_byte(data_len - 1, in_ascii);
	}
	curwp->w_rflag |= WFFULL | WFMOVE;
	free(data);
	return (TRUE);
}

static int
hex_delete_byte(int f, int n)
{
	uint64_t byte_idx;
	int in_ascii;
	int byte_in_row, nibble, region;
	uint64_t row_off;
	uint64_t rs, re;

	if (hex_region_active(&rs, &re))
		return (hex_kill_region(f, n));

	if (hex_byte_under_cursor(&byte_idx, &in_ascii) == TRUE) {
		if (byte_idx == 0)
			return (dobeep_msg("Beginning of buffer"));
		byte_idx--;
		return (hex_delete_one(byte_idx, in_ascii));
	}
	hex_locate(curwp->w_doto, &byte_in_row, &nibble, &region);
	row_off = hex_offset_of_line(curwp->w_dotp);
	if (byte_in_row > 0) {
		byte_idx = row_off + (uint64_t)(byte_in_row - 1);
		return (hex_delete_one(byte_idx, region == HEX_REG_ASCII));
	}
	if (row_off > 0) {
		byte_idx = row_off - 1;
		return (hex_delete_one(byte_idx, region == HEX_REG_ASCII));
	}
	return (dobeep_msg("Beginning of buffer"));
}

static int
hex_delete_byte_forward(int f, int n)
{
	uint64_t byte_idx;
	int in_ascii;
	uint64_t rs, re;

	if (hex_region_active(&rs, &re))
		return (hex_kill_region(f, n));
	if (hex_byte_under_cursor(&byte_idx, &in_ascii) != TRUE)
		return (dobeep_msg("Cursor not on a byte"));
	return (hex_delete_one(byte_idx, in_ascii));
}

/* ---------- movement ---------- */

static int
hex_forw_byte(int f, int n)
{
	int byte_in_row, nibble, region;
	uint64_t row_off, byte_idx;
	int in_ascii;
	int reps;
	unsigned char *data;
	size_t data_len;

	if (n < 0)
		return (hex_back_byte(f, -n));
	if (n == 0)
		return (TRUE);

	hex_locate(curwp->w_doto, &byte_in_row, &nibble, &region);
	if (region != HEX_REG_HEX && region != HEX_REG_ASCII) {
		curwp->w_doto = HEX_FIRST_HEX_COL;
		byte_in_row = 0;
		region = HEX_REG_HEX;
	}
	in_ascii = (region == HEX_REG_ASCII);
	row_off = hex_offset_of_line(curwp->w_dotp);
	byte_idx = row_off + (uint64_t)byte_in_row;

	if (hex_collect_bytes(curbp, &data, &data_len) != TRUE)
		return (FALSE);
	for (reps = 0; reps < n; reps++) {
		if (byte_idx + 1 >= data_len) {
			free(data);
			return (dobeep_msg("End of buffer"));
		}
		byte_idx++;
	}
	free(data);
	hex_goto_byte(byte_idx, in_ascii);
	return (TRUE);
}

static int
hex_back_byte(int f, int n)
{
	int byte_in_row, nibble, region;
	uint64_t row_off, byte_idx;
	int in_ascii;
	int reps;

	if (n < 0)
		return (hex_forw_byte(f, -n));
	if (n == 0)
		return (TRUE);

	hex_locate(curwp->w_doto, &byte_in_row, &nibble, &region);
	if (region != HEX_REG_HEX && region != HEX_REG_ASCII) {
		curwp->w_doto = HEX_FIRST_HEX_COL;
		byte_in_row = 0;
		region = HEX_REG_HEX;
	}
	in_ascii = (region == HEX_REG_ASCII);
	row_off = hex_offset_of_line(curwp->w_dotp);
	byte_idx = row_off + (uint64_t)byte_in_row;
	for (reps = 0; reps < n; reps++) {
		if (byte_idx == 0)
			return (dobeep_msg("Beginning of buffer"));
		byte_idx--;
	}
	hex_goto_byte(byte_idx, in_ascii);
	return (TRUE);
}

/*
 * C-a: snap to the first byte of the current row, staying in whichever
 * section (hex pairs / ASCII gutter) the cursor was already in.
 */
static int
hex_bol(int f, int n)
{
	int byte_in_row, nibble, region;

	hex_locate(curwp->w_doto, &byte_in_row, &nibble, &region);
	if (region == HEX_REG_ASCII)
		curwp->w_doto = hex_ascii_col_for_byte(0);
	else
		curwp->w_doto = hex_col_for_byte(0, 0);
	curwp->w_rflag |= WFMOVE;
	return (TRUE);
}

/*
 * C-e: snap to the last byte of the current row, staying in whichever
 * section the cursor was in. Lands on the low nibble in the hex section
 * (or the corresponding ASCII gutter cell), never into the gutter
 * delimiters or beyond the actual data.
 */
static int
hex_eol(int f, int n)
{
	int byte_in_row, nibble, region;
	int row_bytes = hex_row_byte_count(curwp->w_dotp);
	int idx;

	hex_locate(curwp->w_doto, &byte_in_row, &nibble, &region);
	if (row_bytes == 0) {
		curwp->w_doto = (region == HEX_REG_ASCII) ?
		    hex_ascii_col_for_byte(0) : hex_col_for_byte(0, 0);
	} else {
		idx = row_bytes - 1;
		if (region == HEX_REG_ASCII)
			curwp->w_doto = hex_ascii_col_for_byte(idx);
		else
			curwp->w_doto = hex_col_for_byte(idx, 1);
	}
	curwp->w_rflag |= WFMOVE;
	return (TRUE);
}

/* ---------- byte-aware kill/copy/yank ---------- */

static int
hex_region_byte_range(uint64_t *start, uint64_t *end)
{
	int br_d, nb_d, rg_d;
	int br_m, nb_m, rg_m;
	uint64_t off_d, off_m, b_d, b_m;

	if (curwp->w_markp == NULL)
		return (dobeep_msg("No mark set"));
	hex_locate(curwp->w_doto, &br_d, &nb_d, &rg_d);
	hex_locate(curwp->w_marko, &br_m, &nb_m, &rg_m);
	if ((rg_d != HEX_REG_HEX && rg_d != HEX_REG_ASCII) ||
	    (rg_m != HEX_REG_HEX && rg_m != HEX_REG_ASCII))
		return (dobeep_msg("Mark or point not on a byte"));
	off_d = hex_offset_of_line(curwp->w_dotp);
	off_m = hex_offset_of_line(curwp->w_markp);
	b_d = off_d + (uint64_t)br_d;
	b_m = off_m + (uint64_t)br_m;
	if (b_m <= b_d) {
		*start = b_m;
		*end = b_d;
	} else {
		*start = b_d;
		*end = b_m;
	}
	return (TRUE);
}

/*
 * Region-active probe: silent (no beep) variant of hex_region_byte_range
 * for callers that want to opportunistically treat backspace/delete as a
 * region kill when a non-empty byte region is active.
 */
static int
hex_region_active(uint64_t *start, uint64_t *end)
{
	int br_d, nb_d, rg_d;
	int br_m, nb_m, rg_m;
	uint64_t b_d, b_m;

	if (curwp->w_markp == NULL)
		return (FALSE);
	hex_locate(curwp->w_doto, &br_d, &nb_d, &rg_d);
	hex_locate(curwp->w_marko, &br_m, &nb_m, &rg_m);
	if ((rg_d != HEX_REG_HEX && rg_d != HEX_REG_ASCII) ||
	    (rg_m != HEX_REG_HEX && rg_m != HEX_REG_ASCII))
		return (FALSE);
	b_d = hex_offset_of_line(curwp->w_dotp) + (uint64_t)br_d;
	b_m = hex_offset_of_line(curwp->w_markp) + (uint64_t)br_m;
	if (b_d == b_m)
		return (FALSE);
	*start = (b_m < b_d) ? b_m : b_d;
	*end   = (b_m < b_d) ? b_d : b_m;
	return (TRUE);
}

static int
hex_copy_region(int f, int n)
{
	uint64_t start, end, i;
	unsigned char *data;
	size_t data_len;

	if (hex_region_byte_range(&start, &end) != TRUE)
		return (FALSE);
	if (hex_collect_bytes(curbp, &data, &data_len) != TRUE)
		return (FALSE);
	if (end > data_len)
		end = data_len;
	if (start > end) {
		free(data);
		return (TRUE);
	}
	kdelete();
	for (i = start; i < end; i++) {
		if (kinsert((int)data[i], KFORW) == FALSE) {
			free(data);
			return (FALSE);
		}
	}
	free(data);
	(void)clearmark(FFARG, 0);
	ewprintf("Copied %ld byte(s)", (long)(end - start));
	return (TRUE);
}

static int
hex_kill_region(int f, int n)
{
	uint64_t start, end, i;
	unsigned char *data;
	size_t data_len;
	int in_ascii = FALSE;
	int br, nb, rg;

	if (curbp->b_flag & BFREADONLY)
		return (dobeep_msg("Buffer is read only"));
	if (hex_region_byte_range(&start, &end) != TRUE)
		return (FALSE);
	if (hex_collect_bytes(curbp, &data, &data_len) != TRUE)
		return (FALSE);
	if (end > data_len)
		end = data_len;
	if (start > end) {
		free(data);
		return (TRUE);
	}
	kdelete();
	for (i = start; i < end; i++) {
		if (kinsert((int)data[i], KFORW) == FALSE) {
			free(data);
			return (FALSE);
		}
	}
	if (data_len > end)
		memmove(data + start, data + end, data_len - (size_t)end);
	data_len -= (size_t)(end - start);

	hex_locate(curwp->w_doto, &br, &nb, &rg);
	if (rg == HEX_REG_ASCII)
		in_ascii = TRUE;

	undo_boundary_enable(FFRAND, 0);
	if (hex_install_layout(curbp, data, data_len, TRUE) != TRUE) {
		free(data);
		undo_boundary_enable(FFRAND, 1);
		return (FALSE);
	}
	undo_boundary_enable(FFRAND, 1);

	curbp->b_flag |= BFCHG;
	if (data_len == 0) {
		curwp->w_dotp = bfirstlp(curbp);
		curwp->w_doto = HEX_FIRST_HEX_COL;
		curwp->w_dotline = 1;
	} else if (start < data_len)
		hex_goto_byte(start, in_ascii);
	else
		hex_goto_byte(data_len - 1, in_ascii);
	(void)clearmark(FFARG, 0);
	curwp->w_rflag |= WFFULL | WFMOVE;

	ewprintf("Killed %ld byte(s)", (long)(end - start));
	free(data);
	return (TRUE);
}

static int
hex_yank(int f, int n)
{
	int byte_in_row, nibble, region;
	uint64_t byte_idx;
	int in_ascii;
	int kc, ki, kn = 0;
	unsigned char *insert_data;
	size_t insert_len = 0;
	unsigned char *data;
	size_t data_len;
	unsigned char *combined;
	size_t new_len;

	if (curbp->b_flag & BFREADONLY)
		return (dobeep_msg("Buffer is read only"));

	while (kremove(kn) >= 0)
		kn++;
	if (kn == 0)
		return (dobeep_msg("Kill ring is empty"));
	if ((insert_data = malloc((size_t)kn)) == NULL)
		return (FALSE);
	for (ki = 0; ki < kn; ki++) {
		if ((kc = kremove(ki)) < 0)
			break;
		insert_data[insert_len++] = (unsigned char)kc;
	}

	hex_locate(curwp->w_doto, &byte_in_row, &nibble, &region);
	if (region != HEX_REG_HEX && region != HEX_REG_ASCII) {
		free(insert_data);
		return (dobeep_msg("Cursor not on a byte"));
	}
	in_ascii = (region == HEX_REG_ASCII);
	byte_idx = hex_offset_of_line(curwp->w_dotp) + (uint64_t)byte_in_row;

	if (hex_collect_bytes(curbp, &data, &data_len) != TRUE) {
		free(insert_data);
		return (FALSE);
	}
	if (byte_idx > data_len)
		byte_idx = data_len;

	new_len = data_len + insert_len;
	if ((combined = malloc(new_len > 0 ? new_len : 1)) == NULL) {
		free(insert_data);
		free(data);
		return (FALSE);
	}
	if (byte_idx > 0)
		memcpy(combined, data, (size_t)byte_idx);
	memcpy(combined + byte_idx, insert_data, insert_len);
	if (data_len > byte_idx)
		memcpy(combined + byte_idx + insert_len, data + byte_idx,
		    data_len - (size_t)byte_idx);
	free(data);
	free(insert_data);

	undo_boundary_enable(FFRAND, 0);
	if (hex_install_layout(curbp, combined, new_len, TRUE) != TRUE) {
		free(combined);
		undo_boundary_enable(FFRAND, 1);
		return (FALSE);
	}
	undo_boundary_enable(FFRAND, 1);

	curbp->b_flag |= BFCHG;
	hex_goto_byte(byte_idx + insert_len, in_ascii);
	curwp->w_rflag |= WFFULL | WFMOVE;
	free(combined);
	ewprintf("Yanked %ld byte(s)", (long)insert_len);
	return (TRUE);
}

/* ---------- save round-trip ---------- */

int
hex_write_raw(struct buffer *bp, FILE *ffp)
{
	unsigned char *data;
	size_t data_len;

	if (hex_collect_bytes(bp, &data, &data_len) != TRUE)
		return (FIOERR);
	if (data_len > 0 &&
	    fwrite(data, 1, data_len, ffp) != data_len) {
		free(data);
		dobeep();
		ewprintf("Write I/O error");
		return (FIOERR);
	}
	free(data);
	return (FIOSUC);
}
