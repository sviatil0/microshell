/*
 * parser.c - line tokenizer + pipeline parser
 *
 * The tokenizer walks the input one character at a time, accumulating
 * runs into a working buffer. Quotes flip mode flags; backslash escapes
 * a single following byte; $NAME/${NAME}/$? expand from the environment
 * (or the supplied last-status). The parser then arranges the token
 * stream into ms_cmd nodes split on '|'.
 *
 * No regex, no third-party deps - this is plain libc.
 */
#include "parser.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char g_err[256] = "";

static void set_err(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_err, sizeof g_err, fmt, ap);
    va_end(ap);
}

const char *ms_parser_last_error(void) { return g_err; }

/* ---------- dynamic string buffer ---------- */

typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} sbuf;

static int sbuf_push(sbuf *s, char c) {
    if (s->len + 1 >= s->cap) {
        size_t ncap = s->cap ? s->cap * 2 : 32;
        char *nd = realloc(s->data, ncap);
        if (!nd) return -1;
        s->data = nd;
        s->cap  = ncap;
    }
    s->data[s->len++] = c;
    s->data[s->len]   = '\0';
    return 0;
}

static void sbuf_reset(sbuf *s) { if (s->data) s->data[0] = '\0'; s->len = 0; }

static char *sbuf_steal(sbuf *s) {
    char *out = s->data ? s->data : strdup("");
    s->data = NULL; s->len = 0; s->cap = 0;
    return out;
}

/* ---------- token list ---------- */

typedef enum {
    TK_WORD,
    TK_PIPE,
    TK_LT,
    TK_GT,
    TK_GTGT,
    TK_2GT,
    TK_AMP
} tk_kind;

typedef struct {
    tk_kind kind;
    char *text;     /* owned, only for TK_WORD */
} tok;

typedef struct {
    tok    *items;
    size_t  len;
    size_t  cap;
} toklist;

static int tl_push(toklist *t, tk_kind k, char *text) {
    if (t->len == t->cap) {
        size_t ncap = t->cap ? t->cap * 2 : 16;
        tok *ni = realloc(t->items, ncap * sizeof(tok));
        if (!ni) return -1;
        t->items = ni;
        t->cap   = ncap;
    }
    t->items[t->len].kind = k;
    t->items[t->len].text = text;
    t->len++;
    return 0;
}

static void tl_free(toklist *t) {
    for (size_t i = 0; i < t->len; i++) free(t->items[i].text);
    free(t->items);
}

/* ---------- variable expansion ---------- */

/* Append the value of $NAME (or $? -> last_status) to buf. Advances *i
 * past the variable reference. Caller has already consumed the '$'. */
static int expand_var(const char *src, size_t *i, sbuf *buf, int last_status) {
    char name[128];
    size_t n = 0;
    char c = src[*i];

    if (c == '?') {
        (*i)++;
        char tmp[16];
        int len = snprintf(tmp, sizeof tmp, "%d", last_status);
        for (int k = 0; k < len; k++) if (sbuf_push(buf, tmp[k]) < 0) return -1;
        return 0;
    }

    int braced = 0;
    if (c == '{') { braced = 1; (*i)++; }

    while (src[*i] && n + 1 < sizeof name &&
           (isalnum((unsigned char)src[*i]) || src[*i] == '_')) {
        name[n++] = src[(*i)++];
    }
    name[n] = '\0';

    if (braced) {
        if (src[*i] != '}') { set_err("unterminated ${...}"); return -1; }
        (*i)++;
    }

    if (n == 0) {                       /* lone '$' - keep literal */
        if (sbuf_push(buf, '$') < 0) return -1;
        return 0;
    }
    const char *val = getenv(name);
    if (!val) return 0;
    for (; *val; val++) if (sbuf_push(buf, *val) < 0) return -1;
    return 0;
}

/* ---------- tokenizer ---------- */

static int tokenize(const char *src, toklist *out, int last_status) {
    sbuf word = {0};
    int  in_word = 0;
    size_t i = 0;

    while (src[i]) {
        char c = src[i];

        if (c == '#' && !in_word) break;     /* comment to EOL */

        if (isspace((unsigned char)c)) {
            if (in_word) {
                if (tl_push(out, TK_WORD, sbuf_steal(&word)) < 0) goto oom;
                in_word = 0;
                sbuf_reset(&word);
            }
            i++;
            continue;
        }

        /* operators (only outside quotes - handled in quoted blocks below) */
        if (c == '|' || c == '<' || c == '>' || c == '&') {
            if (in_word) {
                if (tl_push(out, TK_WORD, sbuf_steal(&word)) < 0) goto oom;
                in_word = 0;
                sbuf_reset(&word);
            }
            if (c == '|')      { tl_push(out, TK_PIPE, NULL); i++; }
            else if (c == '&') { tl_push(out, TK_AMP,  NULL); i++; }
            else if (c == '<') { tl_push(out, TK_LT,   NULL); i++; }
            else /* '>' */ {
                if (src[i + 1] == '>') { tl_push(out, TK_GTGT, NULL); i += 2; }
                else                   { tl_push(out, TK_GT,   NULL); i++;   }
            }
            continue;
        }

        /* 2>file - only recognize when '2' is the start of a fresh word */
        if (c == '2' && src[i + 1] == '>' && !in_word) {
            tl_push(out, TK_2GT, NULL);
            i += 2;
            continue;
        }

        /* single-quoted: literal everything up to next ' */
        if (c == '\'') {
            in_word = 1;
            i++;
            while (src[i] && src[i] != '\'') {
                if (sbuf_push(&word, src[i++]) < 0) goto oom;
            }
            if (src[i] != '\'') { set_err("unterminated single quote"); goto fail; }
            i++;
            continue;
        }

        /* double-quoted: backslash + $expansion still apply */
        if (c == '"') {
            in_word = 1;
            i++;
            while (src[i] && src[i] != '"') {
                if (src[i] == '\\' && src[i + 1]) {
                    if (sbuf_push(&word, src[i + 1]) < 0) goto oom;
                    i += 2;
                } else if (src[i] == '$') {
                    i++;
                    if (expand_var(src, &i, &word, last_status) < 0) goto fail;
                } else {
                    if (sbuf_push(&word, src[i++]) < 0) goto oom;
                }
            }
            if (src[i] != '"') { set_err("unterminated double quote"); goto fail; }
            i++;
            continue;
        }

        /* backslash outside quotes */
        if (c == '\\' && src[i + 1]) {
            in_word = 1;
            if (sbuf_push(&word, src[i + 1]) < 0) goto oom;
            i += 2;
            continue;
        }

        /* unquoted $expansion */
        if (c == '$') {
            in_word = 1;
            i++;
            if (expand_var(src, &i, &word, last_status) < 0) goto fail;
            continue;
        }

        /* ordinary character */
        in_word = 1;
        if (sbuf_push(&word, c) < 0) goto oom;
        i++;
    }

    if (in_word) {
        if (tl_push(out, TK_WORD, sbuf_steal(&word)) < 0) goto oom;
    }
    free(word.data);
    return 0;

oom:
    set_err("out of memory");
fail:
    free(word.data);
    return -1;
}

/* ---------- ms_cmd helpers ---------- */

static ms_cmd *cmd_new(void) {
    ms_cmd *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->argv = calloc(1, sizeof(char *));   /* room for NULL terminator */
    return c;
}

static int cmd_push_arg(ms_cmd *c, const char *arg) {
    char **na = realloc(c->argv, (c->argc + 2) * sizeof(char *));
    if (!na) return -1;
    c->argv = na;
    c->argv[c->argc] = strdup(arg);
    if (!c->argv[c->argc]) return -1;
    c->argc++;
    c->argv[c->argc] = NULL;
    return 0;
}

static void cmd_free(ms_cmd *c) {
    if (!c) return;
    if (c->argv) {
        for (size_t i = 0; i < c->argc; i++) free(c->argv[i]);
        free(c->argv);
    }
    free(c->in_file);
    free(c->out_file);
    free(c->err_file);
    free(c);
}

void ms_pipeline_free(ms_pipeline *p) {
    if (!p) return;
    ms_cmd *c = p->head;
    while (c) {
        ms_cmd *n = c->next;
        cmd_free(c);
        c = n;
    }
    free(p->raw);
    free(p);
}

/* ---------- pipeline assembly ---------- */

ms_pipeline *ms_parse(const char *line, int last_status) {
    g_err[0] = '\0';

    ms_pipeline *p = calloc(1, sizeof *p);
    if (!p) { set_err("out of memory"); return NULL; }
    p->raw = strdup(line ? line : "");

    toklist tl = {0};
    if (tokenize(line ? line : "", &tl, last_status) < 0) {
        tl_free(&tl);
        ms_pipeline_free(p);
        return NULL;
    }

    if (tl.len == 0) {                     /* empty / comment-only */
        tl_free(&tl);
        return p;
    }

    ms_cmd *cur = cmd_new();
    if (!cur) { set_err("out of memory"); tl_free(&tl); ms_pipeline_free(p); return NULL; }
    p->head = cur;

    for (size_t i = 0; i < tl.len; i++) {
        tok *t = &tl.items[i];

        switch (t->kind) {
        case TK_WORD:
            if (cmd_push_arg(cur, t->text) < 0) { set_err("out of memory"); goto fail; }
            break;

        case TK_PIPE: {
            if (cur->argc == 0) { set_err("empty command before '|'"); goto fail; }
            ms_cmd *nx = cmd_new();
            if (!nx) { set_err("out of memory"); goto fail; }
            cur->next = nx;
            cur = nx;
            break;
        }

        case TK_AMP:
            if (i != tl.len - 1) { set_err("'&' must terminate the line"); goto fail; }
            p->background = 1;
            break;

        case TK_LT:
        case TK_GT:
        case TK_GTGT:
        case TK_2GT: {
            if (i + 1 >= tl.len || tl.items[i + 1].kind != TK_WORD) {
                set_err("redirection needs a filename"); goto fail;
            }
            const char *fn = tl.items[++i].text;
            char *dup = strdup(fn);
            if (!dup) { set_err("out of memory"); goto fail; }
            if (t->kind == TK_LT)   { free(cur->in_file);  cur->in_file  = dup; }
            else if (t->kind == TK_2GT) { free(cur->err_file); cur->err_file = dup; }
            else { /* > or >> */
                free(cur->out_file);
                cur->out_file   = dup;
                cur->out_append = (t->kind == TK_GTGT);
            }
            break;
        }
        }
    }

    if (p->head && p->head->argc == 0 && !p->head->next) {
        /* Only an '&' or stray redir? Treat as empty input - drop pipeline. */
        if (!p->background) {
            set_err("empty command");
            goto fail;
        }
        ms_cmd *only = p->head;
        if (!only->in_file && !only->out_file && !only->err_file) {
            cmd_free(p->head);
            p->head = NULL;
        }
    }

    tl_free(&tl);
    return p;

fail:
    tl_free(&tl);
    ms_pipeline_free(p);
    return NULL;
}
