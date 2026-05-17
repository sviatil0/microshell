/*
 * builtins.h - shell-internal commands.
 *
 * Builtins return an exit status (0..255) that becomes $?. Some
 * builtins (cd, exit, export) must run in the shell process itself;
 * pipeline stages call into the same registry but inside the child.
 */
#ifndef MICROSHELL_BUILTINS_H
#define MICROSHELL_BUILTINS_H

#include "parser.h"

/* Returns 1 if `name` is a builtin, 0 otherwise. */
int ms_is_builtin(const char *name);

/* Execute a builtin and return its exit status. Caller guarantees
 * argv[0] is a known builtin. `out_should_exit` is set to non-zero
 * when the builtin requested the shell to terminate (`exit`). */
int ms_run_builtin(ms_cmd *cmd, int *out_should_exit);

#endif /* MICROSHELL_BUILTINS_H */
