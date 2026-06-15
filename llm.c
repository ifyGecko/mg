/* This file is in the public domain. */

/*
 * LLM coding-assistant command.
 *
 *	M-x llm
 *		Pop up a *LLM Prompt* buffer and snapshot the source
 *		buffer's region (or dot position if no mark is set).
 *		The user composes a multi-line prompt in the compose
 *		buffer (no length cap, normal editing).  C-c C-c sends
 *		the prompt + saved context to a local OpenAI-compatible
 *		chat-completions endpoint via curl(1) and replaces the
 *		original region with the contents of the first fenced
 *		code block from the response.  C-c C-k aborts.
 *
 *	M-x llm-set-url    "https://host:port/v1/chat/completions"
 *	M-x llm-set-model  "model-name"
 *	M-x llm-set-key    "api-key"   (optional; omit Authorization if empty)
 *
 *	The three setters are intended to be called from .mg via the
 *	interpreter, e.g.:
 *		(llm-set-url   "http://127.0.0.1:8080/v1/chat/completions")
 *		(llm-set-model "qwen2.5-coder")
 *		(llm-set-key   "")
 */

#include <sys/queue.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "def.h"
#include "kbd.h"
#include "funmap.h"
#include "macro.h"

#define LLM_PROMPT_BUFNAME	"*LLM Prompt*"

#define LLM_SYSTEM_PROMPT \
	"You are a coding assistant embedded in a text editor. When asked " \
	"for code, you respond with code only, inside a single fenced code " \
	"block (```language ... ```). No prose before or after. Comments " \
	"inside the code are fine."

#define LLM_WRAP_REGION \
	"Regarding the following code, %s.\n" \
	"Only output the resulting code in a single fenced code block " \
	"(```language ... ```). Do not include explanations, preamble, or " \
	"commentary. Comments inside the code are fine.\n" \
	"\n```\n%s\n```\n"

#define LLM_WRAP_NOREGION \
	"%s\n\n" \
	"Only output the resulting code in a single fenced code block " \
	"(```language ... ```). Do not include explanations, preamble, or " \
	"commentary. Comments inside the code are fine.\n"

static char	*llm_url = NULL;
static char	*llm_model = NULL;
static char	*llm_key = NULL;

/*
 * State carried across the compose-buffer round trip.  Only one
 * session is supported at a time; if the user invokes M-x llm while
 * a compose buffer is already up, the prior session is overwritten.
 * Source-buffer positions are stored as line numbers + offsets, not
 * raw struct line *, so that edits made in the source buffer between
 * compose-open and compose-send don't leave us holding freed pointers.
 */
static struct {
	int		 active;
	struct buffer	*src_bp;
	struct buffer	*prompt_bp;
	int		 has_region;
	int		 dot_lineno;
	int		 dot_offset;
	int		 mark_lineno;
	int		 mark_offset;
} llm_session;

static int	 llm_set_str(char **, const char *);
static char	*json_escape(const char *);
static char	*json_extract_content(const char *);
static char	*llm_strip_fences(char *);
static int	 llm_pipe_curl(const char *, size_t, char **, size_t *);
static int	 llm_query(const char *, const char *, size_t, int, char **);
static int	 read_macro_arg(char *, size_t);
static int	 llm_run(const char *);
static char	*buffer_text(struct buffer *, size_t *);
static struct line	*lineno_to_lp(struct buffer *, int);
static int	 buffer_in_list(struct buffer *);

/*
 * Pull a single argument out of the interpreter's argument list.
 * Mirrors the inmacro branch of insert() in extend.c.  Advances maclcur.
 */
static int
read_macro_arg(char *buf, size_t buflen)
{
	size_t	n;

	if (maclcur == NULL)
		return (FALSE);
	n = maclcur->l_used;
	if (n >= buflen)
		n = buflen - 1;
	memcpy(buf, maclcur->l_text, n);
	buf[n] = '\0';
	maclcur = maclcur->l_fp;
	return (TRUE);
}

/*
 * Common setter: prompt (or pull from macro) and replace *dst.
 */
static int
llm_set_str(char **dst, const char *prompt)
{
	char	 buf[NFILEN], *bufp, *copy;

	if (inmacro) {
		if (read_macro_arg(buf, sizeof(buf)) != TRUE)
			return (FALSE);
		bufp = buf;
	} else {
		if ((bufp = eread("%s", buf, sizeof(buf), EFNEW | EFCR | EFNUL,
		    prompt)) == NULL)
			return (ABORT);
	}
	if ((copy = strdup(bufp)) == NULL) {
		dobeep();
		ewprintf("LLM: out of memory");
		return (FALSE);
	}
	free(*dst);
	*dst = copy;
	return (TRUE);
}

int
llm_set_url(int f, int n)
{
	return (llm_set_str(&llm_url, "LLM URL: "));
}

int
llm_set_model(int f, int n)
{
	return (llm_set_str(&llm_model, "LLM model: "));
}

int
llm_set_key(int f, int n)
{
	return (llm_set_str(&llm_key, "LLM key: "));
}

/*
 * Escape a string for inclusion inside a JSON double-quoted value.
 * Returns a malloc'd buffer.
 */
static char *
json_escape(const char *in)
{
	const unsigned char	*p;
	char			*out, *q;
	size_t			 len;

	len = 1;	/* trailing NUL */
	for (p = (const unsigned char *)in; *p; p++) {
		switch (*p) {
		case '"':
		case '\\':
		case '\b':
		case '\f':
		case '\n':
		case '\r':
		case '\t':
			len += 2;
			break;
		default:
			if (*p < 0x20)
				len += 6;	/* \uXXXX */
			else
				len += 1;
		}
	}
	if ((out = malloc(len)) == NULL)
		return (NULL);
	q = out;
	for (p = (const unsigned char *)in; *p; p++) {
		switch (*p) {
		case '"':  *q++ = '\\'; *q++ = '"';  break;
		case '\\': *q++ = '\\'; *q++ = '\\'; break;
		case '\b': *q++ = '\\'; *q++ = 'b';  break;
		case '\f': *q++ = '\\'; *q++ = 'f';  break;
		case '\n': *q++ = '\\'; *q++ = 'n';  break;
		case '\r': *q++ = '\\'; *q++ = 'r';  break;
		case '\t': *q++ = '\\'; *q++ = 't';  break;
		default:
			if (*p < 0x20) {
				snprintf(q, 7, "\\u%04x", *p);
				q += 6;
			} else
				*q++ = (char)*p;
		}
	}
	*q = '\0';
	return (out);
}

/*
 * Find choices[0].message.content in a JSON response and return its decoded
 * value as a malloc'd string.  Tolerant of whitespace; rejects on missing
 * key or unterminated string.
 */
static char *
json_extract_content(const char *raw)
{
	const char	*p, *key = "\"content\"";
	char		*out, *q;
	size_t		 cap;
	unsigned int	 codepoint;
	int		 i;

	p = strstr(raw, key);
	if (p == NULL)
		return (NULL);
	p += strlen(key);
	while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
		p++;
	if (*p != ':')
		return (NULL);
	p++;
	while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
		p++;
	if (*p != '"')
		return (NULL);
	p++;

	cap = strlen(p) + 1;
	if ((out = malloc(cap)) == NULL)
		return (NULL);
	q = out;

	while (*p != '\0' && *p != '"') {
		if (*p != '\\') {
			*q++ = *p++;
			continue;
		}
		p++;
		switch (*p) {
		case '"':  *q++ = '"';  p++; break;
		case '\\': *q++ = '\\'; p++; break;
		case '/':  *q++ = '/';  p++; break;
		case 'b':  *q++ = '\b'; p++; break;
		case 'f':  *q++ = '\f'; p++; break;
		case 'n':  *q++ = '\n'; p++; break;
		case 'r':  *q++ = '\r'; p++; break;
		case 't':  *q++ = '\t'; p++; break;
		case 'u':
			p++;
			codepoint = 0;
			for (i = 0; i < 4; i++) {
				if (!isxdigit((unsigned char)p[i])) {
					free(out);
					return (NULL);
				}
				codepoint <<= 4;
				if (p[i] >= '0' && p[i] <= '9')
					codepoint |= p[i] - '0';
				else if (p[i] >= 'a' && p[i] <= 'f')
					codepoint |= p[i] - 'a' + 10;
				else
					codepoint |= p[i] - 'A' + 10;
			}
			p += 4;
			/* Encode codepoint as UTF-8.  Surrogate pairs not
			 * supported; emit replacement on lone surrogate. */
			if (codepoint < 0x80) {
				*q++ = (char)codepoint;
			} else if (codepoint < 0x800) {
				*q++ = (char)(0xC0 | (codepoint >> 6));
				*q++ = (char)(0x80 | (codepoint & 0x3F));
			} else if (codepoint >= 0xD800 && codepoint <= 0xDFFF) {
				*q++ = (char)0xEF; *q++ = (char)0xBF;
				*q++ = (char)0xBD;
			} else {
				*q++ = (char)(0xE0 | (codepoint >> 12));
				*q++ = (char)(0x80 |
				    ((codepoint >> 6) & 0x3F));
				*q++ = (char)(0x80 | (codepoint & 0x3F));
			}
			break;
		default:
			free(out);
			return (NULL);
		}
	}
	if (*p != '"') {
		free(out);
		return (NULL);
	}
	*q = '\0';
	return (out);
}

/*
 * Strip a markdown fenced code block, mirroring ellama--code-filter:
 *	- chop everything from the start up to and including the first ``` and
 *	  any language tag plus its trailing newline
 *	- chop from the LAST ``` to the end
 * If no opening fence is found, return the input unchanged (failure-open).
 * Returns a pointer into the caller's buffer; caller still frees the
 * original allocation.
 */
static char *
llm_strip_fences(char *text)
{
	char	*open, *body, *close, *p;

	if ((open = strstr(text, "```")) == NULL)
		return (text);
	if ((body = strchr(open + 3, '\n')) == NULL)
		return (text);
	body++;

	close = NULL;
	for (p = body; (p = strstr(p, "```")) != NULL; p++)
		close = p;
	if (close == NULL)
		return (body);

	if (close > body && close[-1] == '\n')
		close[-1] = '\0';
	else
		*close = '\0';
	return (body);
}

/*
 * Fork curl(1) with stdout piped back to us.  Request body is staged in a
 * private tmpfile to avoid stdin-pipe deadlocks on large requests and to
 * keep payload off the argv list.  API key, if present, is passed via -H on
 * argv (visible via ps(1) -- acceptable for local-only endpoints).
 */
static int
llm_pipe_curl(const char *body, size_t bodylen, char **out, size_t *outlen)
{
	char		 tmpl[] = "/tmp/mg-llm-XXXXXX";
	char		 auth[1024];
	char		 dataarg[64];
	char		*argv[16];
	char		*resp = NULL;
	size_t		 resp_cap = 0, resp_len = 0;
	ssize_t		 nw, nr;
	size_t		 left;
	int		 fd, p[2], argc, status;
	pid_t		 pid;

	*out = NULL;
	*outlen = 0;

	if ((fd = mkstemp(tmpl)) == -1) {
		ewprintf("LLM: mkstemp: %s", strerror(errno));
		return (FALSE);
	}
	(void)fchmod(fd, 0600);
	left = bodylen;
	while (left > 0) {
		nw = write(fd, body + (bodylen - left), left);
		if (nw <= 0) {
			if (nw == -1 && errno == EINTR)
				continue;
			close(fd);
			unlink(tmpl);
			ewprintf("LLM: write tmp: %s", strerror(errno));
			return (FALSE);
		}
		left -= (size_t)nw;
	}
	close(fd);

	if (pipe(p) == -1) {
		unlink(tmpl);
		ewprintf("LLM: pipe: %s", strerror(errno));
		return (FALSE);
	}

	if ((pid = fork()) == -1) {
		close(p[0]); close(p[1]);
		unlink(tmpl);
		ewprintf("LLM: fork: %s", strerror(errno));
		return (FALSE);
	}
	if (pid == 0) {
		/* child */
		close(p[0]);
		if (dup2(p[1], STDOUT_FILENO) == -1) _exit(127);
		if (dup2(p[1], STDERR_FILENO) == -1) _exit(127);
		close(p[1]);

		snprintf(dataarg, sizeof(dataarg), "@%s", tmpl);
		argc = 0;
		argv[argc++] = "curl";
		argv[argc++] = "-sS";
		argv[argc++] = "-X";
		argv[argc++] = "POST";
		argv[argc++] = "-H";
		argv[argc++] = "Content-Type: application/json";
		if (llm_key != NULL && llm_key[0] != '\0') {
			snprintf(auth, sizeof(auth),
			    "Authorization: Bearer %s", llm_key);
			argv[argc++] = "-H";
			argv[argc++] = auth;
		}
		argv[argc++] = llm_url;
		argv[argc++] = "--data-binary";
		argv[argc++] = dataarg;
		argv[argc] = NULL;

		execvp("curl", argv);
		_exit(127);
	}

	/* parent */
	close(p[1]);
	for (;;) {
		char	buf[4096];

		nr = read(p[0], buf, sizeof(buf));
		if (nr == 0)
			break;
		if (nr == -1) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (resp_len + (size_t)nr + 1 > resp_cap) {
			size_t	ncap = resp_cap ? resp_cap * 2 : 8192;
			char	*nresp;

			while (ncap < resp_len + (size_t)nr + 1)
				ncap *= 2;
			if ((nresp = realloc(resp, ncap)) == NULL) {
				free(resp);
				close(p[0]);
				waitpid(pid, NULL, 0);
				unlink(tmpl);
				ewprintf("LLM: out of memory");
				return (FALSE);
			}
			resp = nresp;
			resp_cap = ncap;
		}
		memcpy(resp + resp_len, buf, (size_t)nr);
		resp_len += (size_t)nr;
	}
	close(p[0]);
	waitpid(pid, &status, 0);
	unlink(tmpl);

	if (resp == NULL) {
		if ((resp = malloc(1)) == NULL) {
			ewprintf("LLM: out of memory");
			return (FALSE);
		}
		resp_len = 0;
	}
	resp[resp_len] = '\0';

	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		ewprintf("LLM: curl failed: %.200s",
		    resp_len ? resp : "(no output)");
		free(resp);
		return (FALSE);
	}

	*out = resp;
	*outlen = resp_len;
	return (TRUE);
}

/*
 * Build the chat-completions request, send it, and return the extracted
 * message content via *out (malloc'd).
 */
static int
llm_query(const char *user_instr, const char *ctx, size_t ctxlen,
    int has_region, char **out)
{
	char	*sys_esc = NULL, *user_esc = NULL, *model_esc = NULL;
	char	*user_msg = NULL, *body = NULL, *raw = NULL, *content = NULL;
	char	*ctx_buf = NULL;
	size_t	 raw_len, body_cap, body_len;
	int	 ret = FALSE, n;

	*out = NULL;

	if (has_region) {
		if ((ctx_buf = malloc(ctxlen + 1)) == NULL) {
			ewprintf("LLM: out of memory");
			goto done;
		}
		memcpy(ctx_buf, ctx, ctxlen);
		ctx_buf[ctxlen] = '\0';

		body_cap = strlen(LLM_WRAP_REGION) + strlen(user_instr) +
		    ctxlen + 16;
		if ((user_msg = malloc(body_cap)) == NULL) {
			ewprintf("LLM: out of memory");
			goto done;
		}
		snprintf(user_msg, body_cap, LLM_WRAP_REGION, user_instr,
		    ctx_buf);
	} else {
		body_cap = strlen(LLM_WRAP_NOREGION) + strlen(user_instr) + 16;
		if ((user_msg = malloc(body_cap)) == NULL) {
			ewprintf("LLM: out of memory");
			goto done;
		}
		snprintf(user_msg, body_cap, LLM_WRAP_NOREGION, user_instr);
	}

	if ((sys_esc = json_escape(LLM_SYSTEM_PROMPT)) == NULL ||
	    (user_esc = json_escape(user_msg)) == NULL ||
	    (model_esc = json_escape(llm_model)) == NULL) {
		ewprintf("LLM: out of memory");
		goto done;
	}

	body_cap = strlen(sys_esc) + strlen(user_esc) + strlen(model_esc) +
	    256;
	if ((body = malloc(body_cap)) == NULL) {
		ewprintf("LLM: out of memory");
		goto done;
	}
	n = snprintf(body, body_cap,
	    "{\"model\":\"%s\","
	    "\"stream\":false,"
	    "\"messages\":["
	      "{\"role\":\"system\",\"content\":\"%s\"},"
	      "{\"role\":\"user\",\"content\":\"%s\"}"
	    "]}",
	    model_esc, sys_esc, user_esc);
	if (n < 0 || (size_t)n >= body_cap) {
		ewprintf("LLM: request too large");
		goto done;
	}
	body_len = (size_t)n;

	if (llm_pipe_curl(body, body_len, &raw, &raw_len) != TRUE)
		goto done;

	if ((content = json_extract_content(raw)) == NULL) {
		ewprintf("LLM: malformed response (no content field)");
		goto done;
	}

	*out = content;
	ret = TRUE;
done:
	free(ctx_buf);
	free(user_msg);
	free(sys_esc);
	free(user_esc);
	free(model_esc);
	free(body);
	free(raw);
	return (ret);
}

/*
 * Walk all lines of bp and concatenate them into a malloc'd buffer,
 * separating lines with '\n'.  Empty buffer returns "" (allocated).
 */
static char *
buffer_text(struct buffer *bp, size_t *outlen)
{
	struct line	*lp;
	char		*out;
	size_t		 total = 0, off = 0;
	int		 len;

	for (lp = bfirstlp(bp); lp != bp->b_headp; lp = lforw(lp)) {
		total += (size_t)llength(lp);
		if (lforw(lp) != bp->b_headp)
			total++;	/* implied newline */
	}
	if ((out = malloc(total + 1)) == NULL)
		return (NULL);
	for (lp = bfirstlp(bp); lp != bp->b_headp; lp = lforw(lp)) {
		len = llength(lp);
		if (len > 0)
			memcpy(out + off, ltext(lp), (size_t)len);
		off += (size_t)len;
		if (lforw(lp) != bp->b_headp)
			out[off++] = '\n';
	}
	out[off] = '\0';
	if (outlen != NULL)
		*outlen = off;
	return (out);
}

/*
 * Resolve a 1-based line number to a struct line * in bp.  Clamps to the
 * last line if the buffer has shrunk since the lineno was captured.
 */
static struct line *
lineno_to_lp(struct buffer *bp, int lineno)
{
	struct line	*lp = bfirstlp(bp);
	int		 i = 1;

	while (lp != bp->b_headp && i < lineno) {
		struct line *nx = lforw(lp);
		if (nx == bp->b_headp)
			break;
		lp = nx;
		i++;
	}
	return (lp);
}

/*
 * Returns TRUE if bp is still linked into the global buffer list.
 * Guards against the user killing the source buffer between the
 * compose-open call and the C-c C-c send.
 */
static int
buffer_in_list(struct buffer *bp)
{
	struct buffer	*b;

	if (bp == NULL)
		return (FALSE);
	for (b = bheadp; b != NULL; b = b->b_bufp)
		if (b == bp)
			return (TRUE);
	return (FALSE);
}

/*
 * Run the LLM query and replace the saved region.  Assumes curbp/curwp
 * are pointed at the source buffer and the saved dot/mark are valid.
 * Used by both the interactive (compose-send) and inmacro paths.
 */
static int
llm_run(const char *user_instr)
{
	struct region	 region;
	char		*ctx = NULL, *resp = NULL, *code;
	int		 has_region, ret = FALSE;
	size_t		 ctxlen, codelen, i;
	struct line	*lp;
	int		 off, total;

	if (llm_url == NULL || llm_url[0] == '\0' ||
	    llm_model == NULL || llm_model[0] == '\0') {
		dobeep();
		ewprintf("LLM: not configured; "
		    "use llm-set-url and llm-set-model");
		return (FALSE);
	}

	if (curbp->b_flag & BFREADONLY) {
		dobeep();
		ewprintf("Buffer is read-only");
		return (FALSE);
	}

	has_region = (curwp->w_markp != NULL);

	if (has_region) {
		/* Normalize: dot before mark. */
		if (curwp->w_markp == curwp->w_dotp &&
		    curwp->w_marko < curwp->w_doto) {
			lp = curwp->w_dotp;
			off = curwp->w_doto;
			i = curwp->w_dotline;
			curwp->w_dotp = curwp->w_markp;
			curwp->w_doto = curwp->w_marko;
			curwp->w_dotline = curwp->w_markline;
			curwp->w_markp = lp;
			curwp->w_marko = off;
			curwp->w_markline = (int)i;
		}
		region.r_linep = curwp->w_dotp;
		region.r_offset = curwp->w_doto;
		region.r_lineno = curwp->w_dotline;
		region.r_size = 0;

		lp = curwp->w_dotp;
		off = curwp->w_doto;
		total = 0;
		while (!(lp == curwp->w_markp && off == curwp->w_marko)) {
			if (off == llength(lp)) {
				lp = lforw(lp);
				if (lp == curbp->b_headp) {
					dobeep();
					ewprintf("LLM: lost mark");
					return (FALSE);
				}
				off = 0;
				total++;	/* newline */
			} else {
				off++;
				total++;
			}
		}
		region.r_size = (RSIZE)total;
	} else {
		region.r_linep = bfirstlp(curbp);
		region.r_offset = 0;
		region.r_lineno = 1;
		total = 0;
		for (lp = bfirstlp(curbp); lp != curbp->b_headp;
		    lp = lforw(lp)) {
			total += llength(lp);
			if (lforw(lp) != curbp->b_headp)
				total++;	/* implied newline */
		}
		region.r_size = (RSIZE)total;
	}

	ctxlen = (size_t)region.r_size;
	if ((ctx = malloc(ctxlen + 1)) == NULL) {
		dobeep();
		ewprintf("LLM: out of memory");
		return (FALSE);
	}
	if (ctxlen > 0)
		region_get_data(&region, ctx, (int)ctxlen);
	else
		ctx[0] = '\0';

	ewprintf("Querying LLM...");
	update(CMODE);

	if (llm_query(user_instr, ctx, ctxlen, has_region, &resp) != TRUE)
		goto done;

	code = llm_strip_fences(resp);
	codelen = strlen(code);

	curwp->w_dotp = region.r_linep;
	curwp->w_doto = region.r_offset;
	curwp->w_dotline = region.r_lineno;

	undo_boundary_enable(FFRAND, 0);
	if (ctxlen > 0) {
		if (ldelete((RSIZE)ctxlen, KNONE) == FALSE) {
			undo_boundary_enable(FFRAND, 1);
			ewprintf("LLM: delete failed");
			goto done;
		}
	}
	for (i = 0; i < codelen; i++) {
		if (code[i] == '\n') {
			if (lnewline() == FALSE) {
				undo_boundary_enable(FFRAND, 1);
				ewprintf("LLM: insert failed");
				goto done;
			}
		} else {
			if (linsert(1, (unsigned char)code[i]) == FALSE) {
				undo_boundary_enable(FFRAND, 1);
				ewprintf("LLM: insert failed");
				goto done;
			}
		}
	}
	undo_boundary_enable(FFRAND, 1);

	if (has_region)
		clearmark(FFRAND, 0);
	ewprintf("LLM: replaced %zu bytes with %zu bytes",
	    ctxlen, codelen);
	ret = TRUE;
done:
	free(ctx);
	free(resp);
	return (ret);
}

/*
 * M-x llm  --  pop up the *LLM Prompt* compose buffer.  Snapshots the
 * source buffer's region (if any) so the user can write a free-form,
 * multi-line prompt in a real buffer without the echo-line length cap.
 * C-c C-c (llm-compose-send) submits; C-c C-k (llm-compose-abort) cancels.
 *
 * When called from a macro / .mg interpreter, falls back to the legacy
 * single-line eread-style prompt so existing scripts keep working.
 */
int
llm(int f, int n)
{
	struct buffer	*src_bp, *prompt_bp;
	struct maps_s	*mode;
	char		 ibuf[BUFSIZE], *iprompt;

	if (llm_url == NULL || llm_url[0] == '\0' ||
	    llm_model == NULL || llm_model[0] == '\0') {
		dobeep();
		ewprintf("LLM: not configured; "
		    "use llm-set-url and llm-set-model");
		return (FALSE);
	}

	if (inmacro) {
		if (read_macro_arg(ibuf, sizeof(ibuf)) != TRUE)
			return (FALSE);
		iprompt = ibuf;
		if (iprompt[0] == '\0')
			return (FALSE);
		return (llm_run(iprompt));
	}

	if (curbp->b_flag & BFREADONLY) {
		dobeep();
		ewprintf("Buffer is read-only");
		return (FALSE);
	}

	src_bp = curbp;

	if ((prompt_bp = bfind(LLM_PROMPT_BUFNAME, TRUE)) == NULL) {
		dobeep();
		ewprintf("LLM: cannot create %s", LLM_PROMPT_BUFNAME);
		return (FALSE);
	}
	if (bclear(prompt_bp) != TRUE)
		return (FALSE);

	if ((mode = name_mode("llm-compose")) == NULL) {
		dobeep();
		ewprintf("LLM: llm-compose mode not registered");
		return (FALSE);
	}
	prompt_bp->b_modes[0] = name_mode("fundamental");
	prompt_bp->b_modes[1] = mode;
	prompt_bp->b_nmodes = 1;

	llm_session.active = TRUE;
	llm_session.src_bp = src_bp;
	llm_session.prompt_bp = prompt_bp;
	llm_session.has_region = (curwp->w_markp != NULL);
	llm_session.dot_lineno = curwp->w_dotline;
	llm_session.dot_offset = curwp->w_doto;
	llm_session.mark_lineno = curwp->w_markline;
	llm_session.mark_offset = curwp->w_marko;

	if (showbuffer(prompt_bp, curwp, WFFULL | WFFRAME | WFMODE) != TRUE) {
		llm_session.active = FALSE;
		return (FALSE);
	}
	curbp = prompt_bp;
	curwp->w_dotp = bfirstlp(prompt_bp);
	curwp->w_doto = 0;
	curwp->w_dotline = 1;
	curwp->w_markp = NULL;
	curwp->w_marko = 0;
	curwp->w_markline = 1;
	curwp->w_linep = curwp->w_dotp;

	ewprintf("LLM: type prompt; C-c C-c to send, C-c C-k to abort");
	return (TRUE);
}

/*
 * C-c C-c in *LLM Prompt*.  Read the prompt text from the compose
 * buffer, restore the source buffer + saved dot/mark, kill the
 * compose buffer, then run the query/replace.
 */
int
llm_compose_send(int f, int n)
{
	struct buffer	*src_bp, *prompt_bp;
	char		*prompt = NULL;
	size_t		 plen = 0;
	struct line	*dot_lp, *mark_lp;
	int		 ret;

	if (!llm_session.active || curbp != llm_session.prompt_bp) {
		dobeep();
		ewprintf("LLM: not in compose buffer");
		return (FALSE);
	}

	src_bp = llm_session.src_bp;
	prompt_bp = llm_session.prompt_bp;

	if (!buffer_in_list(src_bp)) {
		dobeep();
		ewprintf("LLM: source buffer is gone");
		llm_session.active = FALSE;
		return (FALSE);
	}

	if ((prompt = buffer_text(prompt_bp, &plen)) == NULL) {
		dobeep();
		ewprintf("LLM: out of memory");
		return (FALSE);
	}
	if (plen == 0) {
		free(prompt);
		dobeep();
		ewprintf("LLM: prompt is empty");
		return (FALSE);
	}

	if (showbuffer(src_bp, curwp, WFFULL | WFFRAME | WFMODE) != TRUE) {
		free(prompt);
		dobeep();
		ewprintf("LLM: cannot switch to source buffer");
		return (FALSE);
	}
	curbp = src_bp;

	dot_lp = lineno_to_lp(src_bp, llm_session.dot_lineno);
	curwp->w_dotp = dot_lp;
	curwp->w_doto = llm_session.dot_offset;
	if (curwp->w_doto > llength(dot_lp))
		curwp->w_doto = llength(dot_lp);
	curwp->w_dotline = llm_session.dot_lineno;

	if (llm_session.has_region) {
		mark_lp = lineno_to_lp(src_bp, llm_session.mark_lineno);
		curwp->w_markp = mark_lp;
		curwp->w_marko = llm_session.mark_offset;
		if (curwp->w_marko > llength(mark_lp))
			curwp->w_marko = llength(mark_lp);
		curwp->w_markline = llm_session.mark_lineno;
	} else {
		curwp->w_markp = NULL;
		curwp->w_marko = 0;
		curwp->w_markline = 1;
	}
	curwp->w_linep = dot_lp;

	llm_session.active = FALSE;
	llm_session.src_bp = NULL;
	llm_session.prompt_bp = NULL;

	(void)killbuffer(prompt_bp);

	ret = llm_run(prompt);
	free(prompt);
	return (ret);
}

/*
 * C-c C-k in *LLM Prompt*.  Discard the in-progress prompt, return
 * to the source buffer, and kill the compose buffer.
 */
int
llm_compose_abort(int f, int n)
{
	struct buffer	*src_bp, *prompt_bp;
	struct line	*dot_lp, *mark_lp;

	if (!llm_session.active || curbp != llm_session.prompt_bp) {
		dobeep();
		ewprintf("LLM: not in compose buffer");
		return (FALSE);
	}

	src_bp = llm_session.src_bp;
	prompt_bp = llm_session.prompt_bp;

	if (buffer_in_list(src_bp)) {
		if (showbuffer(src_bp, curwp, WFFULL | WFFRAME | WFMODE)
		    == TRUE) {
			curbp = src_bp;
			dot_lp = lineno_to_lp(src_bp,
			    llm_session.dot_lineno);
			curwp->w_dotp = dot_lp;
			curwp->w_doto = llm_session.dot_offset;
			if (curwp->w_doto > llength(dot_lp))
				curwp->w_doto = llength(dot_lp);
			curwp->w_dotline = llm_session.dot_lineno;
			if (llm_session.has_region) {
				mark_lp = lineno_to_lp(src_bp,
				    llm_session.mark_lineno);
				curwp->w_markp = mark_lp;
				curwp->w_marko = llm_session.mark_offset;
				if (curwp->w_marko > llength(mark_lp))
					curwp->w_marko = llength(mark_lp);
				curwp->w_markline = llm_session.mark_lineno;
			}
			curwp->w_linep = dot_lp;
		}
	}

	llm_session.active = FALSE;
	llm_session.src_bp = NULL;
	llm_session.prompt_bp = NULL;

	(void)killbuffer(prompt_bp);

	ewprintf("LLM: aborted");
	return (TRUE);
}

/*
 * Keymap for the *LLM Prompt* compose buffer.  Overrides only the
 * C-c prefix; everything else falls through (via rescan defaults)
 * to fundamental-mode, so normal editing keys work as expected.
 *
 *	C-c C-c	llm-compose-send
 *	C-c C-k	llm-compose-abort
 */
static PF llmcompose_cc[] = {
	llm_compose_send,	/* ^C */
	rescan,			/* ^D */
	rescan,			/* ^E */
	rescan,			/* ^F */
	rescan,			/* ^G */
	rescan,			/* ^H */
	rescan,			/* ^I */
	rescan,			/* ^J */
	llm_compose_abort,	/* ^K */
};

static struct KEYMAPE (1) llmcompose_ccmap = {
	1, 1, rescan,
	{
		{ CCHR('C'), CCHR('K'), llmcompose_cc, NULL }
	}
};

static PF llmcompose_top[] = {
	NULL,			/* ^C is a prefix for llmcompose_ccmap */
};

static struct KEYMAPE (1) llmcomposemap = {
	1, 1, rescan,
	{
		{ CCHR('C'), CCHR('C'), llmcompose_top,
		    (KEYMAP *)&llmcompose_ccmap }
	}
};

void
llm_init(void)
{
	funmap_add(llm_compose_send, "llm-compose-send", 0);
	funmap_add(llm_compose_abort, "llm-compose-abort", 0);
	maps_add((KEYMAP *)&llmcomposemap, "llm-compose");
}
