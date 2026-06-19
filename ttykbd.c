/*	$OpenBSD: ttykbd.c,v 1.22 2023/03/30 19:00:02 op Exp $	*/

/* This file is in the public domain. */

/*
 * Name:	MG 2a
 *		Terminfo keyboard driver using key files
 * Created:	22-Nov-1987 Mic Kaczmarczik (mic@emx.cc.utexas.edu)
 */

#include <sys/queue.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <term.h>

#include "def.h"
#include "kbd.h"

/*
 * Get keyboard character.  Very simple if you use keymaps and keys files.
 */

char	*keystrings[] = {NULL};

/*
 * Turn on function keys using keypad_xmit, then load a keys file, if
 * available.  The keys file is located in the same manner as the startup
 * file is, depending on what startupfile() does on your system.
 */
void
ttykeymapinit(void)
{
	char	*cp, file[NFILEN];
	FILE	*ffp;

	/* Bind keypad function keys. */
	if (key_left)
		dobindkey(fundamental_map, "backward-char", key_left);
	if (key_right)
		dobindkey(fundamental_map, "forward-char", key_right);
	if (key_up)
		dobindkey(fundamental_map, "previous-line", key_up);
	if (key_down)
		dobindkey(fundamental_map, "next-line", key_down);
	if (key_beg)
		dobindkey(fundamental_map, "beginning-of-line", key_beg);
	else if (key_home)
		dobindkey(fundamental_map, "beginning-of-line", key_home);
	if (key_end)
		dobindkey(fundamental_map, "end-of-line", key_end);
	if (key_npage)
		dobindkey(fundamental_map, "scroll-up", key_npage);
	if (key_ppage)
		dobindkey(fundamental_map, "scroll-down", key_ppage);
	if (key_ic)
		dobindkey(fundamental_map, "overwrite-mode", key_ic);
	if (key_dc)
		dobindkey(fundamental_map, "delete-char", key_dc);

	/*
	 * Emacs-style Ctrl+Left / Ctrl+Right word movement.  Terminfo has
	 * no standard capability for these; some entries publish them as
	 * extended caps kLFT5 / kRIT5, so honor those when present, and
	 * also bind the well-known xterm and rxvt-style sequences so the
	 * keys work on essentially every modern terminal.
	 */
	{
		char *p;

		p = tigetstr("kLFT5");
		if (p != NULL && p != (char *)-1)
			dobindkey(fundamental_map, "backward-word", p);
		dobindkey(fundamental_map, "backward-word", "\033[1;5D");
		dobindkey(fundamental_map, "backward-word", "\033Od");
		dobindkey(fundamental_map, "backward-word", "\033[5D");

		p = tigetstr("kRIT5");
		if (p != NULL && p != (char *)-1)
			dobindkey(fundamental_map, "forward-word", p);
		dobindkey(fundamental_map, "forward-word", "\033[1;5C");
		dobindkey(fundamental_map, "forward-word", "\033Oc");
		dobindkey(fundamental_map, "forward-word", "\033[5C");
	}

	/*
	 * Emacs-style C-x <left> / C-x <right> buffer cycling.  Bind the
	 * terminfo-reported arrow sequence (when present) and the common
	 * xterm/rxvt fallbacks, each prefixed by C-x.
	 */
	{
		char buf[32];

		if (key_right != NULL) {
			snprintf(buf, sizeof(buf), "\030%s", key_right);
			dobindkey(fundamental_map, "next-buffer", buf);
		}
		dobindkey(fundamental_map, "next-buffer", "^X\\e[C");
		dobindkey(fundamental_map, "next-buffer", "^X\\eOC");

		if (key_left != NULL) {
			snprintf(buf, sizeof(buf), "\030%s", key_left);
			dobindkey(fundamental_map, "previous-buffer", buf);
		}
		dobindkey(fundamental_map, "previous-buffer", "^X\\e[D");
		dobindkey(fundamental_map, "previous-buffer", "^X\\eOD");
	}

	if ((cp = getenv("TERM")) != NULL &&
	    (ffp = startupfile(cp, NULL, file, sizeof(file))) != NULL) {
		if (load(ffp, file) != TRUE)
			ewprintf("Error reading key initialization file");
		(void)ffclose(ffp, NULL);
	}
	if (keypad_xmit)
		/* turn on keypad */
		putpad(keypad_xmit, 1);
}

/*
 * Clean up the keyboard -- called by tttidy()
 */
void
ttykeymaptidy(void)
{
	if (keypad_local)
		/* turn off keypad */
		putpad(keypad_local, 1);
}
