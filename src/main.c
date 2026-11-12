/*
 * xbemu: run a Win64-format x86-64 PE program on Linux without Wine.
 *
 * Usage: xbemu [-v] program.exe
 *   XBEMU_PATH   colon-separated directories searched for imported DLLs
 *                (the program's own directory is always searched too)
 */
#define _DEFAULT_SOURCE
#include "pe.h"
#include "teb.h"
#include "host/hostlib.h"
#include "w64abi.h"

#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void add_search_path_list(const char *list) {
    char *copy = strdup(list);
    if (!copy) return;
    for (char *tok = strtok(copy, ":"); tok; tok = strtok(NULL, ":"))
        if (*tok) pe_add_search_dir(tok);
    free(copy);
}

int main(int argc, char **argv) {
    int verbose = 0;
    const char *path = NULL;

    int guest_start = argc;
    for (int i = 1; i < argc; i++) {
        if (!path && strcmp(argv[i], "-v") == 0) verbose = 1;
        else if (!path) { path = argv[i]; guest_start = i; break; }
    }
    if (!path) {
        fprintf(stderr, "usage: xbemu [-v] program.exe\n");
        return 2;
    }

    /* Search the program's directory first, then XBEMU_PATH. */
    char *dup = strdup(path);
    if (dup) {
        pe_add_search_dir(dirname(dup));
        free(dup);
    }
    const char *env = getenv("XBEMU_PATH");
    if (env) add_search_path_list(env);

    teb_init_current_thread();
    /* The guest's argv: the program path, then everything after it. */
    hostlib_set_args(argc - guest_start, argv + guest_start);

    char err[256];
    PeModule *prog = pe_load_module(path, err, sizeof err);
    if (!prog) {
        fprintf(stderr, "xbemu: %s\n", err);
        return 1;
    }
    if (!prog->entry) {
        fprintf(stderr, "xbemu: %s has no entry point\n", path);
        return 1;
    }
    if (verbose)
        fprintf(stderr, "[xbemu] loaded %s at %p (size 0x%zx, entry %p)\n",
                path, (void *)prog->base, prog->size, (void *)prog->entry);

    uint64_t rc = xbemu_call_w64(prog->entry, 0, 0, 0, 0);
    return (int)(rc & 0xFF);
}
