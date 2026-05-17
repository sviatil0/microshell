/*
 * jobs.h - background/foreground job tracking
 *
 * Each job is one pipeline: a process group leader pid plus a freeform
 * description recovered from the source line. Slots are recycled on
 * job removal. The table is small and bounded - dynamic resizing isn't
 * worth the complexity for an interactive shell.
 */
#ifndef MICROSHELL_JOBS_H
#define MICROSHELL_JOBS_H

#include <sys/types.h>

#define MS_JOBS_MAX 64

typedef enum {
    MS_JOB_FREE = 0,
    MS_JOB_RUNNING,
    MS_JOB_STOPPED,
    MS_JOB_DONE
} ms_job_state;

typedef struct {
    int id;                 /* user-facing job number (1-based) */
    pid_t pgid;             /* process group id */
    ms_job_state state;
    char *cmdline;          /* heap-owned description */
    int notified;           /* reported to user already? */
} ms_job;

void ms_jobs_init(void);
void ms_jobs_shutdown(void);

/* Register a new background/stopped job. Returns the assigned id (>=1)
 * or -1 if the table is full. Takes ownership of `cmdline`. */
int  ms_jobs_add(pid_t pgid, const char *cmdline, ms_job_state state);

/* Locate a job by id (1-based). NULL when not found or freed. */
ms_job *ms_jobs_find(int id);
ms_job *ms_jobs_find_by_pgid(pid_t pgid);

/* Most recently added live job (for "fg"/"bg" with no args). */
ms_job *ms_jobs_current(void);

/* Reap finished children via WNOHANG and update job states. Must be
 * called from the SIGCHLD handler or the main loop tick. */
void ms_jobs_reap(void);

/* Print a "[id]+ state cmd" line for completed jobs and mark them free. */
void ms_jobs_print_pending(void);

/* Iterate every non-free slot. Returns slot count visited. */
int  ms_jobs_each(void (*fn)(const ms_job *, void *), void *ud);

/* Pretty-print state name for a job. */
const char *ms_job_state_label(ms_job_state s);

#endif /* MICROSHELL_JOBS_H */
