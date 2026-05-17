/*
 * executor.h - run a parsed pipeline.
 *
 * The executor handles three cases:
 *   1. A single builtin in the foreground - run it in-process.
 *   2. A single external command - fork/exec, wait if not backgrounded.
 *   3. A multi-stage pipeline - fork each stage, wire pipes between
 *      adjacent stages, and wait on the whole pipeline (or register
 *      it as a background job).
 *
 * Process-group management is centralized here: every child is placed
 * into the pgid of the first stage, and tcsetpgrp() hands the
 * terminal to that group for foreground execution.
 */
#ifndef MICROSHELL_EXECUTOR_H
#define MICROSHELL_EXECUTOR_H

#include <sys/types.h>
#include "parser.h"

/* Execute a pipeline. Returns the resulting exit status (0..255 or
 * 128+sig for terminating signals). Sets *out_should_exit if a
 * builtin requested shutdown. */
int ms_execute(ms_pipeline *p, int *out_should_exit);

/* Configure the shell pgid + terminal fd once at startup. */
void ms_executor_init(pid_t shell_pgid, int shell_tty_fd, int interactive);

#endif /* MICROSHELL_EXECUTOR_H */
