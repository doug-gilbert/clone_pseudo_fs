/*
 * Copyright (c) 2023 Douglas Gilbert.
 * All rights reserved.
 * Use of this source code is governed by a BSD-style
 * license that can be found in the BSD_LICENSE file.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/* This is a utility program for listing USB-C Power Delivery ports
 * and partners in Linux. It performs data-mining in the sysfs file
 * system assumed to be mounted under /sys .
 *
 */


// Initially this utility will assume C++20 or later

static const char * const version_str = "0.90 20230617";

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

#include <unistd.h>
#include <getopt.h>
#include <fcntl.h>
#include <glob.h>
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
    unsigned int num_error;
    // above calculated during source scan
    // below calculated during transfer of regular files
    unsigned int num_reg_tries;       // only incremented when dst active
    unsigned int num_reg_success;
    unsigned int num_reg_s_at_reglen;
    unsigned int num_reg_s_eacces;
    unsigned int num_reg_s_eperm;
    unsigned int num_reg_s_eio;
    unsigned int num_reg_s_enodata;
    unsigned int num_reg_s_enoent_enodev;
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
    bool no_destin;
    bool clone_hidden;
    bool no_xdev;
    bool source_given;
    bool want_stats;
    unsigned int reglen;
    int max_depth;     // 0 means no limit
    int verbose;
    mutable struct stats_t stats;
    fs::path source_pt;
    fs::path destination_pt;
    std::vector<sstring> exclude_v;
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
    "    --statistics|-S    gather then output statistics\n"
    "    --verbose|-v       increase verbosity\n"
    "    --version|-V       output version string and exit\n";

static void
usage()
{
    scout << usage_message1;
}

static void
pr2ser(const sstring & emsg, const std::error_code & ec = { },
       const std::source_location loc = std::source_location::current())
{
    if (emsg.size() == 0)       /* shouldn't need location.column() */
        std::cerr << loc.file_name() << " " << loc.function_name()
                  << ";ln=" << loc.line() << "\n";
    else if (ec)
        std::cerr << loc.function_name() << ";ln=" << loc.line()
                  << ": " << emsg << ", error: " << ec.message() << "\n";
    else
        std::cerr << loc.function_name() << ";ln=" << loc.line() <<  ": "
                  << emsg << "\n";
}

static void
pr3ser(const sstring & e1msg, const char * e2msg = NULL,
       const std::error_code & ec = { },
       const std::source_location loc = std::source_location::current())
{
    if (e2msg == nullptr)
        pr2ser(e1msg, ec, loc);
    else if (ec)
        std::cerr << loc.function_name() << ";ln=" << loc.line() << ": '"
                  << e1msg << "': " << e2msg << ", error: " << ec.message()
                  << "\n";
    else
        std::cerr << loc.function_name() << ";ln=" << loc.line() << ": '"
                  << e1msg << "': " << e2msg << "\n";
}

static void
pr4ser(const sstring & e1msg, const sstring & e2msg,
       const char * e3msg = NULL, const std::error_code & ec = { },
       const std::source_location loc = std::source_location::current())
{
    if (e3msg == nullptr)
        pr3ser(e1msg, e2msg.c_str(), ec, loc);
    else if (ec)
        std::cerr << loc.function_name() << ";ln=" << loc.line() << ": '"
                  << e1msg << "," << e2msg << "': " << e3msg << ", error: "
                  << ec.message() << "\n";
    else
        std::cerr << loc.function_name() << ";ln=" << loc.line() << ": '"
                  << e1msg << "," << e2msg << "': " << e3msg << "\n";
}

static int
xfer_regular_file(const sstring & from_file, const sstring & destin_file,
                  const struct opts_t * op)
{
    int res = 0;
    int from_fd = -1;
    int destin_fd = -1;
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

    from_fd = open(from_nm, O_RDONLY);
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
                else if ((res == ENOENT) || (res == ENODEV))
                    ++op->stats.num_reg_s_enoent_enodev;
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
        else if ((res == ENOENT) || (res == ENODEV))
            ++op->stats.num_reg_s_enoent_enodev;
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
    num = read(from_fd, bp, op->reglen);
    if (num < 0) {
        res = errno;
        if (res == EACCES)
            ++op->stats.num_reg_s_eacces;
        else if (res == EPERM)
            ++op->stats.num_reg_s_eperm;
        else if (res == EIO)
            ++op->stats.num_reg_s_eio;
        else if (res == ENODATA)
            ++op->stats.num_reg_s_enodata;
        else if ((res == ENOENT) || (res == ENODEV))
            ++op->stats.num_reg_s_enoent_enodev;
        else
            ++op->stats.num_reg_s_e_other;
        num = 0;
        close(from_fd);
        goto do_destin;
    }
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
        if ((num2 < num) && (op->verbose > 0))
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
    scout << "Number of symlinks to others: " << q->num_sym_other << "\n";
    scout << "Number of hidden files skipped: " << q->num_hidden_skipped
          << "\n";
    scout << "Number of broken symlinks: " << q->num_sym_hang << "\n";
    scout << "Number of block device nodes: " << q->num_block << "\n";
    scout << "Number of char device nodes: " <<  q->num_char << "\n";
    scout << "Number of fifo_s: " << q->num_fifo << "\n";
    scout << "Number of sockets: " << q->num_socket << "\n";
    scout << "Number of other file types: " << q->num_other << "\n";
    scout << "Number of filenames starting with '.': " << q->num_hidden
          << "\n";
    scout << "Maximum depth of source scan: " << q->max_depth << "\n";
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
    scout << "Number of source ENOENT or ENODEV errors: "
          << q->num_reg_s_enoent_enodev << "\n";
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
         beyond_for = false, itr.increment(ec) /* avoid exceptions ... */ ) {

        beyond_for = true;
        const auto& entry = *itr;

        if (op->verbose > 6)
            pr3ser(entry.path(), "about to scan this source entry");
        fs::file_type sl_ftype = entry.symlink_status(ec).type();

        if (ec) {       // serious error
            ++q->num_error;
            if (op->verbose > 2)
                pr3ser(entry.path(), "symlink_status() failed, continue", ec);
            continue;
        }
        auto depth = itr.depth();
        if (depth > op->stats.max_depth)
            op->stats.max_depth = depth;
        fs::file_type targ_ftype = fs::file_type::none;

        if (fs::exists(entry, ec)) {
            targ_ftype = entry.status(ec).type();
            if (ec) {
                if (op->verbose > 4)
                    pr3ser(entry.path(), "status() failed, continue", ec);
                ++q->num_error;
            }
        } else {
            // expect sl_ftype to be symlink, so dangling target
            if (op->verbose > 4)
                pr3ser(entry.path(), "fs::exists() failed, continue", ec);
            if (ec)
                ++q->num_error;
        }

        fs::path pt { entry.path() };
        sstring fn { pt.filename() };
        bool me_hidden = ((fn.size() > 0) && (fn[0] == '.'));
        fs::path sl_pt, c_sl_pt, rel_path;

        if (op->want_stats)
            update_stats(sl_ftype, targ_ftype, me_hidden, op);
        if (op->no_destin)
            continue;
#if 0
        if (targ_ftype == fs::file_type::none)       // clone broken symlink ?
            continue;
#endif
        if ((! op->clone_hidden) && me_hidden) {
            ++op->stats.num_hidden_skipped;
            continue;
        }
        if ((targ_ftype != fs::file_type::none) &&
            (stat(pt.c_str(), &oth_stat) < 0)) {
            ec.assign(errno, std::system_category());
            pr3ser(pt, "stat() failed", ec);
            ++q->num_error;
            break;
        }
        fs::path rel_pt { fs::proximate(pt, op->source_pt, ec) };
        if (ec) {
            pr3ser(pt, "proximate() failed", ec);
            ++q->num_error;
            break;
        }
        fs::path ongoing_destin_pt { op->destination_pt / rel_pt };

        switch (sl_ftype) {
        case fs::file_type::directory:
            if (! op->no_xdev) { // double negative ...
                if (oth_stat.st_dev != containing_fs_inst) {
                    // do not visit this sub-branch: different fs instance
                    if (op->verbose > 2)
                        std::cout << "Source trying to leave this fs "
                                     "instance at: " << pt << "\n";
                    itr.disable_recursion_pending();
                }
            }
            if ((op->max_depth > 0) && (depth >= op->max_depth)) {
                if (op->verbose > 2)
                    std::cout << "Source at max_depth and this is "
                                 "a directory: " << pt << ", don't enter\n";
                itr.disable_recursion_pending();
            }
            if (! op->destin_all_new) { /* may already exist */
                if (fs::exists(ongoing_destin_pt, ec)) {
                    if (fs::is_directory(ongoing_destin_pt, ec)) {
                        ;       // good, nothing to do, stats ?
                    } else if (ec) {
                        pr3ser(ongoing_destin_pt, "fs::is_directory() failed",
                               ec);
                        ++q->num_error;
                    } else if (op->verbose > 0)
                        pr3ser(ongoing_destin_pt, "exists but not directory, "
                               "skip");
                    break;
                } else if (ec) {
                    pr3ser(ongoing_destin_pt, "fs::exists() failed", ec);
                    ++q->num_error;
                    break;
                } else {
                    ;   // drop through to create_dir
                }
            }
            if (! fs::create_directory(ongoing_destin_pt, pt, ec)) {
		if (op->verbose > 1)
                    pr3ser(ongoing_destin_pt, "fs::create_directory() failed",
                           ec);
                ++q->num_error;
                break;
            } else if (op->verbose > 4)
                pr3ser(pt, "fs::create_directory() ok");
            break;
        case fs::file_type::symlink:
            {
                fs::path target_pt = fs::read_symlink(pt, ec);
                if (ec) {
                    pr3ser(pt, "fs::read_symlink() failed", ec);
                    ++q->num_error;
                    break;
                }
                fs::path pt_parent_pt { pt.parent_path() };
                fs::path prox_pt { op->destination_pt };
                if (pt_parent_pt != op->source_pt) {
                    prox_pt /= fs::proximate(pt_parent_pt, op->source_pt, ec);
                    if (ec) {
                        pr3ser(pt_parent_pt, "symlink: fs::proximate() "
                               "failed", ec);
                        ++q->num_error;
                        break;
                    }
                }
                fs::path lnk_pt { prox_pt / pt.filename() };
                if (! op->destin_all_new) {     /* may already exist */
                    fs::file_type lnk_ftype =
                                fs::symlink_status(lnk_pt, ec).type();

                    if (ec) {
                        // ENOENT can happen with "dynamic" sysfs
                        if ((ec.value() == ENOENT) || (ec.value() == ENODEV))
                            ++op->stats.num_reg_s_enoent_enodev;
                        if (op->verbose > 1) {
                            pr3ser(lnk_pt, "fs::symlink_status() failed", ec);
                            ++q->num_error;
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
                    pr4ser(target_pt, lnk_pt, "create_symlink() failed", ec);
                    ++q->num_error;
                } else if (op->verbose > 4)
                    pr4ser(target_pt, lnk_pt, "create_symlink() ok");
            }
            break;
        case fs::file_type::regular:
            res = xfer_regular_file(pt, ongoing_destin_pt, op);
            if (res) {
                ec.assign(res, std::system_category());
                if (op->verbose > 3) {
                    pr4ser(pt, ongoing_destin_pt, "xfer_regular_file() "
                           "failed", ec);
                    ++q->num_error;
                }
            } else if (op->verbose > 5)
                pr4ser(pt, ongoing_destin_pt, "xfer_regular_file() ok");
            break;
        case fs::file_type::block:
        case fs::file_type::character:
            if (mknod(ongoing_destin_pt.c_str(), oth_stat.st_mode,
                      oth_stat.st_dev) < 0) {
                res = errno;
                ec.assign(res, std::system_category());
                if (op->verbose > 3) {
                    pr4ser(pt, ongoing_destin_pt, "mknod() failed", ec);
                    ++q->num_error;
                }
            } else if (op->verbose > 5)
                pr4ser(pt, ongoing_destin_pt, "mknod() ok");
            break;
        default:
            break;
        }
    }
    if (ec) {
        if (beyond_for) {
            if (op->verbose > 4)
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
    snprintf(b, sizeof(b), "%d.%03d", (int)secs, (int)ms_remainder);
    std:: cout << "Elapsed time: " << b << " seconds\n";

    if (op->want_stats)
        show_stats(op);
    return ec;
}


int
main(int argc, char * argv[])
{
    std::error_code ec { };
    const char * device_name = NULL;
    const char * destination_clone_start = NULL;
    const char * exclude_pattern = NULL;
    const char * source_clone_start = NULL;
    int c;
    struct opts_t opts { };
    struct opts_t * op = &opts;

    op->reglen = def_reglen;
    while (1) {
        int option_index = 0;

        c = getopt_long(argc, argv, "d:De:hHm:Nr:s:SvV", long_options,
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
            if (exclude_pattern) {
                pr2ser("--exclude=PAT can only be given once, can use "
                       "'{fn1,fn2, ... }' syntax instead\n");
                return 1;
            }
            exclude_pattern = optarg;
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
            ++op->verbose;
            break;
        case 'V':
            std::cout << version_str << "\n";
            return 0;
        default:
            std::cerr << "unrecognised option code 0x" << c << "\n";
            usage();
            return 1;
        }
    }
    if (optind < argc) {
        if (NULL == device_name) {
            device_name = argv[optind];
            ++optind;
        }
        if (optind < argc) {
            for (; optind < argc; ++optind)
                std::cerr << "Unexpected extra argument: " << argv[optind]
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
                pr3ser(pt, "fs::canonical() failed", ec);
                return 1;
            }
        } else if (ec) {
            pr3ser(pt, "fs::exists() or fs::is_directory() failed", ec);
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
        else
            d_str = def_destin_root;
        if (d_str.size() == 0) {
            pr2ser("Confused, what is destination? [Got empty string]");
            return 1;
        }
        fs::path pt { d_str };

        if (fs::exists(pt, ec)) {
            if (fs::is_directory(pt, ec)) {
                op->destination_pt = fs::canonical(pt, ec);
                if (ec) {
                    pr3ser(pt, "fs::canonical() failed", ec);
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
                    pr3ser(pt, "fs::create_directory() failed", ec);
                    return 1;
                }
                op->destination_pt = fs::canonical(pt, ec);
                op->destin_all_new = true;
                if (ec) {
                    pr3ser(pt, "fs::canonical() failed", ec);
                    return 1;
                }
            } else {
                pr3ser(p_pt, "needs to be an existing directory", ec);
                return 1;
            }
        }
    }
    if (exclude_pattern) {
        glob_t paths { } ;
        int res = glob(exclude_pattern, 0, NULL, &paths);

        if (res == 0) {
            for(size_t k { }; k < paths.gl_pathc; ++k )
                op->exclude_v.push_back(paths.gl_pathv[k]);
            globfree(&paths);
            if (op->verbose > 0)
                scerr << "--exclude= argument matched "
		      << op->exclude_v.size() << " files\n";
        } else if (res == GLOB_NOMATCH)
            pr2ser("Warning: --exclude= argument did not match any "
	           "file, continue");
	else {
            pr2ser("glob() failed with --exclude= argumen, exit");
	    return 1;
	}
    }

    ec = do_clone(op);
    if (ec)
        pr2ser("do_clone() failed");

    return 0;
}
