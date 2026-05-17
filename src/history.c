/*
 * history.c - file-backed command history.
 *
 * We keep a fixed-size ring buffer in memory and persist the full
 * (deduplicated, trimmed) list back to disk on shutdown. Loading is
 * tolerant of a missing file - first-run users just get an empty list.
 */
#include "history.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pwd.h>
#include <unistd.h>
#include <sys/types.h>

static char *g_entries[MS_HISTORY_MAX];
static int   g_count = 0;

static int is_blank(const char *s) {
    for (; *s; s++) if (!isspace((unsigned char)*s)) return 0;
    return 1;
}

static char *history_path(void) {
    static char path[1024];
    const char *home = getenv("HOME");
    if (!home || !*home) {
        struct passwd *pw = getpwuid(getuid());
        home = pw ? pw->pw_dir : ".";
    }
    snprintf(path, sizeof path, "%s/.microshell_history", home);
    return path;
}

static void push_internal(const char *line) {
    if (g_count > 0 && strcmp(g_entries[g_count - 1], line) == 0) return;
    if (g_count == MS_HISTORY_MAX) {
        free(g_entries[0]);
        memmove(&g_entries[0], &g_entries[1],
                sizeof(char *) * (MS_HISTORY_MAX - 1));
        g_count--;
    }
    char *dup = strdup(line);
    if (!dup) return;
    g_entries[g_count++] = dup;
}

void ms_history_init(void) {
    g_count = 0;
    FILE *f = fopen(history_path(), "r");
    if (!f) return;
    char buf[4096];
    while (fgets(buf, sizeof buf, f)) {
        size_t n = strlen(buf);
        while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = '\0';
        if (n && !is_blank(buf)) push_internal(buf);
    }
    fclose(f);
}

void ms_history_add(const char *line) {
    if (!line || !*line || is_blank(line)) return;
    push_internal(line);
}

void ms_history_shutdown(void) {
    FILE *f = fopen(history_path(), "w");
    if (f) {
        for (int i = 0; i < g_count; i++) fprintf(f, "%s\n", g_entries[i]);
        fclose(f);
    }
    for (int i = 0; i < g_count; i++) { free(g_entries[i]); g_entries[i] = NULL; }
    g_count = 0;
}

void ms_history_each(void (*fn)(int, const char *, void *), void *ud) {
    for (int i = 0; i < g_count; i++) fn(i + 1, g_entries[i], ud);
}
