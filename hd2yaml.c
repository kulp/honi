/**
 * Converts a file in HoNData hierarchical format to YAML or formatted HoNData.
 * Takes one argument, the file to parse. The output file may be specified with
 * the @c -o option. If no such option is provided, the output is written to @c
 * stdout. The default output option is YAML; this can also be specified with
 * the @c -f @c yaml option. The alternate format, pretty-printed HoNData, can
 * be specified with @c -f @c pretty.
 */

#include "hd_parser.h"
#include "filestore.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

void usage(const char *me)
{
    printf("Usage:\n"
           "  %s [ OPTIONS ] filename\n"
           "where OPTIONS are among\n"
           "  -f fmt    select output format (\"yaml\" or \"pretty\")\n"
           "  -h        show this usage message\n"
           "  -o file   write output to this filename (default: stdout)\n"
           , me);
}

int main(int argc, char *argv[])
{
    int rc = EXIT_SUCCESS;

    struct hd_parser_state *state;
    hd_dumper_t dumper = hd_yaml2;
    char filename[4096];
    filename[0] = 0;

    extern char *optarg;
    extern int optind, optopt;
    int ch;
    while ((ch = getopt(argc, argv, "f:ho:")) != -1) {
        switch (ch) {
            case 'f': 
                     if (!strcasecmp(optarg, "yaml"  )) dumper = hd_yaml;
                else if (!strcasecmp(optarg, "pretty")) dumper = hd_dump;
                else {
                    fprintf(stderr, "Invalid format '%s'\n", optarg);
                    return EXIT_FAILURE;
                }
                break;
            case 'o':
                strncpy(filename, optarg, sizeof filename);
                break;
            default : rc = EXIT_FAILURE; /* FALLTHROUGH */
            case 'h': usage(argv[0]); return EXIT_FAILURE;
        }
    }

    if (argc - optind < 1 || argc - optind > 2) {
        fprintf(stderr, "Supply an input filename and an optional output filename\n");
        return EXIT_FAILURE;
    }

    rc = hd_init(&state);
    rc = hd_read_file_init(state, argv[optind]);
    if (rc) {
        fprintf(stderr, "Failed to open input file '%s'\n", argv[optind]);
        return EXIT_FAILURE;
    }

    int fd;
    if (filename[0]) {
        fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (fd < 0) {
            fprintf(stderr, "Failed to open output file '%s'\n", filename);
            return EXIT_FAILURE;
        }
    } else {
        fd = fileno(stdout);
    }

    struct node *result = hd_parse(state);
    rc = dumper(fd, result, HD_PRINT_PRETTY);
    rc = hd_read_file_fini(state);
    rc = hd_fini(&state);
    hd_free(result);

    return rc;
}

/* vim:set et ts=4 sw=4 syntax=c.doxygen: */

