/*
 * executor.c - fork/exec orchestrator
 *
 * For each pipeline stage we open the needed redirections, then dup2()
 * them onto stdin/stdout/stderr in the child. Pipe fds between stages
 * are passed forward through a small two-int rolling buffer; both
 * parent and child must close the right ends to avoid hangs.
 *
 * Process-group rules (mirrors what bash does):
 *   - The first child sets its pgid to its own pid.
 *   - Subsequent children join that pgid via setpgid(0, pgid).
 *   - The parent also calls setpgid() on each child to avoid races.
 *   - For foreground pipelines, we tcsetpgrp() to the new pgid so
 *     ctrl-c reaches the right group, then take the terminal back.
 */
#include "executor.h"
#include "builtins.h"
#include "jobs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/types.h>

static pid_t g_shell_pgid = 0;
static int   g_shell_tty  = -1;
static int   g_interactive = 0;

void ms_executor_init(pid_t shell_pgid, int shell_tty_fd, int interactive) {
    g_shell_pgid  = shell_pgid;
    g_shell_tty   = shell_tty_fd;
    g_interactive = interactive;
}

/* Open a redirection target and dup it onto target_fd. Bails the
 * caller (returns -1) on failure - the child will exit with 1. */
static int apply_redir(const char *path, int flags, int target_fd) {
    int fd = open(path, flags, 0644);
    if (fd < 0) {
        fprintf(stderr, "microshell: %s: %s\n", path, strerror(errno));
        return -1;
    }
    if (dup2(fd, target_fd) < 0) {
        fprintf(stderr, "microshell: dup2: %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

/* In the child: install per-stage redirections + restore signal
 * defaults, then exec into the requested program. */
static void child_setup_and_run(ms_cmd *c, int in_fd, int out_fd, pid_t pgid) {
    /* Move ourselves into the pipeline pgid. Ignore races. */
    if (setpgid(0, pgid ? pgid : 0) < 0) { /* not fatal */ }

    /* Reset signals to defaults - the shell ignored some of them. */
    signal(SIGINT,  SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    signal(SIGTSTP, SIG_DFL);
    signal(SIGTTIN, SIG_DFL);
    signal(SIGTTOU, SIG_DFL);
    signal(SIGCHLD, SIG_DFL);

    if (in_fd  != STDIN_FILENO)  { dup2(in_fd,  STDIN_FILENO);  close(in_fd);  }
    if (out_fd != STDOUT_FILENO) { dup2(out_fd, STDOUT_FILENO); close(out_fd); }

    if (c->in_file  && apply_redir(c->in_file,  O_RDONLY, STDIN_FILENO)  < 0) _exit(1);
    if (c->out_file) {
        int flags = O_WRONLY | O_CREAT | (c->out_append ? O_APPEND : O_TRUNC);
        if (apply_redir(c->out_file, flags, STDOUT_FILENO) < 0) _exit(1);
    }
    if (c->err_file) {
        int flags = O_WRONLY | O_CREAT | O_TRUNC;
        if (apply_redir(c->err_file, flags, STDERR_FILENO) < 0) _exit(1);
    }

    if (ms_is_builtin(c->argv[0])) {
        int dummy = 0;
        int rc = ms_run_builtin(c, &dummy);
        fflush(NULL);
        _exit(rc & 0xff);
    }

    execvp(c->argv[0], c->argv);
    fprintf(stderr, "microshell: %s: %s\n", c->argv[0], strerror(errno));
    _exit(errno == ENOENT ? 127 : 126);
}

/* Reconstruct a printable command line from the pipeline (best-effort,
 * used for job labels). */
static char *render_cmdline(ms_pipeline *p) {
    if (p->raw && *p->raw) return strdup(p->raw);
    size_t cap = 64, len = 0;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    buf[0] = '\0';
    for (ms_cmd *c = p->head; c; c = c->next) {
        for (size_t i = 0; i < c->argc; i++) {
            size_t need = strlen(c->argv[i]) + 2;
            if (len + need >= cap) {
                while (len + need >= cap) cap *= 2;
                char *grown = realloc(buf, cap);
                if (!grown) { free(buf); return NULL; }  /* don't leak the old block */
                buf = grown;
            }
            if (len) strcat(buf, " ");
            strcat(buf, c->argv[i]);
            len = strlen(buf);
        }
        if (c->next) {
            size_t need = 4; /* " | " + NUL */
            if (len + need >= cap) {
                while (len + need >= cap) cap *= 2;
                char *grown = realloc(buf, cap);
                if (!grown) { free(buf); return NULL; }
                buf = grown;
            }
            strcat(buf, " | ");
            len += 3;
        }
    }
    return buf;
}

int ms_execute(ms_pipeline *p, int *out_should_exit) {
    *out_should_exit = 0;
    if (!p || !p->head) return 0;

    /* Single-stage builtin runs in-process so cd/exit/export work. */
    if (!p->head->next && p->head->argc > 0 &&
        ms_is_builtin(p->head->argv[0]) && !p->background) {

        /* Save fds for redirections in-process. */
        int saved_in = -1, saved_out = -1, saved_err = -1;
        int rc = 0;
        ms_cmd *c = p->head;

        if (c->in_file) {
            saved_in = dup(STDIN_FILENO);
            if (apply_redir(c->in_file, O_RDONLY, STDIN_FILENO) < 0) rc = 1;
        }
        if (rc == 0 && c->out_file) {
            saved_out = dup(STDOUT_FILENO);
            int flags = O_WRONLY | O_CREAT | (c->out_append ? O_APPEND : O_TRUNC);
            if (apply_redir(c->out_file, flags, STDOUT_FILENO) < 0) rc = 1;
        }
        if (rc == 0 && c->err_file) {
            saved_err = dup(STDERR_FILENO);
            if (apply_redir(c->err_file, O_WRONLY | O_CREAT | O_TRUNC, STDERR_FILENO) < 0) rc = 1;
        }
        if (rc == 0) rc = ms_run_builtin(c, out_should_exit);
        fflush(stdout); fflush(stderr);

        if (saved_in  >= 0) { dup2(saved_in,  STDIN_FILENO);  close(saved_in);  }
        if (saved_out >= 0) { dup2(saved_out, STDOUT_FILENO); close(saved_out); }
        if (saved_err >= 0) { dup2(saved_err, STDERR_FILENO); close(saved_err); }
        return rc;
    }

    /* Count stages. */
    size_t nstages = 0;
    for (ms_cmd *c = p->head; c; c = c->next) nstages++;

    pid_t *pids = calloc(nstages, sizeof(pid_t));
    if (!pids) { fprintf(stderr, "microshell: out of memory\n"); return 1; }

    pid_t pgid = 0;
    int   in_fd = STDIN_FILENO;
    int   pipefd[2];
    size_t idx = 0;

    for (ms_cmd *c = p->head; c; c = c->next, idx++) {
        int out_fd = STDOUT_FILENO;

        if (c->next) {
            if (pipe(pipefd) < 0) {
                fprintf(stderr, "microshell: pipe: %s\n", strerror(errno));
                free(pids);
                return 1;
            }
            out_fd = pipefd[1];
        }

        if (c->argc == 0) {
            fprintf(stderr, "microshell: empty pipeline stage\n");
            if (c->next) { close(pipefd[0]); close(pipefd[1]); }
            free(pids);
            return 2;
        }

        pid_t pid = fork();
        if (pid < 0) {
            fprintf(stderr, "microshell: fork: %s\n", strerror(errno));
            free(pids);
            return 1;
        }

        if (pid == 0) {
            /* close the read end we won't use */
            if (c->next) close(pipefd[0]);
            child_setup_and_run(c, in_fd, out_fd, pgid);
        }

        /* parent */
        if (pgid == 0) pgid = pid;
        setpgid(pid, pgid);                 /* race-safe duplicate of child call */
        pids[idx] = pid;

        if (in_fd != STDIN_FILENO) close(in_fd);
        if (c->next) close(pipefd[1]);
        in_fd = c->next ? pipefd[0] : STDIN_FILENO;
    }

    if (p->background) {
        char *label = render_cmdline(p);
        int id = ms_jobs_add(pgid, label ? label : "", MS_JOB_RUNNING);
        free(label);
        fprintf(stderr, "[%d] %d\n", id, (int)pgid);
        free(pids);
        return 0;
    }

    /* Foreground: give the terminal to the pgid (if we have one). */
    if (g_interactive && g_shell_tty >= 0) {
        tcsetpgrp(g_shell_tty, pgid);
    }

    int last_status = 0;
    size_t remaining = nstages;
    while (remaining > 0) {
        int   status;
        pid_t r = waitpid(-pgid, &status, WUNTRACED);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (WIFSTOPPED(status)) {
            /* Whole pgid got stopped (^Z). Hand it to jobs and bail. */
            char *label = render_cmdline(p);
            int id = ms_jobs_add(pgid, label ? label : "", MS_JOB_STOPPED);
            free(label);
            fprintf(stderr, "\n[%d]+ Stopped\n", id);
            break;
        }
        if (WIFEXITED(status) || WIFSIGNALED(status)) {
            if (r == pids[nstages - 1]) {
                last_status = WIFEXITED(status)
                    ? WEXITSTATUS(status)
                    : 128 + WTERMSIG(status);
            }
            remaining--;
        }
    }

    /* Reclaim the terminal. */
    if (g_interactive && g_shell_tty >= 0) {
        tcsetpgrp(g_shell_tty, g_shell_pgid);
    }
    free(pids);
    return last_status;
}
