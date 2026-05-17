/*
 * jobs.c - in-memory job table
 *
 * State lives in a fixed-size array. ms_jobs_reap() runs waitpid()
 * with WNOHANG|WUNTRACED|WCONTINUED across every tracked pgid; the
 * shell's main loop calls this on every prompt redraw, and the
 * SIGCHLD handler sets a flag the loop reads.
 *
 * We never call printf() from the signal handler - that would be
 * a textbook async-signal-safety bug. ms_jobs_print_pending() is
 * invoked from the main loop instead.
 */
#include "jobs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/wait.h>
#include <errno.h>

static ms_job g_jobs[MS_JOBS_MAX];
static int    g_next_id = 1;

void ms_jobs_init(void) {
    memset(g_jobs, 0, sizeof g_jobs);
    g_next_id = 1;
}

void ms_jobs_shutdown(void) {
    for (int i = 0; i < MS_JOBS_MAX; i++) {
        free(g_jobs[i].cmdline);
        g_jobs[i].cmdline = NULL;
        g_jobs[i].state   = MS_JOB_FREE;
    }
}

static ms_job *find_free_slot(void) {
    for (int i = 0; i < MS_JOBS_MAX; i++)
        if (g_jobs[i].state == MS_JOB_FREE) return &g_jobs[i];
    return NULL;
}

int ms_jobs_add(pid_t pgid, const char *cmdline, ms_job_state state) {
    ms_job *j = find_free_slot();
    if (!j) return -1;
    j->id       = g_next_id++;
    j->pgid     = pgid;
    j->state    = state;
    j->cmdline  = cmdline ? strdup(cmdline) : NULL;
    j->notified = 0;
    return j->id;
}

ms_job *ms_jobs_find(int id) {
    for (int i = 0; i < MS_JOBS_MAX; i++)
        if (g_jobs[i].state != MS_JOB_FREE && g_jobs[i].id == id)
            return &g_jobs[i];
    return NULL;
}

ms_job *ms_jobs_find_by_pgid(pid_t pgid) {
    for (int i = 0; i < MS_JOBS_MAX; i++)
        if (g_jobs[i].state != MS_JOB_FREE && g_jobs[i].pgid == pgid)
            return &g_jobs[i];
    return NULL;
}

ms_job *ms_jobs_current(void) {
    ms_job *best = NULL;
    for (int i = 0; i < MS_JOBS_MAX; i++) {
        ms_job *j = &g_jobs[i];
        if (j->state == MS_JOB_RUNNING || j->state == MS_JOB_STOPPED) {
            if (!best || j->id > best->id) best = j;
        }
    }
    return best;
}

void ms_jobs_reap(void) {
    /* For each known pgid, drain whatever has happened to its members.
     * We can't just call waitpid(-1, ...) because foreground waits live
     * in the executor and we don't want to swallow their status here. */
    for (int i = 0; i < MS_JOBS_MAX; i++) {
        ms_job *j = &g_jobs[i];
        if (j->state == MS_JOB_FREE || j->state == MS_JOB_DONE) continue;

        for (;;) {
            int   status;
            pid_t r = waitpid(-j->pgid, &status,
                              WNOHANG | WUNTRACED | WCONTINUED);
            if (r == 0) break;          /* still running, no change   */
            if (r < 0) {
                if (errno == EINTR) continue;
                /* ECHILD: pgid has no more children -> mark done.
                 * If we already saw an exit/signal, leave that state. */
                if (errno == ECHILD && j->state != MS_JOB_STOPPED)
                    j->state = MS_JOB_DONE;
                break;
            }
            if (WIFSTOPPED(status))      j->state = MS_JOB_STOPPED;
            else if (WIFCONTINUED(status)) j->state = MS_JOB_RUNNING;
            else if (WIFEXITED(status) || WIFSIGNALED(status))
                j->state = MS_JOB_DONE;
        }
    }
}

static const char *state_label(ms_job_state s) {
    switch (s) {
    case MS_JOB_RUNNING: return "Running";
    case MS_JOB_STOPPED: return "Stopped";
    case MS_JOB_DONE:    return "Done";
    default:             return "?";
    }
}

void ms_jobs_print_pending(void) {
    for (int i = 0; i < MS_JOBS_MAX; i++) {
        ms_job *j = &g_jobs[i];
        if (j->state == MS_JOB_DONE && !j->notified) {
            fprintf(stderr, "[%d]+  Done\t\t%s\n",
                    j->id, j->cmdline ? j->cmdline : "");
            j->notified = 1;
            free(j->cmdline);
            j->cmdline = NULL;
            j->state   = MS_JOB_FREE;
        } else if (j->state == MS_JOB_STOPPED && !j->notified) {
            fprintf(stderr, "[%d]+  Stopped\t%s\n",
                    j->id, j->cmdline ? j->cmdline : "");
            j->notified = 1;
        }
    }
}

int ms_jobs_each(void (*fn)(const ms_job *, void *), void *ud) {
    int n = 0;
    for (int i = 0; i < MS_JOBS_MAX; i++) {
        if (g_jobs[i].state != MS_JOB_FREE) {
            fn(&g_jobs[i], ud);
            n++;
        }
    }
    (void)state_label;          /* keep symbol available for inspectors */
    return n;
}

/* Public helper for builtins.c to render a job line consistently. */
const char *ms_job_state_label(ms_job_state s) { return state_label(s); }
