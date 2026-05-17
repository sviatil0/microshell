/*
 * history.h - command history, in-memory ring + ~/.microshell_history.
 */
#ifndef MICROSHELL_HISTORY_H
#define MICROSHELL_HISTORY_H

#include <stddef.h>

#define MS_HISTORY_MAX 500

void ms_history_init(void);          /* loads ~/.microshell_history if any */
void ms_history_shutdown(void);      /* flushes + frees                    */

/* Append `line` (without a trailing newline). Duplicates of the most
 * recent entry are suppressed. Empty/whitespace-only lines are dropped. */
void ms_history_add(const char *line);

/* Iterate every entry, oldest first, calling fn(index, text, ud). */
void ms_history_each(void (*fn)(int, const char *, void *), void *ud);

#endif /* MICROSHELL_HISTORY_H */
