/* This file is in the public domain. */

/*
 * LLM commands.
 *
 * Two independent facilities live in this file:
 *
 * 1. M-x llm  --  one-shot region/coding assistant.
 *
 *	Pop up a *LLM Prompt* buffer and snapshot the source
 *	buffer's region (or dot position if no mark is set).
 *	The user composes a multi-line prompt in the compose
 *	buffer (no length cap, normal editing).  C-c C-c sends
 *	the prompt + saved context to a local OpenAI-compatible
 *	chat-completions endpoint via curl(1) and replaces the
 *	original region with the contents of the first fenced
 *	code block from the response.  C-c C-k aborts.
 *
 * 2. M-x llm-chat  --  interactive multi-turn chat.
 *
 *	Splits the current frame into a read-only *LLM Chat*
 *	transcript on top and an editable *LLM Input* pane on
 *	the bottom (~6 rows).  Type a prompt in the input pane
 *	and press C-c C-c to send.  Full message history is
 *	replayed to the server on every turn, so the model sees
 *	the whole conversation without any manual pasting.
 *
 *	Slash commands drive session management, entered in the
 *	input pane and dispatched by C-c C-c:
 *
 *	  /help              list slash commands
 *	  /reset             clear history + transcript
 *	  /compact           summarize history into a single
 *	                     system note (blocking)
 *	  /undo              drop last user+assistant turn
 *	  /system <text>     set the system prompt (takes
 *	                     effect on next /reset)
 *	  /model             list models advertised by the
 *	                     server's /v1/models endpoint
 *	  /model <name>      switch model for this session
 *	  /url <url>         switch endpoint for this session
 *	  /save <path>       write transcript buffer to file
 *	  /history           report message count and byte size
 *	  /clear             clear transcript display only
 *	  /quit              tear down both LLM buffers
 *
 *	A prompt whose first character is a literal '/' must be
 *	written with a leading '//' to escape the dispatcher.
 *
 *	Additional input-pane keybindings:
 *	  C-c C-c   send prompt (or dispatch slash command)
 *	  C-c C-l   clear input pane
 *	  C-c C-q   quit chat (== /quit)
 *
 * 3. Endpoint configuration (shared by both facilities):
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
#include <sys/stat.h>
#include <sys/wait.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "def.h"
#include "kbd.h"
#include "funmap.h"
#include "macro.h"

#define LLM_PROMPT_BUFNAME		"*LLM Prompt*"
#define LLM_CHAT_BUFNAME		"*LLM Chat*"
#define LLM_CHAT_INPUT_BUFNAME		"*LLM Input*"
#define LLM_CHAT_INPUT_ROWS		6

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

#define LLM_COMPACT_INSTRUCTION \
	"Summarize the conversation above as a single compact system " \
	"note. Preserve all decisions, code, constraints, and open " \
	"questions. Output only the note, no preamble."

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
	int		 mark_was_active;
	int		 dot_lineno;
	int		 dot_offset;
	int		 mark_lineno;
	int		 mark_offset;
} llm_session;

/*
 * Multi-turn chat session state.  The message list is the source of
 * truth for what goes on the wire; the visible transcript in log_bp
 * is presentation only.  Single session -- second M-x llm-chat just
 * re-focuses the input pane.
 */
struct llm_msg {
	char		*role;		/* "system" | "user" | "assistant" */
	char		*content;
	char		*content_esc;	/* lazily filled JSON-escaped content */
	struct llm_msg	*next;
};

static struct {
	int		 active;
	int		 busy;		/* re-entrancy guard around curl */
	struct buffer	*log_bp;
	struct buffer	*input_bp;
	struct llm_msg	*history;	/* oldest first */
	struct llm_msg	*history_tail;	/* fast O(1) append */
	char		*system_prompt;	/* NULL == no system prompt */
	char		*model_override;
	char		*url_override;
} llm_chat;

static int		 llm_set_str(char **, const char *);
static char		*json_escape(const char *);
static int		 json_read_string(const char **, char **);
static char		*json_extract_content(const char *);
static int		 json_extract_model_ids(const char *, char ***,
			     size_t *);
static char		*llm_strip_fences(char *);
static int		 llm_pipe_curl(const char *, const char *, size_t,
			     char **, size_t *);
static int		 llm_query(const char *, const char *, size_t, int,
			     char **);
static int		 read_macro_arg(char *, size_t);
static int		 llm_run(const char *);
static char		*buffer_text(struct buffer *, size_t *);
static struct line	*lineno_to_lp(struct buffer *, int);
static int		 buffer_in_list(struct buffer *);

static void		 chat_free_history(void);
static int		 chat_push_msg(const char *, const char *, size_t);
static int		 chat_pop_last_pair(void);
static int		 chat_add_line(struct buffer *, const char *,
			     size_t);
static int		 chat_add_separator(struct buffer *);
static int		 chat_append_block(struct buffer *, const char *,
			     const char *, size_t);
static void		 chat_scroll_bp_to_end(struct buffer *);
static void		 chat_focus_input(void);
static const char	*chat_effective_url(void);
static const char	*chat_effective_model(void);
static char		*chat_derive_models_url(const char *);
static int		 chat_build_body(char **, size_t *,
			     const char *, const char *);
static int		 chat_send_history(char **);
static int		 chat_render_history(void);
static int		 chat_dispatch_slash(const char *);
static int		 chat_layout(void);

static int		 slash_help(const char *);
static int		 slash_reset(const char *);
static int		 slash_compact(const char *);
static int		 slash_undo(const char *);
static int		 slash_system(const char *);
static int		 slash_model(const char *);
static int		 slash_url(const char *);
static int		 slash_save(const char *);
static int		 slash_history_cmd(const char *);
static int		 slash_clear(const char *);
static int		 slash_quit(const char *);

struct slash_cmd {
	const char	*name;
	int		 (*handler)(const char *);
	const char	*help;
};

static const struct slash_cmd slash_cmds[] = {
	{ "help",    slash_help,        "list slash commands" },
	{ "reset",   slash_reset,       "clear history and transcript" },
	{ "compact", slash_compact,
	    "summarize history into a single system note" },
	{ "undo",    slash_undo,        "drop last user+assistant turn" },
	{ "system",  slash_system,
	    "/system <text>  set system prompt (applies on next /reset)" },
	{ "model",   slash_model,
	    "/model          list server models\n"
	    "                /model <name>   switch model" },
	{ "url",     slash_url,         "/url <url>      switch endpoint" },
	{ "save",    slash_save,        "/save <path>    write transcript" },
	{ "history", slash_history_cmd, "report message count and size" },
	{ "clear",   slash_clear,       "clear transcript display only" },
	{ "quit",    slash_quit,        "tear down LLM buffers" },
	{ NULL, NULL, NULL }
};

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
 * Read a JSON string starting at *p (which must be pointing at the
 * opening '"').  Advances *p past the closing '"' and returns a
 * malloc'd decoded string via *out.  Returns TRUE on success, FALSE
 * on malformed input or OOM.
 */
static int
json_read_string(const char **pp, char **out)
{
	const char	*p;
	char		*buf, *q;
	size_t		 cap;
	unsigned int	 codepoint;
	int		 i;

	*out = NULL;
	p = *pp;
	if (*p != '"')
		return (FALSE);
	p++;

	cap = strlen(p) + 1;
	if ((buf = malloc(cap)) == NULL)
		return (FALSE);
	q = buf;

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
					free(buf);
					return (FALSE);
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
			free(buf);
			return (FALSE);
		}
	}
	if (*p != '"') {
		free(buf);
		return (FALSE);
	}
	*q = '\0';
	p++;
	*pp = p;
	*out = buf;
	return (TRUE);
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
	char		*out = NULL;

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
	if (json_read_string(&p, &out) != TRUE)
		return (NULL);
	return (out);
}

/*
 * Extract every "id" string from the JSON models-list response
 * (OpenAI shape: {"data":[{"id":"..."}, ...]}).  Returns TRUE on
 * success with *ids as a malloc'd array of malloc'd strings and *n
 * set to the count.  Returns FALSE if the shape can't be parsed.
 */
static int
json_extract_model_ids(const char *raw, char ***ids, size_t *n)
{
	const char	*p, *end;
	char		**arr = NULL, **narr;
	size_t		 cap = 0, cnt = 0;
	char		*val = NULL;
	int		 depth;

	*ids = NULL;
	*n = 0;

	p = strstr(raw, "\"data\"");
	if (p == NULL)
		return (FALSE);
	p += strlen("\"data\"");
	while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
		p++;
	if (*p != ':')
		return (FALSE);
	p++;
	while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
		p++;
	if (*p != '[')
		return (FALSE);
	p++;

	/* find the matching ']' honoring nested brackets (crudely). */
	depth = 1;
	end = p;
	while (*end != '\0' && depth > 0) {
		if (*end == '"') {
			/* skip a JSON string */
			end++;
			while (*end != '\0' && *end != '"') {
				if (*end == '\\' && end[1] != '\0')
					end += 2;
				else
					end++;
			}
			if (*end == '"')
				end++;
			continue;
		}
		if (*end == '[')
			depth++;
		else if (*end == ']')
			depth--;
		if (depth > 0)
			end++;
	}
	if (depth != 0)
		return (FALSE);

	while (p < end) {
		const char *k = strstr(p, "\"id\"");
		if (k == NULL || k >= end)
			break;
		p = k + strlen("\"id\"");
		while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
			p++;
		if (*p != ':')
			continue;
		p++;
		while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
			p++;
		if (*p != '"')
			continue;
		if (json_read_string(&p, &val) != TRUE)
			continue;
		if (cnt + 1 > cap) {
			cap = cap ? cap * 2 : 8;
			if ((narr = reallocarray(arr, cap,
			    sizeof(*arr))) == NULL) {
				free(val);
				goto oom;
			}
			arr = narr;
		}
		arr[cnt++] = val;
		val = NULL;
	}

	*ids = arr;
	*n = cnt;
	return (TRUE);
oom:
	while (cnt > 0)
		free(arr[--cnt]);
	free(arr);
	return (FALSE);
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
 * Fork curl(1) with stdout piped back to us.  If body != NULL, the
 * request is a POST with the body staged in a private tmpfile (avoids
 * stdin-pipe deadlocks on large requests and keeps payload off argv).
 * If body == NULL, a bare GET is issued to url.  API key, if present,
 * is passed via -H on argv (visible via ps(1) -- acceptable for
 * local-only endpoints).
 */
static int
llm_pipe_curl(const char *url, const char *body, size_t bodylen,
    char **out, size_t *outlen)
{
	char		 tmpl[] = "/tmp/mg-llm-XXXXXX";
	char		 auth[1024];
	char		 dataarg[64];
	char		*argv[16];
	char		*resp = NULL;
	size_t		 resp_cap = 0, resp_len = 0;
	ssize_t		 nw, nr;
	size_t		 left;
	int		 fd = -1, p[2], argc, status;
	int		 have_body = (body != NULL);
	pid_t		 pid;

	*out = NULL;
	*outlen = 0;

	if (url == NULL || url[0] == '\0') {
		ewprintf("LLM: no URL configured");
		return (FALSE);
	}

	if (have_body) {
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
				ewprintf("LLM: write tmp: %s",
				    strerror(errno));
				return (FALSE);
			}
			left -= (size_t)nw;
		}
		close(fd);
	}

	if (pipe(p) == -1) {
		if (have_body)
			unlink(tmpl);
		ewprintf("LLM: pipe: %s", strerror(errno));
		return (FALSE);
	}

	if ((pid = fork()) == -1) {
		close(p[0]); close(p[1]);
		if (have_body)
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

		argc = 0;
		argv[argc++] = "curl";
		argv[argc++] = "-sS";
		if (llm_key != NULL && llm_key[0] != '\0') {
			snprintf(auth, sizeof(auth),
			    "Authorization: Bearer %s", llm_key);
			argv[argc++] = "-H";
			argv[argc++] = auth;
		}
		if (have_body) {
			argv[argc++] = "-X";
			argv[argc++] = "POST";
			argv[argc++] = "-H";
			argv[argc++] = "Content-Type: application/json";
			argv[argc++] = (char *)url;
			argv[argc++] = "--data-binary";
			snprintf(dataarg, sizeof(dataarg), "@%s", tmpl);
			argv[argc++] = dataarg;
		} else {
			argv[argc++] = (char *)url;
		}
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
				if (have_body)
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
	if (have_body)
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

	if (llm_pipe_curl(llm_url, body, body_len, &raw, &raw_len) != TRUE)
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
 * Insert a byte string at dot.  Delegates to linsert() to allocate
 * space, records a single undo entry, and updates window state --
 * then overwrites the placeholder bytes with the actual content.
 * Must not contain '\n'; callers split segments and interleave
 * lnewline() themselves.
 */
static int
linsert_bytes(const char *s, size_t len)
{
	struct line	*lp;
	int		 doto;

	if (len == 0)
		return (TRUE);
	if (linsert((int)len, ' ') == FALSE)
		return (FALSE);
	lp = curwp->w_dotp;
	doto = curwp->w_doto;
	/* the len bytes just inserted sit at [doto - len .. doto - 1] */
	memcpy(&lp->l_text[doto - (int)len], s, len);
	return (TRUE);
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
	{
		size_t	start = 0, j;

		for (j = 0; j <= codelen; j++) {
			if (j < codelen && code[j] != '\n')
				continue;
			if (j > start) {
				if (linsert_bytes(code + start,
				    j - start) == FALSE) {
					undo_boundary_enable(FFRAND, 1);
					ewprintf("LLM: insert failed");
					goto done;
				}
			}
			if (j < codelen) {
				if (lnewline() == FALSE) {
					undo_boundary_enable(FFRAND, 1);
					ewprintf("LLM: insert failed");
					goto done;
				}
			}
			start = j + 1;
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
	llm_session.mark_was_active = curwp->w_markactive;
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
	mark_clear_wp(curwp);
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
		curwp->w_markactive = llm_session.mark_was_active;
		curwp->w_rflag |= WFFULL;
	} else {
		mark_clear_wp(curwp);
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
				curwp->w_markactive =
				    llm_session.mark_was_active;
				curwp->w_rflag |= WFFULL;
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

/* ------------------------------------------------------------------- */
/* M-x llm-chat -- interactive multi-turn chat session                  */
/* ------------------------------------------------------------------- */

/*
 * Free every message in the history list.  Idempotent.
 */
static void
chat_free_history(void)
{
	struct llm_msg	*m, *nx;

	m = llm_chat.history;
	while (m != NULL) {
		nx = m->next;
		free(m->role);
		free(m->content);
		free(m->content_esc);
		free(m);
		m = nx;
	}
	llm_chat.history = NULL;
	llm_chat.history_tail = NULL;
}

/*
 * Append a message to the history list.  content_len is the byte count
 * of content; the caller doesn't need to NUL-terminate.
 */
static int
chat_push_msg(const char *role, const char *content, size_t content_len)
{
	struct llm_msg	*m;

	if ((m = calloc(1, sizeof(*m))) == NULL)
		return (FALSE);
	if ((m->role = strdup(role)) == NULL) {
		free(m);
		return (FALSE);
	}
	if ((m->content = malloc(content_len + 1)) == NULL) {
		free(m->role);
		free(m);
		return (FALSE);
	}
	if (content_len > 0)
		memcpy(m->content, content, content_len);
	m->content[content_len] = '\0';
	/* content_esc left NULL; filled lazily by chat_build_body */

	if (llm_chat.history_tail == NULL) {
		llm_chat.history = m;
		llm_chat.history_tail = m;
	} else {
		llm_chat.history_tail->next = m;
		llm_chat.history_tail = m;
	}
	return (TRUE);
}

/*
 * Drop the trailing {user, assistant} pair from history.  Returns TRUE
 * if a pair was removed, FALSE otherwise.
 */
static int
chat_pop_last_pair(void)
{
	struct llm_msg	*cur, *prev, *last, *penult;

	last = NULL;
	penult = NULL;
	for (cur = llm_chat.history; cur != NULL; cur = cur->next) {
		penult = last;
		last = cur;
	}
	if (last == NULL || penult == NULL)
		return (FALSE);
	if (strcmp(last->role, "assistant") != 0 ||
	    strcmp(penult->role, "user") != 0)
		return (FALSE);

	prev = NULL;
	for (cur = llm_chat.history; cur != penult; cur = cur->next)
		prev = cur;

	if (prev == NULL) {
		llm_chat.history = NULL;
		llm_chat.history_tail = NULL;
	} else {
		prev->next = NULL;
		llm_chat.history_tail = prev;
	}

	free(penult->role); free(penult->content); free(penult->content_esc);
	free(penult);
	free(last->role); free(last->content); free(last->content_esc);
	free(last);
	return (TRUE);
}

/*
 * Append a raw line (no embedded newlines) to bp.  Bypasses linsert()
 * so it works transparently on BFREADONLY buffers, mirroring
 * addlinef() in buffer.c.
 */
static int
chat_add_line(struct buffer *bp, const char *s, size_t len)
{
	struct line	*lp;

	if ((lp = lalloc((int)len)) == NULL)
		return (FALSE);
	if (len > 0)
		memcpy(lp->l_text, s, len);
	lp->l_used = (int)len;

	bp->b_headp->l_bp->l_fp = lp;
	lp->l_bp = bp->b_headp->l_bp;
	bp->b_headp->l_bp = lp;
	lp->l_fp = bp->b_headp;
	bp->b_lines++;
	return (TRUE);
}

/*
 * Emit three lines of '#' spanning the current frame width, as a
 * visual delimiter between chat blocks.
 */
static int
chat_add_separator(struct buffer *bp)
{
	char	buf[512];
	size_t	w;
	int	i;

	w = (ncol > 0) ? (size_t)ncol : 40;
	if (w > sizeof(buf))
		w = sizeof(buf);
	memset(buf, '#', w);
	for (i = 0; i < 3; i++) {
		if (chat_add_line(bp, buf, w) != TRUE)
			return (FALSE);
	}
	return (TRUE);
}

/*
 * Append a labelled block to bp:  "<prefix> <first_line>\n<continuation>\n..."
 * with a hash-line separator at the end.  Handles embedded newlines in text.
 */
static int
chat_append_block(struct buffer *bp, const char *prefix,
    const char *text, size_t len)
{
	size_t		 i, start;
	int		 first = TRUE;
	char		 head[128];
	size_t		 plen;
	char		*buf = NULL;
	size_t		 buflen;

	plen = snprintf(head, sizeof(head), "%s ", prefix);
	if (plen >= sizeof(head))
		plen = sizeof(head) - 1;

	start = 0;
	for (i = 0; i <= len; i++) {
		if (i == len || text[i] == '\n') {
			size_t seg = i - start;
			if (first) {
				buflen = plen + seg;
				if ((buf = malloc(buflen ? buflen : 1))
				    == NULL)
					return (FALSE);
				memcpy(buf, head, plen);
				if (seg > 0)
					memcpy(buf + plen,
					    text + start, seg);
				if (chat_add_line(bp, buf, buflen) != TRUE) {
					free(buf);
					return (FALSE);
				}
				free(buf);
				first = FALSE;
			} else {
				if (chat_add_line(bp, text + start,
				    seg) != TRUE)
					return (FALSE);
			}
			start = i + 1;
			if (i == len)
				break;
		}
	}
	if (chat_add_separator(bp) != TRUE)
		return (FALSE);
	return (TRUE);
}

/*
 * For every window currently displaying bp, advance dot to the end of
 * the buffer and flag a full redraw so newly appended content is
 * visible.
 */
static void
chat_scroll_bp_to_end(struct buffer *bp)
{
	struct mgwin	*wp;
	struct line	*last = blastlp(bp);

	bp->b_dotp = last;
	bp->b_doto = llength(last);
	bp->b_dotline = bp->b_lines;

	for (wp = wheadp; wp != NULL; wp = wp->w_wndp) {
		if (wp->w_bufp != bp)
			continue;
		wp->w_dotp = last;
		wp->w_doto = llength(last);
		wp->w_dotline = bp->b_lines;
		wp->w_rflag |= WFFULL | WFMOVE;
	}
}

/*
 * Move focus to the input pane if it exists, and re-anchor every
 * window showing input_bp so its w_linep/w_dotp/w_markp point at
 * currently-valid lines (bclear frees the old ones).  Falls back to
 * no-op if input_bp is not shown anywhere.
 */
static void
chat_focus_input(void)
{
	struct mgwin	*wp;
	struct buffer	*bp;
	struct line	*last;

	if (llm_chat.input_bp == NULL)
		return;
	bp = llm_chat.input_bp;
	last = blastlp(bp);
	bp->b_dotp = last;
	bp->b_doto = llength(last);
	bp->b_dotline = bp->b_lines;
	bp->b_markp = NULL;
	bp->b_marko = 0;
	bp->b_markactive = 0;

	for (wp = wheadp; wp != NULL; wp = wp->w_wndp) {
		if (wp->w_bufp != bp)
			continue;
		wp->w_dotp = last;
		wp->w_doto = llength(last);
		wp->w_dotline = bp->b_lines;
		wp->w_linep = bfirstlp(bp);
		wp->w_markp = NULL;
		wp->w_marko = 0;
		wp->w_markline = 0;
		wp->w_markactive = 0;
		wp->w_rflag |= WFFULL | WFMOVE | WFMODE;
		if (curwp != wp && wp->w_bufp == llm_chat.input_bp) {
			curwp = wp;
			curbp = bp;
		}
	}
}

static const char *
chat_effective_url(void)
{
	if (llm_chat.url_override != NULL && llm_chat.url_override[0] != '\0')
		return (llm_chat.url_override);
	return (llm_url);
}

static const char *
chat_effective_model(void)
{
	if (llm_chat.model_override != NULL &&
	    llm_chat.model_override[0] != '\0')
		return (llm_chat.model_override);
	return (llm_model);
}

/*
 * Turn a chat-completions URL like
 *    http://host/v1/chat/completions
 * into the corresponding models URL:
 *    http://host/v1/models
 * Fallback: replace everything after the last '/' with "models".
 * Returns a malloc'd string, or NULL on failure.
 */
static char *
chat_derive_models_url(const char *chat_url)
{
	const char	*suffix = "/chat/completions";
	size_t		 sfxlen = strlen(suffix);
	size_t		 ulen;
	const char	*slash;
	char		*out;
	size_t		 pfx;

	if (chat_url == NULL || chat_url[0] == '\0')
		return (NULL);
	ulen = strlen(chat_url);
	if (ulen > sfxlen && strcmp(chat_url + ulen - sfxlen, suffix) == 0) {
		pfx = ulen - sfxlen;
		if ((out = malloc(pfx + strlen("/models") + 1)) == NULL)
			return (NULL);
		memcpy(out, chat_url, pfx);
		memcpy(out + pfx, "/models", strlen("/models") + 1);
		return (out);
	}
	slash = strrchr(chat_url, '/');
	if (slash == NULL)
		return (NULL);
	pfx = (size_t)(slash - chat_url) + 1;
	if ((out = malloc(pfx + strlen("models") + 1)) == NULL)
		return (NULL);
	memcpy(out, chat_url, pfx);
	memcpy(out + pfx, "models", strlen("models") + 1);
	return (out);
}

/*
 * Serialize llm_chat.history (optionally with the session's system
 * prompt prepended if not already present, and optionally with one
 * synthetic user message appended for /compact) into a JSON body.
 * Returns TRUE on success with body/bodylen owned by the caller.
 */
static int
chat_build_body(char **body, size_t *bodylen,
    const char *extra_role, const char *extra_content)
{
	char		*model_esc = NULL;
	struct llm_msg	*m;
	char		*buf = NULL;
	size_t		 cap = 4096, len = 0;
	const char	*model;
	int		 first = TRUE;
	int		 need_leading_system;

	*body = NULL;
	*bodylen = 0;

	model = chat_effective_model();
	if (model == NULL || model[0] == '\0') {
		ewprintf("LLM: no model configured");
		return (FALSE);
	}
	if ((model_esc = json_escape(model)) == NULL) {
		ewprintf("LLM: out of memory");
		return (FALSE);
	}

	if ((buf = malloc(cap)) == NULL) {
		free(model_esc);
		ewprintf("LLM: out of memory");
		return (FALSE);
	}

#define APPEND(str, slen)						\
	do {								\
		size_t _l = (slen);					\
		while (len + _l + 1 > cap) {				\
			size_t _n = cap * 2;				\
			char *_p = realloc(buf, _n);			\
			if (_p == NULL) { free(buf); free(model_esc);	\
			    ewprintf("LLM: out of memory");		\
			    return (FALSE); }				\
			buf = _p; cap = _n;				\
		}							\
		memcpy(buf + len, (str), _l);				\
		len += _l;						\
	} while (0)

#define APPEND_S(s) APPEND((s), strlen(s))

	APPEND_S("{\"model\":\"");
	APPEND_S(model_esc);
	APPEND_S("\",\"stream\":false,\"messages\":[");

	need_leading_system = (llm_chat.system_prompt != NULL &&
	    llm_chat.system_prompt[0] != '\0');
	if (need_leading_system && llm_chat.history != NULL &&
	    strcmp(llm_chat.history->role, "system") == 0)
		need_leading_system = 0;

	if (need_leading_system) {
		char *sys_esc = json_escape(llm_chat.system_prompt);
		if (sys_esc == NULL) {
			free(buf); free(model_esc);
			ewprintf("LLM: out of memory");
			return (FALSE);
		}
		APPEND_S("{\"role\":\"system\",\"content\":\"");
		APPEND_S(sys_esc);
		APPEND_S("\"}");
		free(sys_esc);
		first = FALSE;
	}

	for (m = llm_chat.history; m != NULL; m = m->next) {
		if (m->content_esc == NULL) {
			m->content_esc = json_escape(m->content);
			if (m->content_esc == NULL) {
				free(buf); free(model_esc);
				ewprintf("LLM: out of memory");
				return (FALSE);
			}
		}
		if (!first)
			APPEND_S(",");
		/*
		 * Role strings are always one of "system", "user",
		 * "assistant" -- no characters requiring JSON escaping,
		 * so emit them directly.
		 */
		APPEND_S("{\"role\":\"");
		APPEND_S(m->role);
		APPEND_S("\",\"content\":\"");
		APPEND_S(m->content_esc);
		APPEND_S("\"}");
		first = FALSE;
	}

	if (extra_role != NULL && extra_content != NULL) {
		char *r = json_escape(extra_role);
		char *c = json_escape(extra_content);
		if (r == NULL || c == NULL) {
			free(r); free(c);
			free(buf); free(model_esc);
			ewprintf("LLM: out of memory");
			return (FALSE);
		}
		if (!first)
			APPEND_S(",");
		APPEND_S("{\"role\":\"");
		APPEND_S(r);
		APPEND_S("\",\"content\":\"");
		APPEND_S(c);
		APPEND_S("\"}");
		free(r);
		free(c);
	}

	APPEND_S("]}");
	buf[len] = '\0';

#undef APPEND
#undef APPEND_S

	free(model_esc);
	*body = buf;
	*bodylen = len;
	return (TRUE);
}

/*
 * Send the current history to the endpoint and place the extracted
 * assistant reply in *reply (malloc'd).  Does NOT mutate history.
 */
static int
chat_send_history(char **reply)
{
	char		*body = NULL, *raw = NULL, *content = NULL;
	size_t		 bodylen, rawlen;
	const char	*url = chat_effective_url();

	*reply = NULL;

	if (url == NULL || url[0] == '\0') {
		ewprintf("LLM: not configured; use llm-set-url");
		return (FALSE);
	}
	if (chat_build_body(&body, &bodylen, NULL, NULL) != TRUE)
		return (FALSE);
	if (llm_pipe_curl(url, body, bodylen, &raw, &rawlen) != TRUE) {
		free(body);
		return (FALSE);
	}
	free(body);
	if ((content = json_extract_content(raw)) == NULL) {
		free(raw);
		ewprintf("LLM: malformed response (no content field)");
		return (FALSE);
	}
	free(raw);
	*reply = content;
	return (TRUE);
}

/*
 * Rewrite the entire chat log buffer from llm_chat.history.
 */
static int
chat_render_history(void)
{
	struct llm_msg	*m;
	struct buffer	*bp = llm_chat.log_bp;
	int		 saved_ro;

	if (bp == NULL)
		return (FALSE);
	saved_ro = bp->b_flag & BFREADONLY;
	bp->b_flag &= ~BFREADONLY;
	if (bclear(bp) != TRUE) {
		if (saved_ro)
			bp->b_flag |= BFREADONLY;
		return (FALSE);
	}
	if (saved_ro)
		bp->b_flag |= BFREADONLY;

	if (chat_add_line(bp,
	    "-- *LLM Chat* -- /help for slash commands --", 44) != TRUE)
		return (FALSE);
	if (chat_add_separator(bp) != TRUE)
		return (FALSE);

	for (m = llm_chat.history; m != NULL; m = m->next) {
		const char *prefix;
		if (strcmp(m->role, "user") == 0)
			prefix = "mg>";
		else if (strcmp(m->role, "assistant") == 0)
			prefix = "llm>";
		else
			prefix = "sys>";
		if (chat_append_block(bp, prefix, m->content,
		    strlen(m->content)) != TRUE)
			return (FALSE);
	}
	chat_scroll_bp_to_end(bp);
	return (TRUE);
}

/* -------- slash commands -------- */

static int
slash_help(const char *args)
{
	struct buffer		*bp = llm_chat.log_bp;
	const struct slash_cmd	*c;
	char			 line[256];
	int			 n;

	if (bp == NULL)
		return (FALSE);
	if (chat_add_line(bp, "sys> slash commands:", 20) != TRUE)
		return (FALSE);
	for (c = slash_cmds; c->name != NULL; c++) {
		n = snprintf(line, sizeof(line), "     /%-8s  %s",
		    c->name, c->help ? c->help : "");
		if (n < 0)
			return (FALSE);
		if ((size_t)n >= sizeof(line))
			n = (int)sizeof(line) - 1;
		if (chat_add_line(bp, line, (size_t)n) != TRUE)
			return (FALSE);
	}
	if (chat_add_line(bp,
	    "     (prepend a literal '/' with '//' to send it as text)",
	    56) != TRUE)
		return (FALSE);
	if (chat_add_separator(bp) != TRUE)
		return (FALSE);
	chat_scroll_bp_to_end(bp);
	return (TRUE);
}

static int
slash_reset(const char *args)
{
	chat_free_history();
	if (llm_chat.system_prompt != NULL &&
	    llm_chat.system_prompt[0] != '\0') {
		(void)chat_push_msg("system", llm_chat.system_prompt,
		    strlen(llm_chat.system_prompt));
	}
	if (chat_render_history() != TRUE)
		return (FALSE);
	ewprintf("LLM: session reset");
	return (TRUE);
}

static int
slash_compact(const char *args)
{
	char	*body = NULL, *raw = NULL, *summary = NULL;
	size_t	 bodylen, rawlen;
	const char *url = chat_effective_url();

	if (llm_chat.history == NULL) {
		ewprintf("LLM: nothing to compact");
		return (FALSE);
	}
	if (url == NULL || url[0] == '\0') {
		ewprintf("LLM: no URL configured");
		return (FALSE);
	}
	ewprintf("LLM: compacting...");
	update(CMODE);
	if (chat_build_body(&body, &bodylen, "user",
	    LLM_COMPACT_INSTRUCTION) != TRUE)
		return (FALSE);
	if (llm_pipe_curl(url, body, bodylen, &raw, &rawlen) != TRUE) {
		free(body);
		return (FALSE);
	}
	free(body);
	if ((summary = json_extract_content(raw)) == NULL) {
		free(raw);
		ewprintf("LLM: malformed response");
		return (FALSE);
	}
	free(raw);

	chat_free_history();
	if (chat_push_msg("system", summary, strlen(summary)) != TRUE) {
		free(summary);
		ewprintf("LLM: out of memory");
		return (FALSE);
	}
	free(summary);
	if (chat_render_history() != TRUE)
		return (FALSE);
	ewprintf("LLM: history compacted");
	return (TRUE);
}

static int
slash_undo(const char *args)
{
	if (chat_pop_last_pair() != TRUE) {
		dobeep();
		ewprintf("LLM: nothing to undo");
		return (FALSE);
	}
	if (chat_render_history() != TRUE)
		return (FALSE);
	ewprintf("LLM: last turn removed");
	return (TRUE);
}

static int
slash_system(const char *args)
{
	char	*copy;

	if (args == NULL || args[0] == '\0') {
		free(llm_chat.system_prompt);
		llm_chat.system_prompt = NULL;
		ewprintf("LLM: system prompt cleared (applies on /reset)");
		return (TRUE);
	}
	if ((copy = strdup(args)) == NULL) {
		ewprintf("LLM: out of memory");
		return (FALSE);
	}
	free(llm_chat.system_prompt);
	llm_chat.system_prompt = copy;
	ewprintf("LLM: system prompt set (applies on /reset)");
	return (TRUE);
}

static int
slash_model(const char *args)
{
	char		*murl = NULL, *raw = NULL, **ids = NULL;
	size_t		 rawlen, n, i;
	struct buffer	*bp = llm_chat.log_bp;
	const char	*current, *url;
	int		 ret = FALSE;
	char		 line[256];
	int		 wrote;

	if (args != NULL && args[0] != '\0') {
		char *copy = strdup(args);
		if (copy == NULL) {
			ewprintf("LLM: out of memory");
			return (FALSE);
		}
		free(llm_chat.model_override);
		llm_chat.model_override = copy;
		ewprintf("LLM: model set to '%s'", copy);
		return (TRUE);
	}

	url = chat_effective_url();
	if (url == NULL || url[0] == '\0') {
		ewprintf("LLM: no URL configured");
		return (FALSE);
	}
	if ((murl = chat_derive_models_url(url)) == NULL) {
		ewprintf("LLM: cannot derive models URL");
		return (FALSE);
	}
	ewprintf("LLM: fetching models...");
	update(CMODE);
	if (llm_pipe_curl(murl, NULL, 0, &raw, &rawlen) != TRUE) {
		free(murl);
		return (FALSE);
	}

	if (bp == NULL) {
		free(murl); free(raw);
		return (FALSE);
	}

	if (json_extract_model_ids(raw, &ids, &n) != TRUE || n == 0) {
		wrote = snprintf(line, sizeof(line),
		    "sys> models: cannot parse response from %s", murl);
		if (wrote < 0) wrote = 0;
		if ((size_t)wrote >= sizeof(line))
			wrote = (int)sizeof(line) - 1;
		chat_add_line(bp, line, (size_t)wrote);
		if (rawlen > 0)
			chat_append_block(bp, "sys>", raw, rawlen);
		chat_scroll_bp_to_end(bp);
		free(murl); free(raw);
		return (FALSE);
	}

	current = chat_effective_model();
	wrote = snprintf(line, sizeof(line), "sys> models available at %s:",
	    murl);
	if (wrote < 0) wrote = 0;
	if ((size_t)wrote >= sizeof(line)) wrote = (int)sizeof(line) - 1;
	chat_add_line(bp, line, (size_t)wrote);
	for (i = 0; i < n; i++) {
		const char *mark = (current != NULL &&
		    strcmp(current, ids[i]) == 0) ? "*" : " ";
		wrote = snprintf(line, sizeof(line), "     %s %s", mark,
		    ids[i]);
		if (wrote < 0) wrote = 0;
		if ((size_t)wrote >= sizeof(line))
			wrote = (int)sizeof(line) - 1;
		chat_add_line(bp, line, (size_t)wrote);
	}
	chat_add_separator(bp);
	chat_scroll_bp_to_end(bp);
	ret = TRUE;

	for (i = 0; i < n; i++)
		free(ids[i]);
	free(ids);
	free(murl);
	free(raw);
	return (ret);
}

static int
slash_url(const char *args)
{
	char	*copy;

	if (args == NULL || args[0] == '\0') {
		ewprintf("LLM: /url requires a URL");
		return (FALSE);
	}
	if ((copy = strdup(args)) == NULL) {
		ewprintf("LLM: out of memory");
		return (FALSE);
	}
	free(llm_chat.url_override);
	llm_chat.url_override = copy;
	ewprintf("LLM: endpoint set to '%s'", copy);
	return (TRUE);
}

static int
slash_save(const char *args)
{
	char	*text;
	size_t	 len;
	int	 fd;
	ssize_t	 nw;
	size_t	 left;

	if (args == NULL || args[0] == '\0') {
		ewprintf("LLM: /save requires a path");
		return (FALSE);
	}
	if (llm_chat.log_bp == NULL)
		return (FALSE);
	if ((text = buffer_text(llm_chat.log_bp, &len)) == NULL) {
		ewprintf("LLM: out of memory");
		return (FALSE);
	}
	fd = open(args, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd == -1) {
		free(text);
		ewprintf("LLM: open %s: %s", args, strerror(errno));
		return (FALSE);
	}
	left = len;
	while (left > 0) {
		nw = write(fd, text + (len - left), left);
		if (nw <= 0) {
			if (nw == -1 && errno == EINTR)
				continue;
			close(fd);
			free(text);
			ewprintf("LLM: write %s: %s", args, strerror(errno));
			return (FALSE);
		}
		left -= (size_t)nw;
	}
	close(fd);
	free(text);
	ewprintf("LLM: wrote %zu bytes to %s", len, args);
	return (TRUE);
}

static int
slash_history_cmd(const char *args)
{
	struct llm_msg	*m;
	size_t		 count = 0, bytes = 0;

	for (m = llm_chat.history; m != NULL; m = m->next) {
		count++;
		bytes += strlen(m->content);
	}
	ewprintf("LLM: %zu message%s, %zu content bytes",
	    count, count == 1 ? "" : "s", bytes);
	return (TRUE);
}

static int
slash_clear(const char *args)
{
	struct buffer	*bp = llm_chat.log_bp;
	int		 saved_ro;

	if (bp == NULL)
		return (FALSE);
	saved_ro = bp->b_flag & BFREADONLY;
	bp->b_flag &= ~BFREADONLY;
	if (bclear(bp) != TRUE) {
		if (saved_ro)
			bp->b_flag |= BFREADONLY;
		return (FALSE);
	}
	if (saved_ro)
		bp->b_flag |= BFREADONLY;
	chat_add_line(bp,
	    "-- *LLM Chat* -- transcript cleared (history preserved) --",
	    58);
	chat_add_separator(bp);
	chat_scroll_bp_to_end(bp);
	ewprintf("LLM: transcript cleared");
	return (TRUE);
}

static int
slash_quit(const char *args)
{
	struct buffer	*log_bp = llm_chat.log_bp;
	struct buffer	*input_bp = llm_chat.input_bp;

	chat_free_history();
	free(llm_chat.system_prompt);
	free(llm_chat.model_override);
	free(llm_chat.url_override);
	llm_chat.system_prompt = NULL;
	llm_chat.model_override = NULL;
	llm_chat.url_override = NULL;
	llm_chat.log_bp = NULL;
	llm_chat.input_bp = NULL;
	llm_chat.active = FALSE;
	llm_chat.busy = FALSE;

	/* Focus somewhere sane before killing buffers. */
	if (curbp == input_bp || curbp == log_bp) {
		struct buffer	*fallback = NULL;
		struct buffer	*b;
		for (b = bheadp; b != NULL; b = b->b_bufp) {
			if (b != log_bp && b != input_bp &&
			    b->b_bname[0] != '*') {
				fallback = b;
				break;
			}
		}
		if (fallback == NULL) {
			for (b = bheadp; b != NULL; b = b->b_bufp) {
				if (b != log_bp && b != input_bp) {
					fallback = b;
					break;
				}
			}
		}
		if (fallback != NULL) {
			(void)showbuffer(fallback, curwp,
			    WFFULL | WFFRAME | WFMODE);
			curbp = fallback;
		}
	}

	if (input_bp != NULL && buffer_in_list(input_bp))
		(void)killbuffer(input_bp);
	if (log_bp != NULL && buffer_in_list(log_bp))
		(void)killbuffer(log_bp);
	(void)onlywind(FFRAND, 1);

	ewprintf("LLM: chat session ended");
	return (TRUE);
}

/*
 * Parse and dispatch a slash-command line.  raw points at the first
 * '/' of the input.  Returns TRUE on any dispatch attempt (success or
 * clean failure); FALSE only on structural error.
 */
static int
chat_dispatch_slash(const char *raw)
{
	const char		*p, *args;
	char			 name[32];
	size_t			 nlen;
	const struct slash_cmd	*c;

	if (raw == NULL || raw[0] != '/')
		return (FALSE);
	p = raw + 1;
	nlen = 0;
	while (*p != '\0' && *p != ' ' && *p != '\t' && *p != '\n' &&
	    nlen + 1 < sizeof(name)) {
		name[nlen++] = *p++;
	}
	name[nlen] = '\0';
	while (*p == ' ' || *p == '\t')
		p++;
	args = p;
	/* Strip trailing whitespace from args (in a local copy). */
	if (*args != '\0') {
		size_t	alen = strlen(args);
		while (alen > 0 && (args[alen - 1] == ' ' ||
		    args[alen - 1] == '\t' || args[alen - 1] == '\n' ||
		    args[alen - 1] == '\r'))
			alen--;
		char *acopy = malloc(alen + 1);
		if (acopy == NULL) {
			ewprintf("LLM: out of memory");
			return (FALSE);
		}
		memcpy(acopy, args, alen);
		acopy[alen] = '\0';
		for (c = slash_cmds; c->name != NULL; c++) {
			if (strcmp(c->name, name) == 0) {
				int rc = c->handler(acopy);
				free(acopy);
				return (rc);
			}
		}
		free(acopy);
	} else {
		for (c = slash_cmds; c->name != NULL; c++) {
			if (strcmp(c->name, name) == 0)
				return (c->handler(""));
		}
	}
	dobeep();
	ewprintf("LLM: unknown command /%s (try /help)", name);
	return (FALSE);
}

/*
 * Split the current window and lay out log_bp on top, input_bp on the
 * bottom sized to LLM_CHAT_INPUT_ROWS.  Assumes both buffers exist.
 */
static int
chat_layout(void)
{
	struct mgwin	*upper;
	int		 target = LLM_CHAT_INPUT_ROWS;
	int		 delta;

	if (onlywind(FFRAND, 1) != TRUE)
		return (FALSE);
	if (showbuffer(llm_chat.log_bp, curwp,
	    WFFULL | WFFRAME | WFMODE) != TRUE)
		return (FALSE);
	curbp = llm_chat.log_bp;
	curwp->w_dotp = blastlp(curbp);
	curwp->w_doto = llength(curwp->w_dotp);
	curwp->w_dotline = curbp->b_lines;
	curwp->w_linep = bfirstlp(curbp);

	upper = curwp;
	if (splitwind(FFRAND, 1) != TRUE)
		return (FALSE);
	/* After splitwind, curwp is still the upper window; move to lower. */
	if (nextwind(FFRAND, 1) != TRUE)
		return (FALSE);
	if (curwp == upper) {
		/* only one window remained -- split failed silently */
		return (FALSE);
	}
	if (showbuffer(llm_chat.input_bp, curwp,
	    WFFULL | WFFRAME | WFMODE) != TRUE)
		return (FALSE);
	curbp = llm_chat.input_bp;
	curwp->w_dotp = bfirstlp(curbp);
	curwp->w_doto = 0;
	curwp->w_dotline = 1;
	curwp->w_linep = curwp->w_dotp;
	mark_clear_wp(curwp);

	/* Resize input pane toward target rows. */
	delta = curwp->w_ntrows - target;
	if (delta > 0)
		(void)shrinkwind(FFRAND, delta);
	else if (delta < 0)
		(void)enlargewind(FFRAND, -delta);

	return (TRUE);
}

/*
 * M-x llm-chat  --  open (or re-focus) the interactive chat session.
 */
int
llm_chat_cmd(int f, int n)
{
	struct maps_s	*chat_mode;
	int		 fresh;

	if (chat_effective_url() == NULL || chat_effective_url()[0] == '\0' ||
	    chat_effective_model() == NULL ||
	    chat_effective_model()[0] == '\0') {
		if (llm_url == NULL || llm_url[0] == '\0' ||
		    llm_model == NULL || llm_model[0] == '\0') {
			dobeep();
			ewprintf("LLM: not configured; "
			    "use llm-set-url and llm-set-model");
			return (FALSE);
		}
	}

	if ((chat_mode = name_mode("llm-chat")) == NULL) {
		dobeep();
		ewprintf("LLM: llm-chat mode not registered");
		return (FALSE);
	}

	fresh = !llm_chat.active ||
	    !buffer_in_list(llm_chat.log_bp) ||
	    !buffer_in_list(llm_chat.input_bp);

	if (fresh) {
		if (llm_chat.log_bp != NULL && buffer_in_list(llm_chat.log_bp))
			(void)killbuffer(llm_chat.log_bp);
		if (llm_chat.input_bp != NULL &&
		    buffer_in_list(llm_chat.input_bp))
			(void)killbuffer(llm_chat.input_bp);
		chat_free_history();

		llm_chat.log_bp = bfind(LLM_CHAT_BUFNAME, TRUE);
		llm_chat.input_bp = bfind(LLM_CHAT_INPUT_BUFNAME, TRUE);
		if (llm_chat.log_bp == NULL || llm_chat.input_bp == NULL) {
			ewprintf("LLM: cannot create chat buffers");
			return (FALSE);
		}
		(void)bclear(llm_chat.log_bp);
		(void)bclear(llm_chat.input_bp);

		llm_chat.log_bp->b_flag |= BFREADONLY;
		llm_chat.input_bp->b_modes[0] = name_mode("fundamental");
		llm_chat.input_bp->b_modes[1] = chat_mode;
		llm_chat.input_bp->b_nmodes = 1;

		chat_add_line(llm_chat.log_bp,
		    "-- *LLM Chat* -- /help for slash commands --", 44);
		chat_add_separator(llm_chat.log_bp);

		if (llm_chat.system_prompt != NULL &&
		    llm_chat.system_prompt[0] != '\0') {
			(void)chat_push_msg("system", llm_chat.system_prompt,
			    strlen(llm_chat.system_prompt));
			chat_render_history();
		}

		llm_chat.active = TRUE;
		llm_chat.busy = FALSE;
	}

	if (chat_layout() != TRUE) {
		ewprintf("LLM: cannot lay out chat windows");
		return (FALSE);
	}

	ewprintf("LLM: chat ready; C-c C-c sends, /help for commands");
	return (TRUE);
}

/*
 * C-c C-c in *LLM Input*.  Read the pending prompt, dispatch slash
 * commands, or send to the model and append the reply to the log.
 */
int
llm_chat_send(int f, int n)
{
	char		*prompt = NULL, *reply = NULL;
	char		*send_text;
	size_t		 plen;
	int		 ret = FALSE;

	if (!llm_chat.active || curbp != llm_chat.input_bp) {
		dobeep();
		ewprintf("LLM: not in *LLM Input*");
		return (FALSE);
	}
	if (llm_chat.busy) {
		dobeep();
		ewprintf("LLM: request in progress");
		return (FALSE);
	}
	if ((prompt = buffer_text(llm_chat.input_bp, &plen)) == NULL) {
		ewprintf("LLM: out of memory");
		return (FALSE);
	}
	/* trim trailing whitespace */
	while (plen > 0 && (prompt[plen - 1] == ' ' ||
	    prompt[plen - 1] == '\t' || prompt[plen - 1] == '\n' ||
	    prompt[plen - 1] == '\r')) {
		plen--;
	}
	prompt[plen] = '\0';
	/* trim leading whitespace (find start) */
	send_text = prompt;
	while (*send_text == ' ' || *send_text == '\t' ||
	    *send_text == '\n' || *send_text == '\r')
		send_text++;

	if (*send_text == '\0') {
		dobeep();
		ewprintf("LLM: empty prompt");
		free(prompt);
		return (FALSE);
	}

	if (send_text[0] == '/' && send_text[1] != '/') {
		ret = chat_dispatch_slash(send_text);
		/* Only clear the input pane if the session survived. */
		if (llm_chat.active && llm_chat.input_bp != NULL &&
		    buffer_in_list(llm_chat.input_bp)) {
			(void)bclear(llm_chat.input_bp);
			chat_focus_input();
		}
		free(prompt);
		return (ret);
	}
	if (send_text[0] == '/' && send_text[1] == '/')
		send_text++;	/* consume one leading slash */

	if (chat_push_msg("user", send_text, strlen(send_text)) != TRUE) {
		free(prompt);
		ewprintf("LLM: out of memory");
		return (FALSE);
	}
	if (chat_append_block(llm_chat.log_bp, "mg>", send_text,
	    strlen(send_text)) != TRUE) {
		free(prompt);
		ewprintf("LLM: cannot append to log");
		return (FALSE);
	}
	chat_scroll_bp_to_end(llm_chat.log_bp);

	llm_chat.busy = TRUE;
	ewprintf("LLM: querying...");
	update(CMODE);

	if (chat_send_history(&reply) != TRUE) {
		llm_chat.busy = FALSE;
		free(prompt);
		return (FALSE);
	}
	llm_chat.busy = FALSE;

	if (chat_push_msg("assistant", reply, strlen(reply)) != TRUE) {
		free(reply);
		free(prompt);
		ewprintf("LLM: out of memory");
		return (FALSE);
	}
	(void)chat_append_block(llm_chat.log_bp, "llm>", reply,
	    strlen(reply));
	chat_scroll_bp_to_end(llm_chat.log_bp);
	free(reply);

	(void)bclear(llm_chat.input_bp);
	chat_focus_input();
	ewprintf("LLM: reply received");
	free(prompt);
	return (TRUE);
}

/*
 * C-c C-l in *LLM Input*.  Clear the pending prompt.
 */
int
llm_chat_clear_input(int f, int n)
{
	if (!llm_chat.active || curbp != llm_chat.input_bp) {
		dobeep();
		ewprintf("LLM: not in *LLM Input*");
		return (FALSE);
	}
	(void)bclear(llm_chat.input_bp);
	chat_focus_input();
	ewprintf("LLM: input cleared");
	return (TRUE);
}

/*
 * C-c C-q in *LLM Input*.  Same as /quit.
 */
int
llm_chat_quit(int f, int n)
{
	if (!llm_chat.active) {
		dobeep();
		ewprintf("LLM: no chat session");
		return (FALSE);
	}
	return (slash_quit(""));
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

/*
 * Keymap for the *LLM Input* buffer.
 *
 *	C-c C-c	llm-chat-send
 *	C-c C-l	llm-chat-clear-input
 *	C-c C-q	llm-chat-quit
 */
static PF llmchat_cc[] = {
	llm_chat_send,		/* ^C */
	rescan,			/* ^D */
	rescan,			/* ^E */
	rescan,			/* ^F */
	rescan,			/* ^G */
	rescan,			/* ^H */
	rescan,			/* ^I */
	rescan,			/* ^J */
	rescan,			/* ^K */
	llm_chat_clear_input,	/* ^L */
	rescan,			/* ^M */
	rescan,			/* ^N */
	rescan,			/* ^O */
	rescan,			/* ^P */
	llm_chat_quit,		/* ^Q */
};

static struct KEYMAPE (1) llmchat_ccmap = {
	1, 1, rescan,
	{
		{ CCHR('C'), CCHR('Q'), llmchat_cc, NULL }
	}
};

static PF llmchat_top[] = {
	NULL,			/* ^C is a prefix for llmchat_ccmap */
};

static struct KEYMAPE (1) llmchatmap = {
	1, 1, rescan,
	{
		{ CCHR('C'), CCHR('C'), llmchat_top,
		    (KEYMAP *)&llmchat_ccmap }
	}
};

void
llm_init(void)
{
	funmap_add(llm_compose_send, "llm-compose-send", 0);
	funmap_add(llm_compose_abort, "llm-compose-abort", 0);
	funmap_add(llm_chat_cmd, "llm-chat", 0);
	funmap_add(llm_chat_send, "llm-chat-send", 0);
	funmap_add(llm_chat_clear_input, "llm-chat-clear-input", 0);
	funmap_add(llm_chat_quit, "llm-chat-quit", 0);
	maps_add((KEYMAP *)&llmcomposemap, "llm-compose");
	maps_add((KEYMAP *)&llmchatmap, "llm-chat");
}
