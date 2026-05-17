/*
 * shell.c - microshell REPL driver
 *
 * Responsibilities:
 *   - Detect interactive vs non-interactive use
 *   - Take control of the terminal pgroup (interactive mode)
 *   - Install signal handlers (SIGINT cancels prompt, SIGCHLD reaps,
 *     SIGTSTP/SIGTTIN/SIGTTOU ignored in the shell process)
 *   - Read a line, parse, execute, repeat
 *
 * We deliberately don't link against readline so the build stays
 * self-contained on stock macOS/Linux. The line editor is just
 * fgets() with cooked-mode terminal handling - good enough for a
 * teaching shell, and explicit about what's happening.
 */
#include "parser.h"
#include "executor.h"
#include "jobs.h"
#include "history.h"
#include "builtins.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <ctype.h>
#include <termios.h>
#include <sys/types.h>
#include <sys/wait.h>

static volatile sig_atomic_t g_sigchld_pending = 0;
static volatile sig_atomic_t g_sigint_pending  = 0;
static int g_last_status   = 0;
static int g_interactive   = 0;
static pid_t g_shell_pgid  = 0;

/* SIGCHLD is async-signal restricted - we just flag and let the main
 * loop call ms_jobs_reap() at a safe point. */
static void on_sigchld(int sig) { (void)sig; g_sigchld_pending = 1; }

/* SIGINT clears the input line (in interactive mode the prompt will
 * be redrawn). The default action of killing the shell is wrong here. */
static void on_sigint(int sig) { (void)sig; g_sigint_pending = 1; }

static void install_signal_handlers(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigchld;
    sa.sa_flags   = SA_RESTART | SA_NOCLDSTOP;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGCHLD, &sa, NULL);

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigint;
    /* Do NOT use SA_RESTART for SIGINT: we want read() to wake up. */
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);

    /* In the shell process, swallow these - children re-enable them. */
    signal(SIGTSTP, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
}

/* Become the foreground process group on the controlling terminal.
 * Bash does this same dance: loop until we own the tty, then claim a
 * pgrp of our own pid. */
static void grab_terminal(void) {
    int tty = STDIN_FILENO;
    if (!isatty(tty)) return;

    /* Wait until we're in the foreground. */
    while (tcgetpgrp(tty) != getpgrp()) {
        kill(0, SIGTTIN);
    }
    g_shell_pgid = getpid();
    if (setpgid(g_shell_pgid, g_shell_pgid) < 0 && errno != EPERM) {
        /* Already a group leader? Fine. */
    }
    tcsetpgrp(tty, g_shell_pgid);
}

/* Compose the prompt. Shows last exit status when nonzero so people
 * see at a glance that something failed. */
static void render_prompt(char *buf, size_t cap) {
    char cwd[4096];
    if (!getcwd(cwd, sizeof cwd)) snprintf(cwd, sizeof cwd, "?");

    /* Tilde-shorten $HOME. */
    const char *home = getenv("HOME");
    if (home && *home) {
        size_t hl = strlen(home);
        if (strncmp(cwd, home, hl) == 0 &&
            (cwd[hl] == '/' || cwd[hl] == '\0')) {
            char tmp[4096];
            snprintf(tmp, sizeof tmp, "~%s", cwd + hl);
            strncpy(cwd, tmp, sizeof cwd - 1);
            cwd[sizeof cwd - 1] = '\0';
        }
    }
    if (g_last_status)
        snprintf(buf, cap, "microshell %s [%d]$ ", cwd, g_last_status);
    else
        snprintf(buf, cap, "microshell %s$ ", cwd);
}

/* Read one logical line. Returns NULL on EOF. The caller must free. */
static char *read_line(FILE *in) {
    size_t cap = 256, len = 0;
    char *buf = malloc(cap);
    if (!buf) return NULL;

    int c;
    while ((c = fgetc(in)) != EOF) {
        if (g_sigint_pending) {
            /* SIGINT during read: drop what we have and start over. */
            g_sigint_pending = 0;
            len = 0;
            fputc('\n', stderr);
            if (g_interactive) {
                char prompt[5120];
                render_prompt(prompt, sizeof prompt);
                fputs(prompt, stderr);
            }
            continue;
        }
        if (c == '\n') break;
        if (len + 1 >= cap) {
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) { free(buf); return NULL; }
            buf = nb;
        }
        buf[len++] = (char)c;
    }
    if (c == EOF && len == 0) { free(buf); return NULL; }
    buf[len] = '\0';
    return buf;
}

static void post_tick(void) {
    if (g_sigchld_pending) {
        g_sigchld_pending = 0;
        ms_jobs_reap();
    }
    if (g_interactive) ms_jobs_print_pending();
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    g_interactive = isatty(STDIN_FILENO) && isatty(STDERR_FILENO);
    if (g_interactive) grab_terminal();
    else               g_shell_pgid = getpgrp();

    install_signal_handlers();
    ms_jobs_init();
    ms_history_init();
    ms_executor_init(g_shell_pgid,
                     g_interactive ? STDIN_FILENO : -1,
                     g_interactive);

    if (g_interactive) {
        fputs("microshell - type 'help' for builtins, 'exit' to quit.\n", stderr);
    }

    for (;;) {
        post_tick();

        if (g_interactive) {
            char prompt[5120];
            render_prompt(prompt, sizeof prompt);
            fputs(prompt, stderr);
            fflush(stderr);
        }

        char *line = read_line(stdin);
        if (!line) break;                 /* EOF */

        /* Skip whitespace-only lines without recording them. */
        int has_content = 0;
        for (char *p = line; *p; p++) if (!isspace((unsigned char)*p)) { has_content = 1; break; }
        if (!has_content) { free(line); continue; }

        ms_history_add(line);

        ms_pipeline *p = ms_parse(line, g_last_status);
        if (!p) {
            fprintf(stderr, "microshell: parse error: %s\n", ms_parser_last_error());
            g_last_status = 2;
            free(line);
            continue;
        }
        if (!p->head) {                   /* comment-only / empty */
            ms_pipeline_free(p);
            free(line);
            continue;
        }

        int should_exit = 0;
        g_last_status = ms_execute(p, &should_exit);
        ms_pipeline_free(p);
        free(line);
        post_tick();

        if (should_exit) break;
    }

    if (g_interactive) fputc('\n', stderr);
    ms_history_shutdown();
    ms_jobs_shutdown();
    return g_last_status;
}
