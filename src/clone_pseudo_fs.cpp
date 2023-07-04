/*
 * Copyright (c) 2023 Douglas Gilbert.
 * All rights reserved.
 * Use of this source code is governed by a BSD-style
 * license that can be found in the BSD_LICENSE file.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/* This is a utility program for cloning Linux pseudo file systems
 * but may be applicable elsewhere. Normal CLI tools (e.g. find and
 * tar) have problems with sysfs, for example, because regular files
 * in sysfs (i.e. attributes) do not correctly report their file
 * size in their associated 'struct stat' instance.
 *
 */


// Initially this utility will assume C++20 or later

static const char * const version_str = "0.90 20230704 [svn: r6]";

#include <iostream>
#include <fstream>
#include <cstdint>
#include <filesystem>
#include <vector>
#include <map>
#include <ranges>
#include <algorithm>            // needed for ranges::sort()
#include <source_location>
#include <chrono>
#include <cstring>              // needed for strstr()
#include <cstdio>               // using sscanf()
// Unix C headers below
#include <unistd.h>
#include <getopt.h>
#include <fcntl.h>
#include <glob.h>
#include <poll.h>
#include <sys/stat.h>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

static const unsigned int def_reglen = 256;

namespace fs = std::filesystem;
namespace chron = std::chrono;

using sstring=std::string;
static auto & scout { std::cout };
static auto & scerr { std::cerr };

static int verbose;  // don't want to pass 'struct opts_t' just to get this


struct stats_t {
    unsigned int num_exist;     /* should be all valid file types */
    unsigned int num_not_exist;
    unsigned int num_dir;  /* directories that are not symlinks */
    unsigned int num_sym2dir;
    unsigned int num_sym2reg;
    unsigned int num_sym2block;
    unsigned int num_sym2char;
    unsigned int num_sym_other;
    unsigned int num_sym_hang;
    unsigned int num_hidden_skipped;
    unsigned int num_regular;
    unsigned int num_block;
    unsigned int num_char;
    unsigned int num_fifo;
    unsigned int num_socket;
    unsigned int num_other;
    unsigned int num_hidden;
    unsigned int num_excluded;
    unsigned int num_dir_d_success;
    unsigned int num_sym_d_success;
    unsigned int num_error;
    // above calculated during source scan (apart from *_d_success fields)
    // below calculated during transfer of regular files
    unsigned int num_reg_tries;       // only incremented when dst active
    unsigned int num_reg_success;
    unsigned int num_reg_s_at_reglen;
    unsigned int num_reg_s_eacces;
    unsigned int num_reg_s_eperm;
    unsigned int num_reg_s_eio;
    unsigned int num_reg_s_enodata;
    unsigned int num_reg_s_enoent_enodev_enxio;
    unsigned int num_reg_s_eagain;
    unsigned int num_reg_s_timeout;
    unsigned int num_reg_s_e_other;
    unsigned int num_reg_d_eacces;
    unsigned int num_reg_d_eperm;
    unsigned int num_reg_d_eio;
    unsigned int num_reg_d_e_other;
    int max_depth;
};

struct opts_t {
    bool destination_given;
    bool destin_all_new;
    bool max_depth_active;  // for depth: 0 means one level below source_pt
    bool no_destin;
    bool clone_hidden;  // that is files starting with '.'
    bool no_xdev;       // xdev in find means don't scan outside original fs
    bool source_given;
    bool wait_given;
    bool want_stats;
    unsigned int reglen;
    unsigned int wait_ms;
    int max_depth;     // one less than given on cl
    // int verbose;    // make file scope
    mutable struct stats_t stats;
    fs::path source_pt;         // a directory in canonical form
    fs::path destination_pt;    // (will be) a directory in canonical form
    std::vector<fs::path> exclude_v;    // vector of canonical paths
};

static const struct option long_options[] = {
    {"destination", required_argument, 0, 'd'},
    {"dst", required_argument, 0, 'd'},
    {"exclude", required_argument, 0, 'e'},
    {"help", no_argument, 0, 'h'},
    {"hidden", no_argument, 0, 'H'},
    {"max-depth", required_argument, 0, 'm'},
    {"max_depth", required_argument, 0, 'm'},
    {"maxdepth", required_argument, 0, 'm'},
    {"no-destination", no_argument, 0, 'D'},
    {"no_destination", no_argument, 0, 'D'},
    {"no-dst", no_argument, 0, 'D'},
    {"no_dst", no_argument, 0, 'D'},
    {"no-xdev", no_argument, 0, 'N'},
    {"no_xdev", no_argument, 0, 'N'},
    {"reglen", required_argument, 0, 'r'},
    {"source", required_argument, 0, 's'},
    {"src", required_argument, 0, 's'},
    {"statistics", no_argument, 0, 'S'},
    {"stats", no_argument, 0, 'S'},
    {"verbose", no_argument, 0, 'v'},
    {"version", no_argument, 0, 'V'},
    {"wait", required_argument, 0, 'w'},
    {0, 0, 0, 0},
};

static sstring sysfs_root { "/sys" };   // default source
static sstring def_destin_root { "/tmp/sys" };
static const int stat_perm_mask = 0x1ff;         /* bottom 9 bits */

static auto dir_opt = fs::directory_options::skip_permission_denied;



static const char * const usage_message1 =
    "Usage: clone_pseudo_fs [--destination=DPATH] [--exclude=PAT] [--help]\n"
    "                       [--hidden] [--max-depth=MAXD] [--no-dst]\n"
    "                       [--no-xdev] [--reglen=RLEN] [--source=SPATH]\n"
    "                       [--statistics] [--verbose] [--version]\n"
    "                       [--wait=MS_R]\n"
    "  where:\n"
    "    --destination=DPATH|-d DPATH    DPATH is clone destination (def:\n"
    "                                    /tmp/sys)\n"
    "    --exclude=PAT|-e PAT    PAT is a glob pattern, matching files and\n"
    "                            directories excluded (def: nothing "
    "excluded)\n"
    "    --help|-h          this usage information\n"
    "    --hidden|-H        clone hidden files (def: ignore them)\n"
    "    --max-depth=MAXD|-m MAXD    maximum depth of scan (def: 0 which "
    "means\n"
    "                                there is no limit)\n"
    "    --no-dst|-D        ignore destination, just do SPATH scan\n"
    "    --no-xdev|-N       clone of SPATH may span multiple file systems "
    "(def:\n"
    "                       stay in SPATH's containing file system)\n"
    "    --reglen=RLEN|-r RLEN    maximum length to clone of each regular "
    "file\n"
    "                             (def: 256 bytes)\n"
    "    --source=SPATH|-s SPATH    SPATH is source for clone (def: /sys)\n"
    "    --statistics|-S    gather then output statistics (helpful with "
    "--no-dst)\n"
    "    --verbose|-v       increase verbosity\n"
    "    --version|-V       output version string and exit\n"
    "    --wait=MS_R|-w MS_R    MS_R is number of milliseconds to wait on "
    "each\n"
    "                           regular file read(2) call (def: "
    "indefinite)\n"
    "\n";

static const char * const usage_message2 =
    "By default, this utility will clone /sys to /tmp/sys . The resulting "
    "subtree\nis a frozen snapshot that may be useful for later analysis. "
    "Hidden files\nare skipped and symlinks are created, even if dangling. "
    "The default is only\nto copy a maximum of 256 bytes of regular files."
    "\n";

static void
usage()
{
    scout << usage_message1;
    scout << usage_message2;
}

static void
pr2ser(const sstring & emsg, const std::error_code & ec = { },
       const std::source_location loc = std::source_location::current())
{
    if (emsg.size() == 0) {     /* shouldn't need location.column() */
        if (verbose > 1)
            scerr << loc.file_name() << " " << loc.function_name() << ";ln="
                  << loc.line() << "\n";
        else
            scerr << "pr2ser() called but no message?\n";
    } else if (ec) {
        if (verbose > 1)
            scerr << loc.function_name() << ";ln=" << loc.line() << ": "
                  << emsg << ", error: " << ec.message() << "\n";
        else
            scerr << emsg << ", error: " << ec.message() << "\n";
    } else {
        if (verbose > 1)
            scerr << loc.function_name() << ";ln=" << loc.line() <<  ": "
                  << emsg << "\n";
        else
            scerr << emsg << "\n";
    }
}

static void
pr3ser(const sstring & e1msg, const char * e2msg = NULL,
       const std::error_code & ec = { },
       const std::source_location loc = std::source_location::current())
{
    if (e2msg == nullptr)
        pr2ser(e1msg, ec, loc);
    else if (ec) {
        if (verbose > 1)
            scerr << loc.function_name() << ";ln=" << loc.line() << ": '"
                  << e1msg << "': " << e2msg << ", error: "
                  << ec.message() << "\n";
        else
            scerr << "'" << e1msg << "': " << e2msg << ", error: "
                  << ec.message() << "\n";
    } else {
        if (verbose > 1)
            scerr << loc.function_name() << ";ln=" << loc.line() << ": '"
                  << e1msg << "': " << e2msg << "\n";
        else
            scerr << "'" << e1msg << "': " << e2msg << "\n";
    }
}

static void
pr4ser(const sstring & e1msg, const sstring & e2msg,
       const char * e3msg = NULL, const std::error_code & ec = { },
       const std::source_location loc = std::source_location::current())
{
    if (e3msg == nullptr)
        pr3ser(e1msg, e2msg.c_str(), ec, loc);
    else if (ec) {
        if (verbose > 1)
            scerr << loc.function_name() << ";ln=" << loc.line() << ": '"
                  << e1msg << "," << e2msg << "': " << e3msg
                  << ", error: " << ec.message() << "\n";
        else
            scerr << "'" << e1msg << "," << e2msg << "': " << e3msg
                  << ", error: " << ec.message() << "\n";
    } else {
        if (verbose > 1)
            scerr << loc.function_name() << ";ln=" << loc.line() << ": '"
                  << e1msg << "," << e2msg << "': " << e3msg << "\n";
        else
            scerr << "'" << e1msg << "," << e2msg << "': " << e3msg << "\n";
    }
}

// This assumes both paths are in canonical form
static bool
path_contains_canon(const fs::path & haystack_c_pt,
                    const fs::path & needle_c_pt)
{
    auto hay_c_sz { haystack_c_pt.string().size() };
    auto need_c_sz { needle_c_pt.string().size() };

    if (need_c_sz == hay_c_sz)
        return needle_c_pt == haystack_c_pt;
    else if (need_c_sz < hay_c_sz)
        return false;

    auto c_need { needle_c_pt };        // since while loop modifies c_need

    do {
        // step needle back to its parent
        c_need = c_need.parent_path();
        need_c_sz = c_need.string().size();
    } while (need_c_sz > hay_c_sz);

    if (need_c_sz < hay_c_sz)
        return false;
    // here iff need_c_sz==hay_c_sz
    return c_need == haystack_c_pt;
}

#if 0
// Not required yet: path_contains() version for non-canonical paths
static bool
path_contains(const fs::path & haystack, const fs::path & needle,
              std::error_code & ec)
{
    fs::path c_hay { fs::canonical(haystack, ec) };
    if (ec)
        return false;
    fs::path c_need { fs::canonical(needle, ec) };
    if (ec)
        return false;

    ec.clear();
    return path_contains_canon(c_hay, c_need);
}
#endif

// Returns number of bytes read, -1 for general error, -2 for timeout
static int
read_err_wait(int from_fd, uint8_t * bp, int err, const struct opts_t *op)
{
    int num = -1;

    if (err == EAGAIN) {
        ++op->stats.num_reg_s_eagain;
        if (op->wait_given) {
            struct pollfd a_pollfd = {0, POLLIN, 0};

            a_pollfd.fd = from_fd;
            int r = poll(&a_pollfd, 1, op->wait_ms);
            if (r == 0) {
                ++op->stats.num_reg_s_timeout;
                return -2;
            } else if (r > 0) {
                if (a_pollfd.revents & POLLIN) {
                    num = read(from_fd, bp, op->reglen);
                    if (num >= 0)
                        return num;
                    else
                        err = errno;
                } else if (a_pollfd.revents & POLLERR)
                    err = EPROTO;
            }
        }
    }
    if (err == EACCES)
        ++op->stats.num_reg_s_eacces;
    else if (err == EPERM)
        ++op->stats.num_reg_s_eperm;
    else if (err == EIO)
        ++op->stats.num_reg_s_eio;
    else if (err == ENODATA)
        ++op->stats.num_reg_s_enodata;
    else if ((err == ENOENT) || (err == ENODEV) || (err == ENXIO))
        ++op->stats.num_reg_s_enoent_enodev_enxio;
    else
        ++op->stats.num_reg_s_e_other;

    return num;
}

static int
xfer_regular_file(const sstring & from_file, const sstring & destin_file,
                  const struct opts_t * op)
{
    int res = 0;
    int from_fd = -1;
    int destin_fd = -1;
    int rd_flags = O_RDONLY;
    int from_perms, num, num2;
    uint8_t * bp;
    const char * from_nm = from_file.c_str();
    const char * destin_nm = destin_file.c_str();
    struct stat from_stat;
    uint8_t fix_b[def_reglen];

    ++op->stats.num_reg_tries;
    if (op->reglen <= def_reglen)
        bp = fix_b;
    else
        bp = static_cast<uint8_t *>(malloc(op->reglen));
    if (bp == nullptr)
        return ENOMEM;

    if (op->wait_given && (op->reglen > 0))
        rd_flags |= O_NONBLOCK;
    from_fd = open(from_nm, rd_flags);
    if (from_fd < 0) {
        res = errno;
        if (res == EACCES) {
            if (stat(from_nm, &from_stat) < 0) {
                res = errno;
                if (res == EACCES)
                    ++op->stats.num_reg_s_eacces;
                else if (res == EPERM)
                    ++op->stats.num_reg_s_eperm;
                else if (res == EIO)
                    ++op->stats.num_reg_s_eio;
                else if ((res == ENOENT) || (res == ENODEV) || (res == ENXIO))
                    ++op->stats.num_reg_s_enoent_enodev_enxio;
                else
                    ++op->stats.num_reg_s_e_other;
                goto fini;
            }
            from_perms = from_stat.st_mode & stat_perm_mask;
            num = 0;
            goto do_destin;
        } else if (res == EPERM)
            ++op->stats.num_reg_s_eperm;
        else if (res == EIO)
            ++op->stats.num_reg_s_eio;
        else if ((res == ENOENT) || (res == ENODEV) || (res == ENXIO))
            ++op->stats.num_reg_s_enoent_enodev_enxio;
        else
            ++op->stats.num_reg_s_e_other;
        goto fini;
    }
    if (fstat(from_fd, &from_stat) < 0) {
        res = errno;
        ++op->stats.num_reg_s_e_other;  // not expected if open() is good
        goto fini;
    }
    from_perms = from_stat.st_mode & stat_perm_mask;
    if (op->reglen > 0) {
        num = read(from_fd, bp, op->reglen);
        if (num < 0) {
            res = errno;
            num = read_err_wait(from_fd, bp, res, op);
            if (num < 0) {
                if ((num == -2) && (verbose > 0))
                    pr3ser(from_file, "<< timed out waiting for this file");
                num = 0;
                close(from_fd);
                goto do_destin;
            }
        }
    } else
        num = 0;
    // closing now might help in this function is multi-threaded
    close(from_fd);
    if (static_cast<unsigned int>(num) >= op->reglen)
        ++op->stats.num_reg_s_at_reglen;
do_destin:
    from_fd = -1;
    if (op->destin_all_new) {
        destin_fd = creat(destin_nm, from_perms);
        if (destin_fd < 0) {
            res = errno;
            if (res == EACCES)
                ++op->stats.num_reg_d_eacces;
            else if (res == EPERM)
                ++op->stats.num_reg_d_eperm;
            else if (res == EIO)
                ++op->stats.num_reg_d_eio;
            else
                ++op->stats.num_reg_d_e_other;
            goto fini;
        }
    } else {
        destin_fd = open(destin_nm, O_WRONLY | O_CREAT | O_TRUNC, from_perms);
        if (destin_fd < 0) {
            res = errno;
            if (res == EACCES)
                ++op->stats.num_reg_d_eacces;
            else if (res == EPERM)
                ++op->stats.num_reg_d_eperm;
            else if (res == EIO)
                ++op->stats.num_reg_d_eio;
            else
                ++op->stats.num_reg_d_e_other;
            goto fini;
        }
    }
    if (num > 0) {
        num2 = write(destin_fd, bp, num);
        if (num2 < 0) {
            res = errno;
            if (res == EACCES)
                ++op->stats.num_reg_d_eacces;
            else if (res == EPERM)
                ++op->stats.num_reg_d_eperm;
            else if (res == EIO)
                ++op->stats.num_reg_d_eio;
            else
                ++op->stats.num_reg_d_e_other;
            goto fini;
        }
        if ((num2 < num) && (verbose > 0))
            pr3ser(destin_nm, "short write(), strange");
    }

fini:
    if (from_fd >= 0)
        close(from_fd);
    if (destin_fd >= 0)
        close(destin_fd);
    if (bp != fix_b)
        free(bp);
    if (res == 0)
        ++op->stats.num_reg_success;
    return res;
}

static void
update_stats(const fs::file_type & sl_ftype, const fs::file_type & targ_ftype,
             bool hidden, const struct opts_t * op)
{
    if (hidden)
        ++op->stats.num_hidden;
    if (sl_ftype == fs::file_type::symlink) {
        if (targ_ftype == fs::file_type::directory)
            ++op->stats.num_sym2dir;
        else if (targ_ftype == fs::file_type::regular)
            ++op->stats.num_sym2reg;
        else if (targ_ftype == fs::file_type::block)
            ++op->stats.num_sym2block;
        else if (targ_ftype == fs::file_type::character)
            ++op->stats.num_sym2char;
        else if (targ_ftype == fs::file_type::none)
            ++op->stats.num_sym_hang;
        else
            ++op->stats.num_sym_other;
        return;
    }
    switch (targ_ftype) {
    case fs::file_type::directory:
        ++op->stats.num_dir;
        break;
    case fs::file_type::symlink:
        ++op->stats.num_sym_hang;
        break;
    case fs::file_type::regular:
        ++op->stats.num_regular;
        break;
    case fs::file_type::block:
        ++op->stats.num_block;
        break;
    case fs::file_type::character:
        ++op->stats.num_char;
        break;
    case fs::file_type::fifo:
        ++op->stats.num_fifo;
        break;
    case fs::file_type::socket:
        ++op->stats.num_socket;
        break;
    default:
        ++op->stats.num_other;
        break;
    }
}

static void
show_stats(const struct opts_t * op)
{
    struct stats_t * q = &op->stats;

    scout << "Number of regular files: " << q->num_regular << "\n";
    scout << "Number of directories: " << q->num_dir << "\n";
    scout << "Number of symlinks to directories: " << q->num_sym2dir << "\n";
    scout << "Number of symlinks to regular files: " << q->num_sym2reg
          << "\n";
    scout << "Number of symlinks to block device nodes: "
          << q->num_sym2block << "\n";
    scout << "Number of symlinks to char device nodes: "
          << q->num_sym2char << "\n";
    scout << "Number of symlinks to others: " << q->num_sym_other << "\n";
    scout << "Number of hanging symlinks: " << q->num_sym_hang
          << " [may be resolved later in scan]\n";
    scout << "Number of hidden files skipped: " << q->num_hidden_skipped
          << "\n";
    scout << "Number of block device nodes: " << q->num_block << "\n";
    scout << "Number of char device nodes: " <<  q->num_char << "\n";
    scout << "Number of fifo_s: " << q->num_fifo << "\n";
    scout << "Number of sockets: " << q->num_socket << "\n";
    scout << "Number of other file types: " << q->num_other << "\n";
    scout << "Number of filenames starting with '.': " << q->num_hidden
          << "\n";
    if (! op->no_destin) {
        scout << "Number of dst created directories: "
              << q->num_dir_d_success << "\n";
        scout << "Number of dst created symlinks: "
              << q->num_sym_d_success << "\n";
    }
    scout << "Number of files excluded: " << q->num_excluded << "\n";
    // N.B. recursive_directory_iterator::depth() is one less than expected
    scout << "Maximum depth of source scan: " << q->max_depth + 1 << "\n";
    scout << "Number of scan errors detected: " << q->num_error << "\n";
    if (q->num_reg_tries == 0)
        return;

    scout << "\n>> Following associated with clone/copy of regular files\n";
    scout << "Number of attempts to clone: " << q->num_reg_tries << "\n";
    scout << "Number of clone successes: " << q->num_reg_success << "\n";
    scout << "Number of source EACCES errors: " << q->num_reg_s_eacces
          << "\n";
    scout << "Number of source EPERM errors: " << q->num_reg_s_eperm << "\n";
    scout << "Number of source EIO errors: " << q->num_reg_s_eio << "\n";
    scout << "Number of source ENODATA errors: " << q->num_reg_s_enodata
          << "\n";
    scout << "Number of source ENOENT, ENODEV or ENXIO errors: "
          << q->num_reg_s_enoent_enodev_enxio << "\n";
    scout << "Number of source EAGAIN errors: " << q->num_reg_s_eagain
          << "\n";
    scout << "Number of source poll timeouts: " << q->num_reg_s_timeout
          << "\n";
    scout << "Number of source other errors: " << q->num_reg_s_e_other
          << "\n";
    scout << "Number of dst EACCES errors: " << q->num_reg_d_eacces << "\n";
    scout << "Number of dst EPERM errors: " << q->num_reg_d_eperm << "\n";
    scout << "Number of dst EIO errors: " << q->num_reg_d_eio << "\n";
    scout << "Number of dst other errors: " << q->num_reg_d_e_other << "\n";
    scout << "Number of files " << op->reglen << " bytes or longer: "
          << q->num_reg_s_at_reglen << "\n";
}

static std::error_code
do_clone(const struct opts_t * op)
{
    bool beyond_for = false;
    bool possible_exclude = ! op->exclude_v.empty();
    bool exclude_entry;
    int res {} ;

    struct stats_t * q = &op->stats;
    dev_t containing_fs_inst {} ;
    struct stat root_stat, oth_stat;
    std::error_code ec { };
    auto start = chron::steady_clock::now();

    if (stat(op->source_pt.c_str(), &root_stat) < 0) {
        ec.assign(errno, std::system_category());
        return ec;
    }
    containing_fs_inst = root_stat.st_dev;

    // All fs calls use std::error_code so nothing should throw ... g++
    fs::recursive_directory_iterator end_itr { };
    for (fs::recursive_directory_iterator itr(op->source_pt, dir_opt, ec);
         (! ec) && itr != end_itr;
         beyond_for = false, itr.increment(ec) ) {
        beyond_for = true;

        // since op->source_pt is in canonical form, assume entry.path()
        // will either be in canonical form, or absolute form if symlink
        fs::path pt { itr->path() };
        auto depth = itr.depth();

        if (verbose > 6)
            pr3ser(pt, "about to scan this source entry");
        fs::file_type sl_ftype = itr->symlink_status(ec).type();
        if (ec) {       // serious error
            ++q->num_error;
            if (verbose > 2)
                pr3ser(pt, "symlink_status() failed, continue", ec);
            // thought of using entry.refresh(ec) but no speed improvement
            continue;
        }
        if (depth > op->stats.max_depth)
            op->stats.max_depth = depth;
        fs::file_type targ_ftype = fs::file_type::none;

        if (fs::exists(*itr, ec)) {
            targ_ftype = itr->status(ec).type();
            if (ec) {
                if (verbose > 4)
                    pr3ser(pt, "status() failed, continue", ec);
                ++q->num_error;
            }
            // this conditional is a sanity check, may be overkill
            if ((sl_ftype == fs::file_type::symlink) &&
		(! path_contains_canon(op->source_pt, pt))) {
                pr4ser(op->source_pt, pt, "is not contained in source path?");
                break;
            }
        } else {
            // expect sl_ftype to be symlink, so dangling target
            if (verbose > 4)
                pr3ser(pt, "fs::exists() failed, continue", ec);
            if (ec)
                ++q->num_error;
        }

        bool me_hidden = ((! pt.empty()) &&
                          (pt.filename().string()[0] == '.'));
        fs::path sl_pt, c_sl_pt, rel_path;

        exclude_entry = false;
        if (possible_exclude) {
            exclude_entry = std::ranges::binary_search(op->exclude_v, pt);
            if (exclude_entry)
                ++op->stats.num_excluded;
            if (exclude_entry && (verbose > 3))
                pr3ser(pt, "matched for exclusion");
        }
        if (op->want_stats)
            update_stats(sl_ftype, targ_ftype, me_hidden, op);
        if (op->no_destin) {
            if ((sl_ftype == fs::file_type::directory) &&
                op->max_depth_active && (depth >= op->max_depth)) {
                if (verbose > 2)
                    scerr << "Source at max_depth: " << pt
                          << " [" << depth << "], don't enter\n";
                itr.disable_recursion_pending();
            }
            continue;
        }
        if ((! op->clone_hidden) && me_hidden) {
            ++op->stats.num_hidden_skipped;
            continue;
        }
        if ((targ_ftype != fs::file_type::none) &&
            (stat(pt.c_str(), &oth_stat) < 0)) {
            ec.assign(errno, std::system_category());
            pr3ser(pt, "stat() failed", ec);
            ++q->num_error;
            continue;
        }
        fs::path rel_pt { fs::proximate(pt, op->source_pt, ec) };
        if (ec) {
            if (verbose > 1)
                pr3ser(pt, "proximate() failed", ec);
            ++q->num_error;
            continue;
        }
        fs::path ongoing_destin_pt { op->destination_pt / rel_pt };

        switch (sl_ftype) {
        case fs::file_type::directory:
            if (! op->no_xdev) { // double negative ...
                if (oth_stat.st_dev != containing_fs_inst) {
                    // do not visit this sub-branch: different fs instance
                    if (verbose > 2)
                        scout << "Source trying to leave this fs instance "
                                 "at: " << pt << "\n";
                    itr.disable_recursion_pending();
                }
            }
            if (exclude_entry) {
                itr.disable_recursion_pending();
                continue;
            }
            if (op->max_depth_active && (depth >= op->max_depth)) {
                if (verbose > 2)
                    scout << "Source at max_depth and this is a directory: "
                          << pt << ", don't enter\n";
                itr.disable_recursion_pending();
            }
            if (! op->destin_all_new) { /* may already exist */
                if (fs::exists(ongoing_destin_pt, ec)) {
                    if (fs::is_directory(ongoing_destin_pt, ec)) {
                        ;       // good, nothing to do, stats ?
                    } else if (ec) {
                        pr3ser(ongoing_destin_pt, ":is_directory() failed",
                               ec);
                        ++q->num_error;
                    } else if (verbose > 0)
                        pr3ser(ongoing_destin_pt, "exists but not directory, "
                               "skip");
                    break;
                } else if (ec) {
                    pr3ser(ongoing_destin_pt, "exists() failed", ec);
                    ++q->num_error;
                    break;
                } else {
                    ;   // drop through to create_dir
                }
            }
            if (! fs::create_directory(ongoing_destin_pt, pt, ec)) {
                if (verbose > 1)
                    pr3ser(ongoing_destin_pt, "create_directory() failed",
                           ec);
                ++q->num_error;
                break;
            } else {
                ++q->num_dir_d_success;
                if (verbose > 4)
                    pr3ser(pt, "create_directory() ok");
            }
            break;
        case fs::file_type::symlink:
            {
                // what to do about exclude_entry==true ?
                fs::path target_pt = fs::read_symlink(pt, ec);
                if (ec) {
                    if (verbose > 1)
                        pr3ser(pt, "read_symlink() failed", ec);
                    ++q->num_error;
                    break;
                }
                fs::path pt_parent_pt { pt.parent_path() };
                fs::path prox_pt { op->destination_pt };
                if (pt_parent_pt != op->source_pt) {
                    prox_pt /= fs::proximate(pt_parent_pt, op->source_pt, ec);
                    if (ec) {
                        pr3ser(pt_parent_pt, "symlink: proximate() failed",
                               ec);
                        ++q->num_error;
                        break;
                    }
                }
                fs::path lnk_pt { prox_pt / pt.filename() };
                if (! op->destin_all_new) {     /* may already exist */
                    fs::file_type lnk_ftype =
                                fs::symlink_status(lnk_pt, ec).type();

                    if (ec) {
                        int v = ec.value();

                        // ENOENT can happen with "dynamic" sysfs
                        if ((v == ENOENT) || (v == ENODEV) || (v == ENXIO)) {
                            ++op->stats.num_reg_s_enoent_enodev_enxio;
                            if (verbose > 4)
                                pr3ser(lnk_pt, "symlink_status() failed", ec);
                        } else {
                            ++q->num_error;
                            if (verbose > 2)
                                pr3ser(lnk_pt, "symlink_status() failed", ec);
                        }
                        break;
                    }
                    if (lnk_ftype == fs::file_type::symlink)
                        break;
                    else if (lnk_ftype == fs::file_type::not_found) {
                        ;       // drop through
                    } else {
                        pr3ser(lnk_pt, "unexpected lnk_ftype", ec);
                        ++q->num_error;
                        break;
                    }
                }

                fs::create_symlink(target_pt, lnk_pt, ec);
                if (ec) {
                    if (verbose > 0)
                        pr4ser(target_pt, lnk_pt, "create_symlink() failed",
                               ec);
                    ++q->num_error;
                } else {
                    ++q->num_sym_d_success;
                    if (verbose > 4)
                        pr4ser(target_pt, lnk_pt, "create_symlink() ok");
                }
            }
            break;
        case fs::file_type::regular:
            if (exclude_entry)
                continue;
            res = xfer_regular_file(pt, ongoing_destin_pt, op);
            if (res) {
                ec.assign(res, std::system_category());
                if (verbose > 3) {
                    pr4ser(pt, ongoing_destin_pt, "xfer_regular_file() "
                           "failed", ec);
                    ++q->num_error;
                }
            } else if (verbose > 5)
                pr4ser(pt, ongoing_destin_pt, "xfer_regular_file() ok");
            break;
        case fs::file_type::block:
        case fs::file_type::character:
            if (exclude_entry)
                continue;
            if (mknod(ongoing_destin_pt.c_str(), oth_stat.st_mode,
                      oth_stat.st_dev) < 0) {
                res = errno;
                ec.assign(res, std::system_category());
                if (verbose > 3) {
                    pr4ser(pt, ongoing_destin_pt, "mknod() failed", ec);
                    ++q->num_error;
                }
            } else if (verbose > 5)
                pr4ser(pt, ongoing_destin_pt, "mknod() ok");
            break;
        case fs::file_type::fifo:
        case fs::file_type::socket:
            break;              // skip these file types
        default:                // here when something no longer exists
            switch (sl_ftype) {
            case fs::file_type::directory:
                if (op->max_depth_active && (depth >= op->max_depth)) {
                    if (verbose > 2)
                        scerr << "Source at max_depth: " << pt
                              << " [" << depth << "], don't enter\n";
                    itr.disable_recursion_pending();
                }
                break;
            case fs::file_type::symlink:
                if (verbose > 2)
                    pr4ser(pt, "switch in switch symlink, skip");
                break;
            case fs::file_type::regular:
                if (verbose > 2)
                    pr4ser(pt, "switch in switch regular file, skip");
                break;
            default:
                if (verbose > 2)
                    scerr << pt << ", switch in switch sl_ftype: "
                          << static_cast<int>(sl_ftype) << "\n";

                break;
            }
            break;
        }
    }
    if (ec) {
        if (beyond_for) {
            if (verbose > 4)
                pr3ser(op->source_pt, "problem already reported", ec);
        } else {
            ++q->num_error;
            pr3ser(op->source_pt, "recursive_directory_iterator() failed", ec);
        }
    }

    auto end = chron::steady_clock::now();
    auto ms = chron::duration_cast<chron::milliseconds>(end - start).count();
    auto secs = ms / 1000;
    auto ms_remainder = ms % 1000;
    char b[32];
    snprintf(b, sizeof(b), "%d.%03d", static_cast<int>(secs),
             static_cast<int>(ms_remainder));
    std:: cout << "Elapsed time: " << b << " seconds\n";

    if (op->want_stats)
        show_stats(op);
    return ec;
}


int
main(int argc, char * argv[])
{
    bool ex_glob_seen = false;
    int c, res, glob_opt;
    std::error_code ec { };
    const char * destination_clone_start = NULL;
    const char * source_clone_start = NULL;
    glob_t ex_paths { };
    struct opts_t opts { };
    struct opts_t * op = &opts;

    op->reglen = def_reglen;
    while (1) {
        int option_index = 0;

        c = getopt_long(argc, argv, "d:De:hHm:Nr:s:SvVw:", long_options,
                        &option_index);
        if (c == -1)
            break;

        switch (c) {
        case 'd':
            destination_clone_start = optarg;
            op->destination_given = true;
            break;
        case 'D':
            op->no_destin = true;
            break;
        case 'e':
            if (ex_glob_seen)
                glob_opt = GLOB_APPEND;
            else {
                ex_glob_seen = true;
                glob_opt = 0;
            }
            res = glob(optarg, glob_opt, NULL, &ex_paths);
            if (res != 0) {
                if ((res == GLOB_NOMATCH) && (verbose > 0))
                    scerr << "Warning: --exclude=" << optarg
                          << " did not match any file, continue\n";
                else {
                    globfree(&ex_paths);
                    scerr << "glob() failed with --exclude=" << optarg
                          << " , exit\n";
                    return 1;
                }
            }
            break;
        case 'h':
            usage();
            return 0;
        case 'H':
            op->clone_hidden = true;
            break;
        case 'm':
            if (1 != sscanf(optarg, "%d", &op->max_depth)) {
                pr2ser("unable to decode integer for --max-depth=MAXD");
                return 1;
            }
            if (op->max_depth > 0) {
                --op->max_depth;
                op->max_depth_active = true;
            }
            break;
        case 'N':
            op->no_xdev = true;
            break;
        case 'r':
            if (1 != sscanf(optarg, "%u", &op->reglen)) {
                pr2ser("unable to decode integer for --reglen=RLEN");
                return 1;
            }
            break;
        case 's':
            source_clone_start = optarg;
            op->source_given = true;
            break;
        case 'S':
            op->want_stats = true;
            break;
        case 'v':
            ++verbose;
            break;
        case 'V':
            scout << version_str << "\n";
            return 0;
        case 'w':
            if (1 != sscanf(optarg, "%u", &op->wait_ms)) {
                pr2ser("unable to decode integer for --reglen=RLEN");
                return 1;
            }
            op->wait_given = true;
            break;
        default:
            scerr << "unrecognised option code 0x" << c << "\n";
            usage();
            return 1;
        }
    }
    if (optind < argc) {
#if 0
        if (NULL == device_name) {
            device_name = argv[optind];
            ++optind;
        }
#endif
        if (optind < argc) {
            for (; optind < argc; ++optind)
                scerr << "Unexpected extra argument: " << argv[optind]
                          << "\n";
            usage();
            return 1;
        }
    }
    if (op->source_given) {
        fs::path pt { source_clone_start };

        if (fs::exists(pt, ec) && fs::is_directory(pt, ec)) {
            op->source_pt = fs::canonical(pt, ec);
            if (ec) {
                pr3ser(pt, "canonical() failed", ec);
                return 1;
            }
        } else if (ec) {
            pr3ser(pt, "exists() or is_directory() failed", ec);
        } else {
            pr3ser(source_clone_start, "doesn't exist, is not a directory, "
                   "or ...", ec);
            return 1;
        }
    } else
        op->source_pt = sysfs_root;

    if (! op->no_destin) {
        sstring d_str;

        if (op->destination_given)
            d_str = destination_clone_start;
        else if (op->source_given) {
            pr2ser("When --source= given, need also to give "
                   "--destination= (or --no-dst)");
            return 1;
        } else
            d_str = def_destin_root;
        if (d_str.size() == 0) {
            pr2ser("Confused, what is destination? [Got empty string]");
            return 1;
        }
        fs::path pt { d_str };

        if (pt.filename().empty())
            pt = pt.parent_path(); // to handle trailing / as in /tmp/sys/
        if (fs::exists(pt, ec)) {
            if (fs::is_directory(pt, ec)) {
                op->destination_pt = fs::canonical(pt, ec);
                if (ec) {
                    pr3ser(pt, "canonical() failed", ec);
                    return 1;
                }
            } else {
                pr3ser(pt, "is not a directory", ec);
                return 1;
            }
        } else {
            fs::path p_pt { pt.parent_path() };

            if (fs::exists(p_pt, ec) && fs::is_directory(p_pt, ec)) {
                fs::create_directory(pt, ec);
                if (ec) {
                    pr3ser(pt, "create_directory() failed", ec);
                    return 1;
                }
                op->destination_pt = fs::canonical(pt, ec);
                op->destin_all_new = true;
                if (ec) {
                    pr3ser(pt, "canonical() failed", ec);
                    return 1;
                }
            } else {
                pr3ser(p_pt, "needs to be an existing directory", ec);
                return 1;
            }
        }
        if (op->source_pt == op->destination_pt) {
            pr4ser(op->source_pt, op->destination_pt,
                   "source and destination seem to be the same. That is not "
                   "practical");
            return 1;
        }
    } else if (op->destination_given) {
        pr2ser("the --destination= and the --no-dst options contradict, "
               "pick one");
        return 1;
    }

    auto ex_sz = op->exclude_v.size();  // will be zero here
    bool destin_excluded = false;

    if (ex_glob_seen) {
        for(size_t k { }; k < ex_paths.gl_pathc; ++k) {
            fs::path ex_pt { ex_paths.gl_pathv[k] };
            fs::path c_ex_pt { fs::canonical(ex_pt, ec) };

            if (ec) {
                if (verbose > 1)
                    pr3ser(ex_pt, "exclude path rejected", ec);
            } else {
                if (path_contains_canon(op->source_pt, c_ex_pt)) {
                    op->exclude_v.push_back(c_ex_pt);
                    if (verbose > 3)
                        pr3ser(c_ex_pt, "accepted canonical exclude path");
                    if (c_ex_pt == op->destination_pt)
                        destin_excluded = true;
                } else
                    pr3ser(ex_pt, "ignored as not contained in source");
            }
        }
        globfree(&ex_paths);
        ex_sz = op->exclude_v.size();

        if (verbose > 0)
            scerr << "--exclude= argument matched " << ex_sz << " files\n";
        if (ex_sz > 1) {
            if (! std::ranges::is_sorted(op->exclude_v)) {
                if (verbose > 0)
                    pr2ser("need to sort exclude vector");
                std::ranges::sort(op->exclude_v);
            }
            const auto ret = std::ranges::unique(op->exclude_v);
            op->exclude_v.erase(ret.begin(), ret.end());
            ex_sz = op->exclude_v.size();       // could be less after erase
            if (verbose > 0)
                scerr << "exclude vector size after sort then unique="
                      << ex_sz << "\n";
        }
    }
    if (! op->no_destin) {
        if (path_contains_canon(op->source_pt, op->destination_pt)) {
            pr2ser("Source contains destination, infinite recursion "
                   "possible");
            if ((op->max_depth == 0) && (ex_sz == 0)) {
                pr2ser("exit, due to no --max-depth= and no --exclude=");
                return 1;
            } else if (! destin_excluded)
                pr2ser("Probably best to --exclude= destination, will "
                       "continue");
        } else {
            if (verbose > 0)
                pr2ser("Source does NOT contain destination (good)");
            if (path_contains_canon(op->destination_pt, op->source_pt)) {
                pr2ser("Strange: destination contains source, is infinite "
                       "recursion possible ?");
                if (verbose > 2)
                    pr2ser("destination does NOT contain source (also good)");
            }
        }
    }

    ec = do_clone(op);
    if (ec)
        pr2ser("do_clone() failed");
    else if ((op->want_stats == false) && (op->destination_given == false) &&
             (op->source_given == false) && (op->no_destin == false))
        scout << "Successfully cloned " << sysfs_root << " to "
              << def_destin_root << "\n";

    return 0;
}
