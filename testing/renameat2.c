/*
 * Copyright (c) 2023 Russell Harmon
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#define _GNU_SOURCE
#include <stdlib.h>
#include <fcntl.h>
#include <stdio.h>
#include <getopt.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include <sys/syscall.h>

/*
 * The code in this file was obtained from https://gist.github.com/eatnumber1 .
 * The file is called renameat2.c and was fetched on 20230619. Thanks to Russel.
 * It doesn't work well with /sys as that file system instance is perpetually
 * busy, probably thanks to systemd, udev and other deamons.
 * Another avenue to investigate is 'mount --move <olddir> <newdir>' but not
 * on my development machine :-)
 *               dpg 20230620
 */

#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1 << 0) /* Don't overwrite target */ // from include/uapi/linux/fs.h
#endif

#ifndef RENAME_EXCHANGE
#define RENAME_EXCHANGE (1 << 1) /* Exchange source and dest */ // from include/uapi/linux/fs.h
#endif

#ifndef SYS_renameat2
#if defined(__x86_64__)
#define SYS_renameat2 314 // from arch/x86/syscalls/syscall_64.tbl
#elif defined(__i386__)
#define SYS_renameat2 353 // from arch/x86/syscalls/syscall_32.tbl
#else
#error Architecture unsupported
#endif
#endif // ifndef SYS_renameat2

static void fatal_fprintf(FILE *out, const char * restrict fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    int ret = vfprintf(out, fmt, ap);
    int saved_errno = errno;
    va_end(ap);

    if (ret < 0) {
        errno = saved_errno;
        perror("vfprintf");
        exit(EXIT_FAILURE);
    }
}

static void print_usage(FILE *out, char *progname) {
    fatal_fprintf(out, "Usage: %s [options] SOURCE DEST\n", progname);
    fatal_fprintf(out, "Call the renameat2(2) system call.\n");
    fatal_fprintf(out, "\n");
    fatal_fprintf(out, " -h, --help      This help message\n");
    fatal_fprintf(out, " -e, --exchange  Atomically exchange SOURCE and DEST\n");
    fatal_fprintf(out, " -n, --noreplace Don't overwrite DEST if it already exists\n");
}

int main(int argc, char *argv[]) {
    int flags = 0;

    while (true) {
        static const struct option long_options[] = {
            {"exchange", no_argument, NULL, 'e'},
            {"noreplace", no_argument, NULL, 'n'},
            {"help", no_argument, NULL, 'h'},
            {NULL, 0, NULL, 0}
        };

        int c = getopt_long(argc, argv, "enh", long_options, NULL);
        if (c == -1) {
            break;
        }

        switch (c) {
            case 'n':
                flags |= RENAME_NOREPLACE;
                break;
            case 'e':
                flags |= RENAME_EXCHANGE;
                break;
            case 'h':
                print_usage(stdout, argv[0]);
                exit(EXIT_SUCCESS);
            case '?':
                print_usage(stderr, argv[0]);
                exit(EXIT_FAILURE);
            default:
                fprintf(stderr, "?? getopt returned character code 0%o ??\n", c);
                exit(EXIT_FAILURE);
        }
    }

    if (argc - optind != 2) {
        print_usage(stderr, argv[0]);
        exit(EXIT_FAILURE);
    }
    char *source = argv[optind], *dest = argv[optind + 1];

    if (syscall(SYS_renameat2, AT_FDCWD, source, AT_FDCWD, dest, flags) != 0) {
        perror("renameat2");
        exit(EXIT_FAILURE);
    }
}
