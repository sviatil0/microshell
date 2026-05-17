/*
 * parser.h - tokenizer + pipeline parser for microshell
 *
 * Grammar (informal):
 *   line     := pipeline ('&')?  ('#' ...)?
 *   pipeline := command ( '|' command )*
 *   command  := word ( word | redir )*
 *   redir    := ('<' | '>' | '>>' | '2>') word
 *
 * A pipeline is parsed into a linked list of `ms_cmd` nodes; each node
 * carries its argv plus per-stage redirection targets. The caller owns
 * the pipeline and must release it with ms_pipeline_free().
 */
#ifndef MICROSHELL_PARSER_H
#define MICROSHELL_PARSER_H

#include <stddef.h>

/* Redirection kinds. MS_REDIR_NONE means "no override for this fd". */
typedef enum {
    MS_REDIR_NONE = 0,
    MS_REDIR_IN,        /* <  file        */
    MS_REDIR_OUT,       /* >  file (trunc) */
    MS_REDIR_APPEND,    /* >> file        */
    MS_REDIR_ERR        /* 2> file (trunc) */
} ms_redir_kind;

typedef struct ms_cmd {
    char **argv;            /* NULL-terminated argv */
    size_t argc;
    char *in_file;          /* stdin redirect target (or NULL) */
    char *out_file;         /* stdout redirect target (or NULL) */
    int   out_append;       /* nonzero if out_file should be appended */
    char *err_file;         /* stderr redirect target (or NULL) */
    struct ms_cmd *next;    /* next stage of the pipeline */
} ms_cmd;

typedef struct {
    ms_cmd *head;           /* first command in the pipeline */
    int background;         /* trailing '&' present */
    char *raw;              /* original (post-comment-strip) source */
} ms_pipeline;

/*
 * Parse a full input line into a pipeline.
 * `last_status` is substituted for `$?` during expansion.
 * Returns NULL on parse error; check ms_parser_last_error() for a
 * human-readable diagnostic. Returns a pipeline with head==NULL for
 * empty or comment-only input - callers should treat that as a no-op.
 */
ms_pipeline *ms_parse(const char *line, int last_status);

/* Release a pipeline and everything it owns. NULL-safe. */
void ms_pipeline_free(ms_pipeline *p);

/* Last parser error message ("" if none). Thread-unsafe by design - this
 * shell is single-threaded and the message is overwritten on each call. */
const char *ms_parser_last_error(void);

#endif /* MICROSHELL_PARSER_H */
