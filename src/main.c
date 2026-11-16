#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
/*
 * main.c - CLI entry point for chcl.
 *
 * Provides three subcommands:
 *   chcl fmt     [-w] [--check] <file>  Format (stdout, or in-place)
 *   chcl to-json                <file>  Convert HCL to JSON
 *   chcl lint                   <file>  Validate syntax, exit non-zero on error
 *
 * Exit codes:
 *   0 = success
 *   1 = usage error or I/O error
 *   2 = parse/lint error
 */
#include "chcl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include <sys/stat.h>
#else
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#endif

/* Create exclusively beside the destination; never truncate the original. */
static int replace_file(const char *path, const char *text) {
    size_t len = strlen(path);
    if (len > SIZE_MAX - 40) return 0;
    char *temporary = malloc(len + 40);
    if (!temporary) return 0;
#ifndef _WIN32
    struct stat original;
    /* Refuse links and special files rather than replacing their identities. */
    if (lstat(path, &original) || !S_ISREG(original.st_mode) || original.st_nlink != 1) {
        free(temporary); return 0;
    }
#endif
    FILE *stream = NULL;
    for (unsigned i = 0; i < 1000; i++) {
        snprintf(temporary, len + 40, "%s.chcl-tmp-%u", path, i);
#ifdef _WIN32
        int fd = _open(temporary, _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY, _S_IREAD | _S_IWRITE);
        if (fd >= 0) {
            stream = _fdopen(fd, "wb");
            if (!stream) { _close(fd); remove(temporary); free(temporary); return 0; }
        }
#else
        int fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd >= 0) {
            stream = fdopen(fd, "wb");
            if (!stream) { close(fd); remove(temporary); free(temporary); return 0; }
        }
#endif
        if (stream || errno != EEXIST) break;
    }
    if (!stream) { free(temporary); return 0; }
    int ok = fputs(text, stream) != EOF && fflush(stream) == 0;
#ifndef _WIN32
    if (ok && fchmod(fileno(stream), original.st_mode & 0777)) ok = 0;
    if (ok && fsync(fileno(stream))) ok = 0;
#else
    if (ok && _commit(_fileno(stream))) ok = 0;
#endif
    if (fclose(stream)) ok = 0;
#ifndef _WIN32
    if (ok && rename(temporary, path)) ok = 0;
#else
    if (ok && !ReplaceFileA(path, temporary, NULL, 0, NULL, NULL)) ok = 0;
#endif
    if (!ok) remove(temporary);
    free(temporary);
    return ok;
}

static void usage(void) {
    fprintf(stderr,
        "chcl / HCL toolkit in C (formatter, linter, JSON)\n"
        "\n"
        "Usage:\n"
        "   chcl fmt [-w] [--check] <file.hcl>  Format (or check) the file\n"
        "   chcl to-json <file.hcl>     Convert to JSON\n"
        "   chcl lint <file.hcl>        Validate the syntax\n"
        "   chcl --help                 Show this help\n"
        "   chcl --version              Print version\n");
}

int main(int argc, char **argv) {
#ifdef _WIN32
    if (_setmode(_fileno(stdout), _O_BINARY) == -1) return 1;
#endif
    if (argc < 2) { usage(); return 1; }

    const char *cmd = argv[1];

    /* --help / --version never read a file. */
    if (!strcmp(cmd, "--help") || !strcmp(cmd, "-h")) { usage(); return argc == 2 ? 0 : 1; }
    if (!strcmp(cmd, "--version")) {
        if (argc != 2) { usage(); return 1; }
        return puts("chcl 0.1.0") == EOF || fflush(stdout) ? 1 : 0;
    }

    /* --- fmt ---------------------------------------------------- */
    if (!strcmp(cmd, "fmt")) {
        int inplace = 0, check = 0, argi = 2;

        /* Parse leading flags in any order. */
        for (; argi < argc && argv[argi][0] == '-'; argi++) {
            if (!strcmp(argv[argi], "--")) { argi++; break; }
            if      (!strcmp(argv[argi], "-w"))      inplace = 1;
            else if (!strcmp(argv[argi], "--check")) check   = 1;
            else { usage(); return 1; }
        }
        if (argi != argc - 1 || (inplace && check)) { usage(); return 1; }
        const char *file = argv[argi];

        /* --check: exit 1 and print the filename if reformatting needed. */
        if (check) {
            int same = chcl_format_is_canonical(file);
            if (same < 0) {
                fprintf(stderr, "error: cannot read or process %s\n", file);
                return 1;
            }
            if (!same) { puts(file); return 1; }
            return 0;
        }

        /* Normal fmt: stdout, or -w to overwrite the file. */
        char *out = chcl_format_file(file);
        if (!out) {
            fprintf(stderr, "error: cannot read or process %s\n", file);
            return 1;
        }
        int ok;
        if (inplace) ok = replace_file(file, out);
        else ok = fputs(out, stdout) != EOF && fflush(stdout) == 0;
        free(out);
        if (!ok) fprintf(stderr, "error: cannot write %s\n", inplace ? file : "stdout");
        return ok ? 0 : 1;
    }

    /* --- to-json ----------------------------------------------- */
    if (!strcmp(cmd, "to-json")) {
        if (argc != 3) { usage(); return 1; }

        chcl_document_t *d = chcl_parse_file(argv[2]);
        if (!d) {
            fprintf(stderr, "error: cannot read or process %s\n", argv[2]);
            return 1;
        }

        if (chcl_document_error(d)) {
            fprintf(stderr, "%s:%d:%d: error: %s\n", argv[2],
                    chcl_document_error_line(d),
                    chcl_document_error_column(d),
                    chcl_document_error(d));
            chcl_free_document(d);
            return 2;
        }

        char *json = chcl_to_json(d);
        int ok = json && puts(json) != EOF && fflush(stdout) == 0;
        free(json);
        chcl_free_document(d);
        if (!ok) fprintf(stderr, "error: serialization or output failed\n");
        return ok ? 0 : 1;
    }

    /* --- lint -------------------------------------------------- */
    if (!strcmp(cmd, "lint")) {
        if (argc != 3) { usage(); return 1; }

        chcl_document_t *d = chcl_parse_file(argv[2]);
        if (!d) {
            fprintf(stderr, "error: cannot read or process %s\n", argv[2]);
            return 1;
        }

        int rc = 0;
        if (chcl_document_error(d)) {
            fprintf(stderr, "%s:%d:%d: error: %s\n", argv[2],
                    chcl_document_error_line(d),
                    chcl_document_error_column(d),
                    chcl_document_error(d));
            rc = 2;
        }
        chcl_free_document(d);
        return rc;
    }

    usage();
    return 1;
}
