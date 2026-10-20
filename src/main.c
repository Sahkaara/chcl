/*
 * main.c - CLI entry point for chcl.
 *
 * Provides three subcommands:
 *   chcl fmt     [-w] <file>  Format (stdout, or in-place with -w)
 *   chcl to-json       <file>  Convert HCL to JSON
 *   chcl lint          <file>  Validate syntax, exit non-zero on error
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

static void usage(void) {
    fprintf(stderr,
        "chcl / HCL toolkit in C (formatter, linter, JSON)\n"
        "\n"
        "Usage:\n"
        "   chcl fmt [/w] <file.hcl>    Format (stdout, or in-place with -w)\n"
        "   chcl to-json <file.hcl>     Convert to JSON\n"
        "   chcl lint <file.hcl>        Validate the syntax\n"
        "   chcl --help                 Show this help\n"
        "   chcl --version              Print version\n");
}

    int main(int argc, char **argv) {
        if(argc < 2) { usage(); return 1; }

        const char *cmd = argv[1];

        /* --help / --version never read a file. */
        if (!strcmp(cmd, "--help") || !strcmp(cmd, "-h")) { usage(); return 0; }
        if (!strcmp(cmd, "--version")) { puts("chcl 0.1.0"); return 0; }

        /* --- fmt -------------------------------------------------- */
        if (!strcmp(cmd, "fmt")) {
            int inplace = 0, idx = 2;
            if (argc > 2 && !strcmp(argv[2], "-w")) { inplace = 1; idx = 3; }
            if (argc <= idx) { usage(); return 1; }

            char *out = chcl_format_file(argv[idx]);
            if (!out) {
                fprintf(stderr, "error: cannot read %s\n", argv[idx]);
                return 1;
            }

            if (inplace) {
                FILE *f = fopen(argv[idx], "wb");
                if (!f) {
                    free(out);
                    fprintf(stderr, "error: cannot write %s\n", argv[idx]);
                    return 1;
                }
                fputs(out, f);
                fclose(f);
            } else {
                fputs(out, stdout);
            }
            free(out);
            return 0;
        }

        /* --- to-json ---------------------------------------------- */
        if (!strcmp(cmd, "to-json")) {
            if (argc < 3) { usage(); return 1; }

            
        }
}

