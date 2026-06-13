/*
 * builtins.c - built-in commands
 *
 * Builtins that change shell state (cd, exit, export) must run in
 * the shell process; otherwise the changes would be discarded along
 * with the child. The executor checks ms_is_builtin() *before* fork
 * for single-stage pipelines for that reason.
 *
 * Inside a multi-stage pipeline we still call builtins, but in the
 * child - cd inside a pipeline has no observable effect, which
 * matches bash behaviour.
 */
#include "builtins.h"
#include "jobs.h"
#include "history.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>

extern char **environ;

/* ---------- registry ---------- */

typedef int (*builtin_fn)(ms_cmd *, int *);

typedef struct { const char *name; builtin_fn fn; } builtin_entry;

static int bi_cd(ms_cmd *, int *);
static int bi_pwd(ms_cmd *, int *);
static int bi_exit(ms_cmd *, int *);
static int bi_export(ms_cmd *, int *);
static int bi_env(ms_cmd *, int *);
static int bi_help(ms_cmd *, int *);
static int bi_history(ms_cmd *, int *);
static int bi_jobs(ms_cmd *, int *);
static int bi_fg(ms_cmd *, int *);
static int bi_bg(ms_cmd *, int *);
static int bi_kill(ms_cmd *, int *);
static int bi_unset(ms_cmd *, int *);

static const builtin_entry kBuiltins[] = {
    { "cd",      bi_cd      },
    { "pwd",     bi_pwd     },
    { "exit",    bi_exit    },
    { "quit",    bi_exit    },
    { "export",  bi_export  },
    { "unset",   bi_unset   },
    { "env",     bi_env     },
    { "help",    bi_help    },
    { "history", bi_history },
    { "jobs",    bi_jobs    },
    { "fg",      bi_fg      },
    { "bg",      bi_bg      },
    { "kill",    bi_kill    },
    { NULL,      NULL       },
};

int ms_is_builtin(const char *name) {
    if (!name) return 0;
    for (const builtin_entry *e = kBuiltins; e->name; e++)
        if (strcmp(e->name, name) == 0) return 1;
    return 0;
}

int ms_run_builtin(ms_cmd *cmd, int *out_should_exit) {
    *out_should_exit = 0;
    for (const builtin_entry *e = kBuiltins; e->name; e++)
        if (strcmp(e->name, cmd->argv[0]) == 0)
            return e->fn(cmd, out_should_exit);
    fprintf(stderr, "microshell: %s: not a builtin\n", cmd->argv[0]);
    return 127;
}

/* ---------- implementations ---------- */

static int bi_cd(ms_cmd *cmd, int *exitf) {
    (void)exitf;
    const char *target = cmd->argv[1];
    if (!target) target = getenv("HOME");
    if (!target) { fprintf(stderr, "cd: HOME not set\n"); return 1; }
    if (chdir(target) < 0) {
        fprintf(stderr, "cd: %s: %s\n", target, strerror(errno));
        return 1;
    }
    /* keep PWD in sync so subprocesses see the right value */
    char buf[4096];
    if (getcwd(buf, sizeof buf)) setenv("PWD", buf, 1);
    return 0;
}

static int bi_pwd(ms_cmd *cmd, int *exitf) {
    (void)cmd; (void)exitf;
    char buf[4096];
    if (!getcwd(buf, sizeof buf)) {
        fprintf(stderr, "pwd: %s\n", strerror(errno));
        return 1;
    }
    puts(buf);
    return 0;
}

static int bi_exit(ms_cmd *cmd, int *exitf) {
    *exitf = 1;
    if (cmd->argv[1]) return atoi(cmd->argv[1]) & 0xff;
    return 0;
}

static int bi_export(ms_cmd *cmd, int *exitf) {
    (void)exitf;
    if (cmd->argc == 1) {                  /* dump like `env` would */
        for (char **e = environ; *e; e++) printf("declare -x %s\n", *e);
        return 0;
    }
    int rc = 0;
    for (size_t i = 1; i < cmd->argc; i++) {
        char *eq = strchr(cmd->argv[i], '=');
        if (eq) {
            *eq = '\0';
            if (setenv(cmd->argv[i], eq + 1, 1) < 0) {
                fprintf(stderr, "export: %s\n", strerror(errno));
                rc = 1;
            }
            *eq = '=';
        } else {
            /* `export NAME` with no '=' just promotes an existing var */
            const char *v = getenv(cmd->argv[i]);
            if (v) setenv(cmd->argv[i], v, 1);
        }
    }
    return rc;
}

static int bi_unset(ms_cmd *cmd, int *exitf) {
    (void)exitf;
    for (size_t i = 1; i < cmd->argc; i++) unsetenv(cmd->argv[i]);
    return 0;
}

static int bi_env(ms_cmd *cmd, int *exitf) {
    (void)cmd; (void)exitf;
    for (char **e = environ; *e; e++) puts(*e);
    return 0;
}

static int bi_help(ms_cmd *cmd, int *exitf) {
    (void)cmd; (void)exitf;
    puts("microshell builtins:");
    puts("  cd [dir]          change directory");
    puts("  pwd               print working directory");
    puts("  export NAME[=val] export environment variable");
    puts("  unset NAME...     remove environment variable");
    puts("  env               list environment");
    puts("  history           show command history");
    puts("  jobs              list background jobs");
    puts("  fg [%n]           resume job in foreground");
    puts("  bg [%n]           resume job in background");
    puts("  kill [-SIG] %n|pid");
    puts("  help              this text");
    puts("  exit [n]          quit the shell");
    return 0;
}

static void hist_print(int idx, const char *line, void *ud) {
    (void)ud; printf("%5d  %s\n", idx, line);
}
static int bi_history(ms_cmd *cmd, int *exitf) {
    (void)cmd; (void)exitf;
    ms_history_each(hist_print, NULL);
    return 0;
}

static void jobs_print(const ms_job *j, void *ud) {
    (void)ud;
    if (j->state == MS_JOB_DONE) return;
    printf("[%d]  %s\t\t%s\n",
           j->id, ms_job_state_label(j->state),
           j->cmdline ? j->cmdline : "");
}
static int bi_jobs(ms_cmd *cmd, int *exitf) {
    (void)cmd; (void)exitf;
    ms_jobs_each(jobs_print, NULL);
    return 0;
}

/* Resolve "%n" or "<n>" -> job id; default to current job. */
static ms_job *pick_job(ms_cmd *cmd) {
    if (cmd->argc < 2) return ms_jobs_current();
    const char *s = cmd->argv[1];
    if (*s == '%') s++;
    int id = atoi(s);
    if (id <= 0) return NULL;
    return ms_jobs_find(id);
}

static int bi_fg(ms_cmd *cmd, int *exitf) {
    (void)exitf;
    ms_job *j = pick_job(cmd);
    if (!j) { fprintf(stderr, "fg: no such job\n"); return 1; }

    /* Hand the terminal to the job's pgid, continue it, wait. */
    if (tcsetpgrp(STDIN_FILENO, j->pgid) < 0) {
        /* not a TTY (e.g. tests) - keep going anyway */
    }
    if (kill(-j->pgid, SIGCONT) < 0)
        fprintf(stderr, "fg: %s\n", strerror(errno));
    j->state = MS_JOB_RUNNING;

    int status = 0;
    for (;;) {
        pid_t r = waitpid(-j->pgid, &status, WUNTRACED);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) break;
        if (WIFSTOPPED(status)) { j->state = MS_JOB_STOPPED; break; }
        if (WIFEXITED(status) || WIFSIGNALED(status)) {
            j->state = MS_JOB_DONE;
            break;
        }
    }

    /* Reclaim terminal control. */
    pid_t shell_pgid = getpgrp();
    tcsetpgrp(STDIN_FILENO, shell_pgid);

    if (WIFEXITED(status))   return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 0;
}

static int bi_bg(ms_cmd *cmd, int *exitf) {
    (void)exitf;
    ms_job *j = pick_job(cmd);
    if (!j) { fprintf(stderr, "bg: no such job\n"); return 1; }
    if (kill(-j->pgid, SIGCONT) < 0) {
        fprintf(stderr, "bg: %s\n", strerror(errno));
        return 1;
    }
    j->state    = MS_JOB_RUNNING;
    j->notified = 0;
    fprintf(stderr, "[%d]+ %s &\n", j->id, j->cmdline ? j->cmdline : "");
    return 0;
}

/* Parse a signal spec after '-': a number ("9") or a name ("KILL"/"SIGKILL").
 * Returns the signal number, or -1 if unrecognized. */
static int parse_signal(const char *spec) {
    if (!spec || !*spec) return -1;
    /* All-digits -> numeric signal. */
    int all_digits = 1;
    for (const char *p = spec; *p; p++) {
        if (*p < '0' || *p > '9') { all_digits = 0; break; }
    }
    if (all_digits) {
        int n = atoi(spec);
        return (n > 0 && n < NSIG) ? n : -1;
    }
    /* Accept an optional "SIG" prefix, case-insensitive on the name. */
    const char *name = spec;
    if (strncasecmp(name, "SIG", 3) == 0) name += 3;
    static const struct { const char *n; int s; } sigs[] = {
        {"HUP", SIGHUP}, {"INT", SIGINT}, {"QUIT", SIGQUIT}, {"KILL", SIGKILL},
        {"TERM", SIGTERM}, {"STOP", SIGSTOP}, {"CONT", SIGCONT}, {"TSTP", SIGTSTP},
        {"USR1", SIGUSR1}, {"USR2", SIGUSR2}, {"ABRT", SIGABRT}, {"ALRM", SIGALRM},
        {"SEGV", SIGSEGV}, {"PIPE", SIGPIPE},
    };
    for (size_t i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++)
        if (strcasecmp(name, sigs[i].n) == 0) return sigs[i].s;
    return -1;
}

static int bi_kill(ms_cmd *cmd, int *exitf) {
    (void)exitf;
    if (cmd->argc < 2) { fprintf(stderr, "usage: kill [-SIG] pid|%%n\n"); return 1; }

    int sig = SIGTERM;
    size_t arg_i = 1;
    if (cmd->argv[1][0] == '-' && cmd->argv[1][1]) {
        sig    = parse_signal(cmd->argv[1] + 1);
        arg_i  = 2;
        if (sig <= 0) { fprintf(stderr, "kill: %s: invalid signal\n", cmd->argv[1] + 1); return 1; }
    }
    int rc = 0;
    for (; arg_i < cmd->argc; arg_i++) {
        const char *t = cmd->argv[arg_i];
        pid_t target;
        if (*t == '%') {
            ms_job *j = ms_jobs_find(atoi(t + 1));
            if (!j) { fprintf(stderr, "kill: no such job: %s\n", t); rc = 1; continue; }
            target = -j->pgid;
        } else {
            target = (pid_t)atoi(t);
        }
        if (kill(target, sig) < 0) {
            fprintf(stderr, "kill: %s: %s\n", t, strerror(errno));
            rc = 1;
        }
    }
    return rc;
}
