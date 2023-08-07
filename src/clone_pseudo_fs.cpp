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

static const char * const version_str = "0.95 20230807 [svn: r8]";

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

// "Base" class object of type inmem_t physically contains one of the 6
// "derived" class objects of type inmem_*_t using a std::variant named
// inmem_t::derived . C++ (virtual or plain) inheritance is _not_ used.
struct inmem_t;
struct inmem_subdirs_t;
struct inmem_dir_t;
struct opts_t;

static auto & scout { std::cout };
static auto & scerr { std::cerr };
static fs::path prev_rdi_pt;

static int verbose;  // don't want to pass 'struct opts_t' just to get this

// This template function does the dirty work of an upcast (i.e. Der -> B)
// including using offsetof() to cope if the inmem_t::derived field (i.e.
// the variant object) is not the first object in inmem_t .
template <typename B, typename Der>
    const B * get_t_base_cp(const Der * dp);

template <typename B, typename Der>
    B * get_t_base_p(Der * dp);

struct short_stat {     // stripped down version of 'struct stat'
    dev_t   st_dev;     // ID of device containing file
    mode_t  st_mode;    // File type and mode
};

struct inmem_other_t {
    inmem_other_t() = default;

    // no data members

    struct inmem_t * get_basep() { return reinterpret_cast<inmem_t *>(this); }
    const struct inmem_t * get_basep() const
                 { return reinterpret_cast<const inmem_t *>(this); }
    void debug(const sstring intro = "") const;
};

struct inmem_symlink_t {
    inmem_symlink_t() = default;

    fs::path target;

    const struct inmem_t * get_basep() const
                { return get_t_base_cp<inmem_t, inmem_symlink_t>(this); }
    struct inmem_t * get_basep()
                { return get_t_base_p<inmem_t, inmem_symlink_t>(this); }
    void debug(const sstring intro = "") const;
};

struct inmem_device_t { // block of char
    bool is_block_dev { false };

    dev_t st_rdev { };

    const struct inmem_t * get_basep() const
                { return get_t_base_cp<inmem_t, inmem_device_t>(this); }
    struct inmem_t * get_basep()
                { return get_t_base_p<inmem_t, inmem_device_t>(this); }
    void debug(const sstring intro = "") const;
};

struct inmem_fifo_socket_t {
    // no data members

    const struct inmem_t * get_basep() const
                { return get_t_base_cp<inmem_t, inmem_fifo_socket_t>(this); }
    struct inmem_t * get_basep()
                { return get_t_base_p<inmem_t, inmem_fifo_socket_t>(this); }
    void debug(const sstring intro = "") const;
};

struct inmem_regular_t {
    inmem_regular_t() = default;

    inmem_regular_t(const inmem_regular_t & oth) :
        contents(oth.contents), read_found_nothing(oth.read_found_nothing),
        always_use_contents(oth.always_use_contents)
        { }

    inmem_regular_t(inmem_regular_t && oth) :
        contents(oth.contents), read_found_nothing(oth.read_found_nothing),
        always_use_contents(oth.always_use_contents)
        { }

    std::vector<uint8_t> contents { };

    bool read_found_nothing { };

    bool always_use_contents { };  // set when symlink_src_tgt file inserted

    const struct inmem_t * get_basep() const
                { return get_t_base_cp<inmem_t, inmem_regular_t>(this); }
    struct inmem_t * get_basep()
                { return get_t_base_p<inmem_t, inmem_regular_t>(this); }
    void debug(const sstring intro = "") const;
};

struct inmem_subdirs_t {
    // vector of a directory's contents including subdirectories
    std::vector<inmem_t> sdir_v;

    // Using pointers for parent directories is dangerous due to std::vector
    // re-allocating when it expands. Backing up 1 level (depth) seems safe,
    // so when backing up two or more levels, instead use the new path to
    // navigate down from the root. The following map speeds directory name
    // lookup for for that latter case.
    std::map<sstring, size_t> sdir_fn_ind_m;  // sdir filename to index

    void debug(const sstring intro = "") const;
};

struct inmem_dir_t {
    inmem_dir_t();   // cannot use designated initializer if given

    inmem_dir_t(const inmem_dir_t & oth) :
                sdirs_sp(oth.sdirs_sp),
                par_pt_s(oth.par_pt_s), depth(oth.depth) { }
    inmem_dir_t(inmem_dir_t && oth) : sdirs_sp(oth.sdirs_sp),
                                      par_pt_s(oth.par_pt_s),
                                      depth(oth.depth) { }

    ~inmem_dir_t() { }

    std::shared_ptr<inmem_subdirs_t> sdirs_sp;

    // directory absolute path: par_pt_s + '/' + get_basep()->filename
    sstring par_pt_s { };

    int depth { -3 };   // purposely invalid. SPATH root has depth=-1

    // these add_to_sdir_v() functions all return the index position in
    // the sdirs_sp->sdir_v[] vector into which an inmem_t object was
    // inserted. The inmem_t object is formed from the passed arguments.
    // The first argument goes into inmem_t::derived which is a
    // std::variant .
    size_t add_to_sdir_v(inmem_other_t & n_oth, const sstring & fn,
                         const short_stat & a_shstat);
    size_t add_to_sdir_v(inmem_dir_t & n_dir, const sstring & fn,
                        const short_stat & a_shstat);
    size_t add_to_sdir_v(inmem_symlink_t & n_sym, const sstring & fn,
                       const short_stat & a_shstat);
    size_t add_to_sdir_v(inmem_device_t & n_dev, const sstring & fn,
                         const short_stat & a_shstat);
    size_t add_to_sdir_v(inmem_fifo_socket_t & n_fs, const sstring & fn,
                         const short_stat & a_shstat);
    size_t add_to_sdir_v(inmem_regular_t & n_reg, const sstring & fn,
                         const short_stat & a_shstat);

    const struct inmem_t * get_basep() const
                { return get_t_base_cp<inmem_t, inmem_dir_t>(this); }
    struct inmem_t * get_basep()
                { return get_t_base_p<inmem_t, inmem_dir_t>(this); }
    void debug(const sstring intro = "") const;
};


using inm_var_t=std::variant<inmem_other_t,
                             inmem_dir_t,
                             inmem_symlink_t,
                             inmem_device_t,
                             inmem_fifo_socket_t,
                             inmem_regular_t>;


struct inmem_t {        // <<< objects of this struct stored in a in-memory
                        // <<< tree that uses lots of std::vector<inmem_t>
    inmem_t() = default;

    inmem_t(const inm_var_t & der, const sstring & fn,
            const short_stat & a_shstat, size_t a_par_dir_ind)
                : derived(der), filename(fn), shstat(a_shstat),
                  par_dir_ind(a_par_dir_ind) { }

    inmem_t(const inmem_t & oth) : derived(oth.derived),
                                   filename(oth.filename),
                                   shstat(oth.shstat),
                                   par_dir_ind(oth.par_dir_ind) { }

    inmem_t(inmem_t && oth) : derived(oth.derived),
                                   filename(oth.filename),
                                   shstat(oth.shstat),
                                   par_dir_ind(oth.par_dir_ind) { }

    // Does the variant holding one of the variants need to be first ?
    inm_var_t derived;

    sstring filename { };  // link name if symlink

    struct short_stat shstat { };

    // This value is this object's index in its parent's
    // inmem_dir_t::sdirs_sp->sdir_v vector. Holding parent pointers is
    // dangerous as the parent itself is a vector member which can be
    // relocated when a new parent sibling is added. Usually when adding
    // new nodes we already know the parent's address. We assume the
    // following: when adding a node at depth <d>, no vectors holding
    // lesser depths (i.e. depths [0 ... (<d> - 1)] ) are modified.
    size_t par_dir_ind { };

    void debug(const sstring intro = "") const;

    void debug_base(const sstring intro = "") const;
};

enum class inmem_var_e {
    var_other = 0,      // matching inmem_t::derived.index()
    var_dir,
    var_symlink,
    var_device,
    var_fifo_socket,
    var_regular,
};

static const size_t derived_offset = offsetof(inmem_t, derived);

template <typename B, typename Der>
    const B * get_t_base_cp(const Der * dp) {
        if constexpr (derived_offset == 0) {
            return reinterpret_cast<const B * >(dp);
        } else { const uint8_t * u8p = reinterpret_cast<const uint8_t *>(dp);
            return reinterpret_cast<const B * >(u8p - derived_offset);
        }
    }

template <typename B, typename Der>
    B * get_t_base_p(Der * dp) {
        if constexpr (derived_offset == 0) {
            return reinterpret_cast<B * >(dp);
        } else {
            uint8_t * u8p = reinterpret_cast<uint8_t *>(dp);
            return reinterpret_cast<B * >(u8p - derived_offset);
        }
    }

struct stats_t {
    unsigned int num_node;      /* should be all valid file types */
    unsigned int num_dir;       /* directories that are not symlinks */
    unsigned int num_sym2dir;
    unsigned int num_sym2reg;
    unsigned int num_sym2block;
    unsigned int num_sym2char;
    unsigned int num_sym_other;
    unsigned int num_symlink;
    unsigned int num_sym_s_dangle;
    unsigned int num_oth_fs_skipped;
    unsigned int num_hidden_skipped;
    unsigned int num_regular;
    unsigned int num_block;
    unsigned int num_char;
    unsigned int num_fifo;
    unsigned int num_socket;
    unsigned int num_other;
    unsigned int num_hidden;
    unsigned int num_excluded;
    unsigned int num_derefed;
    unsigned int num_dir_d_success;
    unsigned int num_dir_d_exists;
    unsigned int num_sym_d_success;
    unsigned int num_sym_d_dangle;
    unsigned int num_mknod_d_fail;
    unsigned int num_mknod_d_eacces;
    unsigned int num_mknod_d_eperm;
    unsigned int num_follow_sym_outside;
    unsigned int num_scan_failed;
    unsigned int num_error;
    // above calculated during source scan (apart from *_d_* fields)
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
    unsigned int num_reg_d_enoent_enodev_enxio;
    unsigned int num_reg_d_e_other;
    int max_depth;
};

struct mut_opts_t {
    size_t starting_src_sz { };
    dev_t starting_fs_inst { };
    inmem_dir_t * cache_rt_dirp { };
    struct stats_t stats { };
    std::vector<sstring> deref_v;
};

struct opts_t {
    bool deref_given;
    bool destination_given;
    bool destin_all_new;    // checks for existing can be skipped if all_new
    bool do_extra;
    bool max_depth_active;  // for depth: 0 means one level below source_pt
    bool no_destin;         // -D
    bool clone_hidden;      // copy files starting with '.' (default: don't)
    bool no_xdev;           // -N : 'find(1) -xdev' means don't scan outside
                            // original fs so no_xdev is a double negative.
                            // (default for this utility: don't scan outside)
    bool source_given;      // once source is given, won't default destination
    bool wait_given;        // --wait=0 seems sufficient to cope waiting reads
    unsigned int reglen;    // maximum bytes read from regular file
    unsigned int wait_ms;   // to cope with waiting reads (e.g. /proc/kmsg)
    int cache_op_num;       // -c : cache SPATH to meomory then ...
    int max_depth;          // one less than given on command line
    int want_stats;         // should this be the default ? ?
    // int verbose;         // make file scope
    struct mut_opts_t * mutp;
    fs::path source_pt;         // a directory in canonical form
    fs::path destination_pt;    // (will be) a directory in canonical form
    std::vector<fs::path> exclude_v;    // vector of canonical paths
    std::shared_ptr<uint8_t[]> reg_buff_sp;
};

static const struct option long_options[] {
    {"cache", no_argument, 0, 'c'},
    {"dereference", required_argument, 0, 'R'},
    {"deref", required_argument, 0, 'R'},
    {"destination", required_argument, 0, 'd'},
    {"dst", required_argument, 0, 'd'},
    {"exclude", required_argument, 0, 'e'},
    {"extra", no_argument, 0, 'x'},
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
static const int stat_perm_mask { 0x1ff };         /* bottom 9 bits */
static const int def_file_perm { S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH };
static const char * symlink_src_tgt { "0_symlink_source_target" };

static auto dir_opt = fs::directory_options::skip_permission_denied;

/**
 * @param v - sorted vector instance
 * @param data - value to search
 * @return 0-based index if data found, -1 otherwise
*/
template <typename T>
    int binary_search_find_index(std::vector<T> v, const T & data)
    {
        auto it = std::lower_bound(v.begin(), v.end(), data);
        if (it == v.end() || *it != data) {
            return -1;
        } else {
            std::size_t index = std::distance(v.begin(), it);
            return index;
        }
    }


static const char * const usage_message1 {
    "Usage: clone_pseudo_fs [--cache] [--dereference=SPTSYM]\n"
    "                       [--destination=DPATH] [--exclude=PAT] "
    "[--extra]\n"
    "                       [--help] [--hidden] [--max-depth=MAXD] "
    "[--no-dst]\n"
    "                       [--no-xdev] [--reglen=RLEN] [--source=SPATH]\n"
    "                       [--statistics] [--verbose] [--version]\n"
    "                       [--wait=MS_R]\n"
    "  where:\n"
    "    --cache|-c         first cache SPATH to in-memory tree, then dump "
    "to\n"
    "                       DPATH. If used twice, also cache regular file\n"
    "                       contents\n"
    "    --dereference=SPTSYM|-r SPTSYM    SPTSYM should be a symlink "
    "within\n"
    "                                      SPATH which will become a "
    "directory\n"
    "                                      under DPATH (i.e. a 'deep' copy)\n"
    "    --destination=DPATH|-d DPATH    DPATH is clone destination (def:\n"
    "                                    /tmp/sys (unless SPATH given))\n"
    "    --exclude=PAT|-e PAT    PAT is a glob pattern, matching files and\n"
    "                            directories excluded (def: nothing "
    "excluded)\n"
    "    --extra|-x         do some extra sanity checking\n"
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
    "\n" };

static const char * const usage_message2 {
    "By default, this utility will clone /sys to /tmp/sys . The resulting "
    "subtree\nis a frozen snapshot that may be useful for later analysis. "
    "Hidden files\nare skipped and symlinks are created, even if dangling. "
    "The default is only\nto copy a maximum of 256 bytes from regular files."
    " If the --cache option\nis given, a two pass clone is used; the first "
    "pass creates an in memory tree.\n" };

static void
usage()
{
    scout << usage_message1;
    scout << usage_message2;
}

static sstring
inmem_var_str(inmem_var_e var)
{
    switch (var) {
    using enum inmem_var_e;

    //cases in definition order, first is 0
    case var_other:
        return "other inmem_var enumeration";
    case var_dir:
        return "directory";
    case var_symlink:
        return "symbolic link";
    case var_device:
        return "block or char device";
    case var_fifo_socket:
        return "fifo or socket";
    case var_regular:
        return "regular";
    default:
        return "unexpected inmem_var enumeration";
    }
}

static sstring
inmem_var_str(int var_i)
{
    return inmem_var_str(static_cast<inmem_var_e>(var_i));
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
pr3ser(const sstring & e1msg, const char * e2msg = nullptr,
       const std::error_code & ec = { },
       const std::source_location loc = std::source_location::current())
{
    sstring e1 { e1msg.empty() ? "<empty>" : e1msg };

    if (e2msg == nullptr)
        pr2ser(e1, ec, loc);
    else if (ec) {
        if (verbose > 1)
            scerr << loc.function_name() << ";ln=" << loc.line() << ": '"
                  << e1 << "': " << e2msg << ", error: "
                  << ec.message() << "\n";
        else
            scerr << "'" << e1 << "': " << e2msg << ", error: "
                  << ec.message() << "\n";
    } else {
        if (verbose > 1)
            scerr << loc.function_name() << ";ln=" << loc.line() << ": '"
                  << e1 << "': " << e2msg << "\n";
        else
            scerr << "'" << e1 << "': " << e2msg << "\n";
    }
}

static void
pr4ser(const sstring & e1msg, const sstring & e2msg,
       const char * e3msg = nullptr, const std::error_code & ec = { },
       const std::source_location loc = std::source_location::current())
{
    sstring e1 { e1msg.empty() ? "<empty>" : e1msg };
    sstring e2 { e2msg.empty() ? "<empty>" : e2msg };

    if (e3msg == nullptr)
        pr3ser(e1, e2.c_str(), ec, loc);
    else if (ec) {
        if (verbose > 1)
            scerr << loc.function_name() << ";ln=" << loc.line() << ": '"
                  << e1 << ", " << e2 << "': " << e3msg
                  << ", error: " << ec.message() << "\n";
        else
            scerr << "'" << e1 << ", " << e2 << "': " << e3msg
                  << ", error: " << ec.message() << "\n";
    } else {
        if (verbose > 1)
            scerr << loc.function_name() << ";ln=" << loc.line() << ": '"
                  << e1 << ", " << e2 << "': " << e3msg << "\n";
        else
            scerr << "'" << e1 << ", " << e2 << "': " << e3msg << "\n";
    }
}

void
inmem_subdirs_t::debug(const sstring intro) const
{
    size_t sdir_v_sz { sdir_v.size() };
    size_t sdir_fn_ind_m_sz { sdir_fn_ind_m.size() };

    if (intro.size() > 0)
        fprintf(stderr, "%s\n", intro.c_str());
    fprintf(stderr, "  sdir_v.size: %lu\n", sdir_v_sz);
    fprintf(stderr, "  sdir_fn_ind_m.size: %lu\n", sdir_fn_ind_m_sz);
    if ((verbose > 0) && (sdir_fn_ind_m_sz > 0)) {
        fprintf(stderr, "  sdir_fn_ind_m map:\n");
        for (auto && [n, v] : sdir_fn_ind_m)
            fprintf(stderr, "    [%s]--> %zu\n", n.c_str(), v);
    }
    if ((verbose > 1) && (sdir_v_sz > 0)) {
        fprintf(stderr, "  sdir_v vector:\n");
        for (int k { }; auto && v : sdir_v) {
            fprintf(stderr, "    %d:  %s, filename: %s\n", k,
                    inmem_var_str(v.derived.index()).c_str(),
                    v.filename.c_str());
            ++k;
        }
    }
}

// inmem_dir_t constructor
inmem_dir_t::inmem_dir_t() : sdirs_sp(std::make_shared<inmem_subdirs_t>())
{
}

void
inmem_dir_t::debug(const sstring intro) const
{
    const inmem_t * bp { get_basep() };

    if (intro.size() > 0)
        fprintf(stderr, "%s\n", intro.c_str());
    if (bp)
        bp->debug_base();
    else
        fprintf(stderr, "No base ptr ??\n");
    fprintf(stderr, "  directory\n");
    fprintf(stderr, "  parent_path: %s\n", par_pt_s.c_str());
    fprintf(stderr, "  depth: %d\n", depth);
    if (verbose > 0)
        fprintf(stderr, "     this: %p\n", (void *) this);
    if (sdirs_sp)
        sdirs_sp->debug();
}

void
inmem_other_t::debug(const sstring intro) const
{
    const inmem_t * bp { get_basep() };

    if (intro.size() > 0)
        fprintf(stderr, "%s\n", intro.c_str());
    if (bp)
        bp->debug_base();
    else
        fprintf(stderr, "No base ptr ??\n");
    fprintf(stderr, "  other file type\n");
    if (verbose > 0)
        fprintf(stderr, "     this: %p\n", (void *) this);
}

size_t
inmem_dir_t::add_to_sdir_v(inmem_other_t & n_oth, const sstring & filename,
                           const short_stat & a_shstat)
{
    if (sdirs_sp) {
        size_t sz = sdirs_sp->sdir_v.size();
        sdirs_sp->sdir_v.push_back(
                inmem_t { inm_var_t { n_oth /* std::move(n_oth) */ },
                          filename, a_shstat, sz } );
        return sz;
    }
    return 0;
}

size_t
inmem_dir_t::add_to_sdir_v(inmem_dir_t & n_dir, const sstring & filename,
                           const short_stat & a_shstat)
{
    if (sdirs_sp) {
        size_t sz { sdirs_sp->sdir_v.size() };
        sdirs_sp->sdir_v.push_back(
                inmem_t { inm_var_t { n_dir /* std::move(n_dir) */ },
                filename, a_shstat, sz } );
        sdirs_sp->sdir_fn_ind_m[filename] = sz; // only for directories
        return sz;
    }
    return 0;
}

size_t
inmem_dir_t::add_to_sdir_v(inmem_symlink_t & n_sym, const sstring & filename,
                           const short_stat & a_shstat)
{
    if (sdirs_sp) {
        size_t sz { sdirs_sp->sdir_v.size() };
        sdirs_sp->sdir_v.push_back(
                inmem_t { inm_var_t { n_sym /* std::move(n_sym) */ },
                          filename, a_shstat, sz } );
        return sz;
    }
    return 0;
}

size_t
inmem_dir_t::add_to_sdir_v(inmem_device_t & n_dev, const sstring & filename,
                           const short_stat & a_shstat)
{
    if (sdirs_sp) {
        size_t sz { sdirs_sp->sdir_v.size() };
        sdirs_sp->sdir_v.push_back(
                inmem_t { inm_var_t { n_dev /* std::move(n_dev) */ },
                          filename, a_shstat, sz } );
        return sz;
    }
    return 0;
}

size_t
inmem_dir_t::add_to_sdir_v(inmem_fifo_socket_t & n_fs, const sstring & fn,
                           const short_stat & a_shstat)
{
    if (sdirs_sp) {
        size_t sz { sdirs_sp->sdir_v.size() };
        sdirs_sp->sdir_v.push_back(
                inmem_t { inm_var_t { n_fs /* std::move(n_fs) */ },
                          fn, a_shstat, sz } );
        return sz;
    }
    return 0;
}

size_t
inmem_dir_t::add_to_sdir_v(inmem_regular_t & n_reg, const sstring & filename,
                           const short_stat & a_shstat)
{
    if (sdirs_sp) {
        size_t sz { sdirs_sp->sdir_v.size() };
        sdirs_sp->sdir_v.push_back(
                inmem_t { inm_var_t { n_reg /* std::move(n_reg) */ },
                          filename, a_shstat, sz } );
        return sz;
    }
    return 0;
}

void
inmem_symlink_t::debug(const sstring intro) const
{
    const inmem_t * bp { get_basep() };

    if (intro.size() > 0)
        fprintf(stderr, "%s\n", intro.c_str());
    if (bp)
        bp->debug_base();
    else
        fprintf(stderr, "No base ptr ??\n");
    fprintf(stderr, "  symlink\n");
    fprintf(stderr, "  target: %s\n", target.c_str());
}

void
inmem_device_t::debug(const sstring intro) const
{
    const inmem_t * bp { get_basep() };

    if (intro.size() > 0)
        fprintf(stderr, "%s\n", intro.c_str());
    if (bp)
        bp->debug_base();
    else
        fprintf(stderr, "No base ptr ??\n");
    fprintf(stderr, "  device type: %s\n", is_block_dev ? "block" : "char");
    fprintf(stderr, "  st_rdev: 0x%lx\n", st_rdev);
}

void
inmem_fifo_socket_t::debug(const sstring intro) const
{
    const inmem_t * bp { get_basep() };

    if (intro.size() > 0)
        fprintf(stderr, "%s\n", intro.c_str());
    if (bp)
        bp->debug_base();
    else
        fprintf(stderr, "No base ptr ??\n");
    fprintf(stderr, "  %s\n", "FIFO or socket");
}

void
inmem_regular_t::debug(const sstring intro) const
{
    const inmem_t * bp { get_basep() };

    if (intro.size() > 0)
        fprintf(stderr, "%s\n", intro.c_str());
    if (bp)
        bp->debug_base();
    else
        fprintf(stderr, "No base ptr ??\n");
    fprintf(stderr, "  regular file:\n");
    if (read_found_nothing)
        fprintf(stderr, "  read of contents found nothing\n");
    else if (contents.empty())
        fprintf(stderr, "  empty\n");
    else
        fprintf(stderr, "  file is %lu bytes long\n", contents.size());
}

void
inmem_t::debug(const sstring intro) const
{
    if (intro.size() > 0)
        fprintf(stderr, "%s\n", intro.c_str());
    if (const inmem_other_t * iothp { std::get_if<inmem_other_t>(&derived) })
        iothp->debug();
    else if (const inmem_dir_t * idirp { std::get_if<inmem_dir_t>(&derived) })
        idirp->debug();
    else if (const inmem_symlink_t * isymp
                         { std::get_if<inmem_symlink_t>(&derived) })
        isymp->debug();
    else if (const inmem_device_t * idevp
                         { std::get_if<inmem_device_t>(&derived) })
        idevp->debug();
    else if (const inmem_fifo_socket_t * ifsp
                         { std::get_if<inmem_fifo_socket_t>(&derived) })
        ifsp->debug();
    else if (const inmem_regular_t * iregp
                         { std::get_if<inmem_regular_t>(&derived) })
        iregp->debug();
    else if (verbose > 0)
        fprintf(stderr, "unknown object this=%p\n",
                static_cast<const void *>(this));
}

void
inmem_t::debug_base(const sstring intro) const
{
    if (intro.size() > 0)
        fprintf(stderr, "%s\n", intro.c_str());
    fprintf(stderr, "variant: %s\n", inmem_var_str(derived.index()).c_str());
    // fprintf(stderr, "name: %s\n", name.c_str());
    fprintf(stderr, "filename: %s\n", filename.c_str());
    fprintf(stderr, "parent_index: %zu\n", par_dir_ind);
    fprintf(stderr, "shstat.st_dev: 0x%lx\n", shstat.st_dev);
    fprintf(stderr, "shstat.st_mode: 0x%x\n", shstat.st_mode);
    if (verbose > 0)
        fprintf(stderr, "  this=%p\n", static_cast<const void *>(this));
}

// This assumes both paths are in canonical form. Will still work if
// needle_c_pt is in absolute form (e.g. when needle_c_pt is the link
// path of a symlink).
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

// Splits the parent path (par_pt_s) into a vector of strings containing the
// split up path, starting with (but not including) the initial SPATH path
// (base_pt_s). If an error is detected, ec is set appropriately.
static std::vector<sstring>
split_path(const sstring & par_pt_s, const sstring & base_pt_s,
           const struct opts_t *op, std::error_code & ec)
{
    std::vector<sstring> res;
    // sstring src_pt_s { op->source_pt.string() };
    // sstring par_pt_s { par_pt.string() };
    const char * pp_cp { par_pt_s.c_str() };
    const size_t base_pt_sz { base_pt_s.size() };
    size_t par_pt_sz { par_pt_s.size() };

    if ((base_pt_sz == par_pt_sz) &&
        (0 == memcmp(base_pt_s.c_str(), pp_cp, par_pt_sz)))
        return res;
    if (! path_contains_canon(op->source_pt, par_pt_s)) {
        ec.assign(EDOM, std::system_category());
        return res;
    }
    if (base_pt_sz == par_pt_sz)
        return res;
    if (base_pt_sz > par_pt_sz) {
        ec.assign(EINVAL, std::system_category());
        return res;
    }
    size_t pos = base_pt_sz;
    pp_cp += pos;
    par_pt_sz -= pos;

    if (*pp_cp == '/') {
        ++pp_cp;
        --par_pt_sz;
    }
    pos = 0;
    do {
        if (*pp_cp == '/') {
            ec.assign(EINVAL, std::system_category());
            return res;
        }
        par_pt_sz -= pos;
        const char * nxt_sl_p { strchr(pp_cp, '/') };

        if (nxt_sl_p == nullptr) {
            res.push_back(sstring(pp_cp));
            return res;
        }
        pos = nxt_sl_p - pp_cp;
        res.push_back(sstring(pp_cp, pos));
        pp_cp += ++pos;         // step over separator '/'
    } while (pos < par_pt_sz ) ;

    return res;
}

// Returns number of bytes read, -1 for general error, -2 for timeout
static int
read_err_wait(int from_fd, uint8_t * bp, int err, const struct opts_t *op)
{
    int num { -1 };
    struct stats_t * q { &op->mutp->stats };

    if (err == EAGAIN) {
        ++q->num_reg_s_eagain;
        if (op->wait_given) {
            struct pollfd a_pollfd {0, POLLIN, 0};

            a_pollfd.fd = from_fd;
            int r { poll(&a_pollfd, 1, op->wait_ms) };
            if (r == 0) {
                ++q->num_reg_s_timeout;
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
        ++q->num_reg_s_eacces;
    else if (err == EPERM)
        ++q->num_reg_s_eperm;
    else if (err == EIO)
        ++q->num_reg_s_eio;
    else if ((err == ENOENT) || (err == ENODEV) || (err == ENXIO))
        ++q->num_reg_s_enoent_enodev_enxio;
    else
        ++q->num_reg_s_e_other;

    return num;
}

// Returns 0 on success, else a Unix like errno value is returned.
static int
xfr_vec2file(const std::vector<uint8_t> & v, const sstring & destin_file,
             mode_t st_mode, const struct opts_t * op)
{
    int res { };
    int destin_fd { -1 };
    int num { static_cast<int>(v.size()) };
    int num2;
    mode_t from_perms { st_mode & stat_perm_mask };
    const uint8_t * bp { &v[0] };   // std::vector is continuous
    const char * destin_nm { destin_file.c_str() };
    struct stats_t * q { &op->mutp->stats };

    if (op->destin_all_new) {
        destin_fd = creat(destin_nm, from_perms);
        if (destin_fd < 0) {
            res = errno;
            if (res == EACCES)
                ++q->num_reg_d_eacces;
            else if (res == EPERM)
                ++q->num_reg_d_eperm;
            else if (res == EIO)
                ++q->num_reg_d_eio;
            else if ((res == ENOENT) || (res == ENODEV) || (res == ENXIO))
                ++q->num_reg_d_enoent_enodev_enxio;
            else
                ++q->num_reg_d_e_other;
            goto fini;
        }
    } else {
        destin_fd = open(destin_nm, O_WRONLY | O_CREAT | O_TRUNC, from_perms);
        if (destin_fd < 0) {
            res = errno;
            if (res == EACCES)
                ++q->num_reg_d_eacces;
            else if (res == EPERM)
                ++q->num_reg_d_eperm;
            else if (res == EIO)
                ++q->num_reg_d_eio;
            else if ((res == ENOENT) || (res == ENODEV) || (res == ENXIO))
                ++q->num_reg_d_enoent_enodev_enxio;
            else
                ++q->num_reg_d_e_other;
            goto fini;
        }
    }
    if (num > 0) {
        num2 = write(destin_fd, bp, num);
        if (num2 < 0) {
            res = errno;
            if (res == EACCES)
                ++q->num_reg_d_eacces;
            else if (res == EPERM)
                ++q->num_reg_d_eperm;
            else if (res == EIO)
                ++q->num_reg_d_eio;
            else if ((res == ENOENT) || (res == ENODEV) || (res == ENXIO))
                ++q->num_reg_d_enoent_enodev_enxio;
            else
                ++q->num_reg_d_e_other;
            goto fini;
        }
        if ((num2 < num) && (verbose > 0))
            pr3ser(destin_nm, "short write(), strange");
    }
fini:
    if (destin_fd >= 0)
        close(destin_fd);
    if (res == 0)
        ++q->num_reg_success;
    return res;
}

// Returns 0 on success, else a Unix like errno value is returned.
static int
xfr_reg_file2inmem(const sstring & from_file, inmem_regular_t & ireg,
                   const struct opts_t * op)
{
    int res { 0 };
    int from_fd { -1 };
    int rd_flags { O_RDONLY };
    int from_perms, num;
    uint8_t * bp;
    const char * from_nm { from_file.c_str() };
    struct stats_t * q { &op->mutp->stats };
    struct stat from_stat;
    uint8_t fix_b[def_reglen];

    ++q->num_reg_tries;
    if (op->reglen <= def_reglen)
        bp = fix_b;
    else
        bp = op->reg_buff_sp.get();
    if (bp == nullptr) {
        ++q->num_reg_s_e_other;
        return ENOMEM;
    }
    if (op->wait_given && (op->reglen > 0))
        rd_flags |= O_NONBLOCK;
    from_fd = open(from_nm, rd_flags);
    if (from_fd < 0) {
        res = errno;
        if (res == EACCES) {
            if (stat(from_nm, &from_stat) < 0) {
                res = errno;
                if (res == EACCES)
                    ++q->num_reg_s_eacces;
                else if (res == EPERM)
                    ++q->num_reg_s_eperm;
                else if (res == EIO)
                    ++q->num_reg_s_eio;
                else if ((res == ENOENT) || (res == ENODEV) || (res == ENXIO))
                    ++q->num_reg_s_enoent_enodev_enxio;
                else
                    ++q->num_reg_s_e_other;
                goto fini;
            }
            res = 0;
            from_perms = from_stat.st_mode & stat_perm_mask;
            num = 0;
            goto store;
        } else if (res == EPERM)
            ++q->num_reg_s_eperm;
        else if (res == EIO)
            ++q->num_reg_s_eio;
        else if ((res == ENOENT) || (res == ENODEV) || (res == ENXIO))
            ++q->num_reg_s_enoent_enodev_enxio;
        else
            ++q->num_reg_s_e_other;
        goto fini;
    }
    if (fstat(from_fd, &from_stat) < 0) {
        res = errno;
        ++q->num_reg_s_e_other;  // not expected if open() is good
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
                res = 0;
                close(from_fd);
                goto store;
            }
        }
    } else
        num = 0;
    // closing now might help in this function is multi-threaded
    close(from_fd);
    if (static_cast<unsigned int>(num) >= op->reglen)
        ++q->num_reg_s_at_reglen;

store:
    if (num > 0) {
        std::vector<uint8_t> l_contents(bp, bp + num);
        ireg.contents.swap(l_contents);
        ireg.read_found_nothing = false;
    } else if (num == 0)
        ireg.read_found_nothing = true;
    ireg.get_basep()->shstat.st_mode = from_perms;

fini:
    if (from_fd >= 0)
        close(from_fd);
    if (res == 0)
        ++q->num_reg_success;
    return res;
}

// Returns 0 on success, else a Unix like errno value is returned.
static int
xfr_reg_inmem2file(const inmem_regular_t & ireg, const sstring & destin_file,
                   const struct opts_t * op)
{
    mode_t from_perms { ireg.get_basep()->shstat.st_mode & stat_perm_mask };

    return xfr_vec2file(ireg.contents, destin_file, from_perms, op);
}

// N.B. Only root can successfully invoke the mknod(2) system call
static int
xfr_dev_inmem2file(const inmem_device_t & idev, const sstring & destin_file,
                   const struct opts_t * op)
{
    int res { };
    const auto * inmem_p = idev.get_basep();
    struct stats_t * q { &op->mutp->stats };

    if (mknod(destin_file.c_str(), inmem_p->shstat.st_mode,
              idev.st_rdev) < 0) {
        res = errno;
        if (res == EACCES)
            ++q->num_mknod_d_eacces;
        else if (res == EPERM)
            ++q->num_mknod_d_eperm;
        else
            ++q->num_mknod_d_fail;
    } else {
        if (idev.is_block_dev)
            ++q->num_block;
        else
            ++q->num_char;
    }
    return res;
}

// Returns 0 on success, else a Unix like errno value is returned.
static int
xfr_reg_file2file(const sstring & from_file, const sstring & destin_file,
                  const struct opts_t * op)
{
    int res { 0 };
    int from_fd { -1 };
    int rd_flags { O_RDONLY };
    int num;
    mode_t from_perms;
    uint8_t * bp;
    const char * from_nm { from_file.c_str() };
    struct stats_t * q { &op->mutp->stats };
    struct stat from_stat;
    uint8_t fix_b[def_reglen];

    ++q->num_reg_tries;
    if (op->reglen <= def_reglen)
        bp = fix_b;
    else
        bp = op->reg_buff_sp.get();
    if (bp == nullptr) {
        ++q->num_reg_s_e_other;
        return ENOMEM;
    }
    if (op->wait_given && (op->reglen > 0))
        rd_flags |= O_NONBLOCK;
    from_fd = open(from_nm, rd_flags);
    if (from_fd < 0) {
        res = errno;
        if (res == EACCES) {
            if (stat(from_nm, &from_stat) < 0) {
                res = errno;
                if (res == EACCES)
                    ++q->num_reg_s_eacces;
                else if (res == EPERM)
                    ++q->num_reg_s_eperm;
                else if (res == EIO)
                    ++q->num_reg_s_eio;
                else if ((res == ENOENT) || (res == ENODEV) || (res == ENXIO))
                    ++q->num_reg_s_enoent_enodev_enxio;
                else
                    ++q->num_reg_s_e_other;
                goto fini;
            }
            res = 0;
            from_perms = from_stat.st_mode & stat_perm_mask;
            num = 0;
            goto do_destin;
        } else if (res == EPERM)
            ++q->num_reg_s_eperm;
        else if (res == EIO)
            ++q->num_reg_s_eio;
        else if ((res == ENOENT) || (res == ENODEV) || (res == ENXIO))
            ++q->num_reg_s_enoent_enodev_enxio;
        else
            ++q->num_reg_s_e_other;
        goto fini;
    }
    if (fstat(from_fd, &from_stat) < 0) {
        res = errno;
        ++q->num_reg_s_e_other;  // not expected if open() is good
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
                res = 0;
                close(from_fd);
                goto do_destin;
            }
        }
    } else
        num = 0;
    // closing now might help in this function is multi-threaded
    close(from_fd);
    if (static_cast<unsigned int>(num) >= op->reglen)
        ++q->num_reg_s_at_reglen;
do_destin:
    from_fd = -1;
    res = xfr_vec2file(std::vector<uint8_t>(bp, bp + num), destin_file,
                       from_perms, op);
fini:
    if (from_fd >= 0)
        close(from_fd);
    if (res == 0)
        ++q->num_reg_success;
    return res;
}

static std::error_code
xfr_other_ft(fs::file_type ft, const fs::path & src_pt,
             const struct stat & src_stat, const fs::path & dst_pt,
             const struct opts_t * op)
{
    int res { };
    std::error_code ec { };
    struct stats_t * q { &op->mutp->stats };

    if (verbose > 3)
        scerr << __func__ << ": ft=" << static_cast<int>(ft)
              << ", src_pt: " << src_pt.c_str()
              << ", dst_pt: " << dst_pt.c_str()  << "\n";
    switch (ft) {
    using enum fs::file_type;

    case regular:
        res = xfr_reg_file2file(src_pt, dst_pt, op);
        if (res) {
            ec.assign(res, std::system_category());
            if (verbose > 3) {
                pr4ser(src_pt, dst_pt, "xfr_reg_file2file() "
                       "failed", ec);
                ++q->num_error;
            }
        } else if (verbose > 5)
            pr4ser(src_pt, dst_pt, "xfr_reg_file2file() ok");
        break;
    case block:
    case character:
        // N.B. Only root can successfully invoke the mknod(2) system call
        if (mknod(dst_pt.c_str(), src_stat.st_mode,
                  src_stat.st_rdev) < 0) {
            res = errno;
            ec.assign(res, std::system_category());
            if (verbose > 3) {
                pr4ser(src_pt, dst_pt, "mknod() failed", ec);
            }
            if (res == EACCES)
                ++q->num_mknod_d_eacces;
            else if (res == EPERM)
                ++q->num_mknod_d_eperm;
            else
                ++q->num_mknod_d_fail;
        } else if (verbose > 5)
            pr4ser(src_pt, dst_pt, "mknod() ok");
        break;
    case fifo:
        if (verbose > 0)
            pr3ser(src_pt, "<< cloning file type: fifo not supported");
        break;
    case socket:
        if (verbose > 0)
            pr3ser(src_pt, "<< cloning file type: socket not supported");
        break;              // skip these file types
    default:                // here when something no longer exists
        if (verbose > 3)
            fprintf(stderr, "%s: unexpected file_type=%d\n", __func__,
                   static_cast<int>(ft));
        break;
    }
    return ec;
}

static void
update_stats(const fs::file_type & s_sym_ftype,
             const fs::file_type & targ_ftype, bool hidden,
             const struct opts_t * op)
{
    struct stats_t * q { &op->mutp->stats };

    if (hidden)
        ++q->num_hidden;
    if (s_sym_ftype == fs::file_type::symlink) {
        if (targ_ftype == fs::file_type::directory)
            ++q->num_sym2dir;
        else if (targ_ftype == fs::file_type::regular)
            ++q->num_sym2reg;
        else if (targ_ftype == fs::file_type::block)
            ++q->num_sym2block;
        else if (targ_ftype == fs::file_type::character)
            ++q->num_sym2char;
        else if (targ_ftype == fs::file_type::none)
            ++q->num_symlink;
        else if (targ_ftype == fs::file_type::not_found)
            ++q->num_sym_s_dangle;
        else
            ++q->num_sym_other;
        return;
    }
    switch (s_sym_ftype) {
    using enum fs::file_type;

    case directory:
        ++q->num_dir;
        break;
    case symlink:
        ++q->num_symlink;
        break;
    case regular:
        ++q->num_regular;
        break;
    case block:
        ++q->num_block;
        break;
    case character:
        ++q->num_char;
        break;
    case fifo:
        ++q->num_fifo;
        break;
    case socket:
        ++q->num_socket;
        break;
    case not_found:
        ++q->num_sym_s_dangle;
        break;
    case none:
    default:
        ++q->num_other;
        break;
    }
}

static void
show_stats(const struct opts_t * op)
{
    bool extra { (op->want_stats > 1) || (verbose > 0) };
    bool eagain_likely { (op->wait_given && (op->reglen > 0)) };
    struct stats_t * q { &op->mutp->stats };

    scout << "Statistics:\n";
    scout << "Number of nodes: " << q->num_node << "\n";
    scout << "Number of regular files: " << q->num_regular << "\n";
    scout << "Number of directories: " << q->num_dir << "\n";
    scout << "Number of symlinks to directories: " << q->num_sym2dir << "\n";
    scout << "Number of symlinks to regular files: " << q->num_sym2reg
          << "\n";
    if (extra) {
        scout << "Number of symlinks to block device nodes: "
              << q->num_sym2block << "\n";
        scout << "Number of symlinks to char device nodes: "
              << q->num_sym2char << "\n";
    }
    if (q->num_sym_other > 0)
        scout << "Number of symlinks to others: " << q->num_sym_other << "\n";
    if (q->num_symlink > 0)
        scout << "Number of symlinks: " << q->num_symlink << "\n";
    scout << "Number of source dangling symlinks: " << q->num_sym_s_dangle
          << "\n";
    scout << "Number of hidden files skipped: " << q->num_hidden_skipped
          << "\n";
    if (! op->no_xdev)
        scout << "Number of other file systems skipped: "
              << q->num_oth_fs_skipped << "\n";
    scout << "Number of block device nodes: " << q->num_block << "\n";
    scout << "Number of char device nodes: " <<  q->num_char << "\n";
    if (extra) {
        scout << "Number of fifo_s: " << q->num_fifo << "\n";
        scout << "Number of sockets: " << q->num_socket << "\n";
        scout << "Number of other file types: " << q->num_other << "\n";
    }
    scout << "Number of filenames starting with '.': " << q->num_hidden
          << "\n";
    if (! op->no_destin) {
        scout << "Number of dst created directories: "
              << q->num_dir_d_success << "\n";
        scout << "Number of already existing dst directories: "
              << q->num_dir_d_exists << "\n";
        scout << "Number of dst created symlinks: "
              << q->num_sym_d_success << "\n";
        scout << "Number of dst dangling symlinks: " << q->num_sym_d_dangle
              << " [may be resolved later in scan]\n";
        if ((q->num_mknod_d_fail > 0) || (q->num_mknod_d_eacces > 0) ||
            (q->num_mknod_d_eperm > 0)) {
            scout << "Number of dst mknod EACCES failures: "
                  << q->num_mknod_d_eacces << "\n";
            scout << "Number of dst mknod EPERM failures: "
                  << q->num_mknod_d_eperm << "\n";
            scout << "Number of dst mknod other failures: "
                  << q->num_mknod_d_fail << "\n";
        }
        if (op->deref_given)
            scout << "Number of follow symlinks outside subtree: "
                  << q->num_follow_sym_outside << "\n";
    }
    scout << "Number of files excluded: " << q->num_excluded << "\n";
    // N.B. recursive_directory_iterator::depth() is one less than expected
    scout << "Maximum depth of source scan: " << q->max_depth + 1 << "\n";
    scout << "Number of scans truncated: " << q->num_scan_failed << "\n";
    scout << "Number of other errors: " << q->num_error << "\n";
    if (op->no_destin && (op->cache_op_num < 2))
        return;

    scout << "\n>> Following associated with clone/copy of regular files\n";
    scout << "Number of attempts to clone a regular file: "
          << q->num_reg_tries << "\n";
    scout << "Number of clone regular file successes: " << q->num_reg_success
          << "\n";
    scout << "Number of source EACCES, EPERM, EIO errors: "
          << q->num_reg_s_eacces << ", " << q->num_reg_s_eperm << ", "
          << q->num_reg_s_eio << "\n";
    scout << "Number of source ENOENT, ENODEV or ENXIO errors, combined: "
          << q->num_reg_s_enoent_enodev_enxio << "\n";
    if (extra || eagain_likely) {
        scout << "Number of source EAGAIN errors: " << q->num_reg_s_eagain
              << "\n";
        scout << "Number of source poll timeouts: " << q->num_reg_s_timeout
              << "\n";
    }
    scout << "Number of source other errors: " << q->num_reg_s_e_other
          << "\n";
    if (! op->no_destin) {
        scout << "Number of dst EACCES, EPERM, EIO errors: "
              << q->num_reg_d_eacces <<  ", " << q->num_reg_d_eperm << ", "
              << q->num_reg_d_eio << "\n";
        scout << "Number of dst ENOENT, ENODEV or ENXIO errors, combined: "
              << q->num_reg_d_enoent_enodev_enxio << "\n";
        scout << "Number of dst other errors: " << q->num_reg_d_e_other
              << "\n";
    }
    scout << "Number of files " << op->reglen << " bytes or longer: "
          << q->num_reg_s_at_reglen << "\n";
}

// Called from do_clone() and if --deref= given may call itself recursively.
// There are two levels of error reporting, when ecc is set it will cause
// the a bear immediate return of that value. If this function has been
// called recursively, the recursive stack will be quickly unwound. The
// other variety of errors are placed in 'ec' and are reported in the
// statistics and may cause processing of the currently node to be stopped
// and processing will continue to the next node.
static std::error_code
clone_work(int dc_depth, const fs::path & src_pt, const fs::path & dst_pt,
           const struct opts_t * op)
{
    bool possible_exclude { ! op->exclude_v.empty() };
    bool exclude_entry { false };
    bool possible_deref { ! op->mutp->deref_v.empty() };
    bool deref_entry { false };
    struct stats_t * q { &op->mutp->stats };
    struct stat src_stat;
    std::error_code ecc { };

    if (dc_depth > 0) {
        // Check (when dc_depth > 0) whether src_pt is a directory
        if (op->do_extra) {
            bool src_pt_contained { path_contains_canon(op->source_pt,
                                                        src_pt) };
            bool dst_pt_contained { path_contains_canon(op->destination_pt,
                                                        dst_pt) };
            bool bad { false };

            if (src_pt_contained || dst_pt_contained) {
                if (verbose > 3)
                    scerr << __func__
                          << ": both src and dst contained, good\n";
            } else if (! src_pt_contained) {
                scerr << __func__ << ": src: " << src_pt
                      << " NOT contained, bad\n";
                bad = true;
            } else {
                scerr << __func__ << ": dst: " << dst_pt
                      << " NOT contained, bad\n";
                bad = true;
            }
            fprintf(stderr, "%s: dc_depth=%d, src_pt: %s, dst_pt: %s\n",
                    __func__, dc_depth, src_pt.c_str(), dst_pt.c_str());
            if (bad) {
                ecc.assign(EDOM, std::system_category());
                return ecc;
            }
        }
        const auto src_ft { fs::status(src_pt, ecc).type() };

        if (ecc) {
            pr3ser(src_pt, "failed getting file type");
            return ecc;
        }
        if (src_ft != fs::file_type::directory) {
            if (stat(src_pt.c_str(), &src_stat) < 0) {
                ecc.assign(errno, std::system_category());
                pr3ser(src_pt, "stat() failed", ecc);
                ++q->num_error;
                return ecc;
            }
            return xfr_other_ft(src_ft, src_pt, src_stat, dst_pt, op);
        }       // drops through if is directory [[expected]]
    }

    // All fs calls use std::error_code so nothing should throw ... g++
    const fs::recursive_directory_iterator end_itr { };

    for (fs::recursive_directory_iterator itr(src_pt, dir_opt, ecc);
         (! ecc) && itr != end_itr;
         itr.increment(ecc) ) {

        // since src_pt is in canonical form, assume entry.path()
        // will either be in canonical form, or absolute form if symlink
        std::error_code ec { };
        const fs::path pt { itr->path() };
        const auto depth { itr.depth() };
        prev_rdi_pt = pt;

        ++q->num_node;
        if (verbose > 6)
            pr3ser(pt, "about to scan this source entry");
        const auto s_sym_ftype = itr->symlink_status(ec).type();
        if (ec) {       // serious error
            ++q->num_error;
            if (verbose > 2)
                pr3ser(pt, "symlink_status() failed, continue", ec);
            // thought of using entry.refresh(ec) but no speed improvement
            continue;
        }
        if (depth > q->max_depth)
            q->max_depth = depth;
        fs::file_type targ_ftype { fs::file_type::none };

        if (itr->exists(ec)) {
            targ_ftype = itr->status(ec).type();
            if (ec) {
                if (verbose > 4)
                    pr3ser(pt, "status() failed, continue", ec);
                ++q->num_error;
                continue;
            }
            // this conditional is a sanity check, may be overkill
            if (op->do_extra) {
                if ((s_sym_ftype == fs::file_type::symlink) &&
                    (! path_contains_canon(src_pt, pt))) {
                    pr4ser(src_pt, pt, "is not contained in source path?");
                    break;
                }
            }
        } else {
            // expect s_sym_ftype to be symlink, so dangling target
            if (ec) {
                ++q->num_error;
                if (verbose > 4)
                    pr3ser(pt, "itr->exists() failed, continue", ec);
            } else if (verbose > 1)
                pr3ser(pt, "<< does not exist, continue");
            continue;
        }

        const bool me_hidden = ((! pt.empty()) &&
                                (pt.filename().string()[0] == '.'));
        if (possible_exclude) {
            exclude_entry = std::ranges::binary_search(op->exclude_v, pt);
            if (exclude_entry) {
                ++q->num_excluded;
                if (verbose > 3)
                    pr3ser(pt, "matched for exclusion");
            }
        }
        if (possible_deref && (s_sym_ftype == fs::file_type::symlink)) {
            const auto & pt_s { pt.string() };

            deref_entry = false;
            const auto ind = binary_search_find_index(op->mutp->deref_v,
                                                      pt_s);
            if (ind >= 0) {
                deref_entry = true;
                // make sure this match is not rematched by removing it!
                // auto it = std::remove(op->mutp->deref_v.begin(),
                                      // op->mutp->deref_v.end(), pt_s);
                op->mutp->deref_v.erase(op->mutp->deref_v.begin() + ind);
                if (op->mutp->deref_v.empty())
                    possible_deref = false;
                ++q->num_derefed;
                if (verbose > 3)
                    pr3ser(pt, "matched for dereference");
            }
        }
        if (op->want_stats > 0)
            update_stats(s_sym_ftype, targ_ftype, me_hidden, op);
        if (op->no_destin) {
            if ((s_sym_ftype == fs::file_type::directory) &&
                op->max_depth_active && (depth >= op->max_depth)) {
                if (verbose > 2)
                    scerr << "Source at max_depth: " << pt
                          << " [" << depth << "], don't enter\n";
                itr.disable_recursion_pending();
                continue;
            }
        }
        if ((! op->clone_hidden) && me_hidden) {
            ++q->num_hidden_skipped;
            continue;
        }
        if ((targ_ftype != fs::file_type::none) &&
            (stat(pt.c_str(), &src_stat) < 0)) {
            ec.assign(errno, std::system_category());
            pr3ser(pt, "stat() failed", ec);
            ++q->num_error;
            continue;
        }
        fs::path rel_pt { fs::proximate(pt, src_pt, ec) };
        if (ec) {
            if (verbose > 1)
                pr3ser(pt, "proximate() failed", ec);
            ++q->num_error;
            continue;
        }
        fs::path ongoing_destin_pt { dst_pt / rel_pt };
        if (verbose > 2)
             fprintf(stderr, "%s: pt: %s, rel_path: %s, ongoing_d_pt: %s\n",
                     __func__, pt.c_str(), rel_pt.c_str(),
                     ongoing_destin_pt.c_str());

        switch (s_sym_ftype) {
        using enum fs::file_type;

        case directory:
            if (! op->no_xdev) { // double negative ...
                if (src_stat.st_dev != op->mutp->starting_fs_inst) {
                    // do not visit this sub-branch: different fs instance
                    if (verbose > 1)
                        scerr << "Source trying to leave this fs instance "
                                 "at: " << pt << "\n";
                    itr.disable_recursion_pending();
                    ++q->num_oth_fs_skipped;
                }
            }
            if (exclude_entry) {
                itr.disable_recursion_pending();
                continue;
            }
            if (op->max_depth_active && (depth >= op->max_depth)) {
                if (verbose > 2)
                    scerr << "Source at max_depth and this is a directory: "
                          << pt << ", don't enter\n";
                itr.disable_recursion_pending();
            }
            if (! op->destin_all_new) { /* may already exist */
                if (fs::exists(ongoing_destin_pt, ec)) {
                    if (fs::is_directory(ongoing_destin_pt, ec)) {
                        ++q->num_dir_d_exists;
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
                if (ec) {
                    ++q->num_error;
                    if (verbose > 1)
                        pr4ser(ongoing_destin_pt, pt, "create_directory() "
                               "failed", ec);
                } else {
                    ++q->num_dir_d_exists;
                    if (verbose > 2)
                        pr3ser(ongoing_destin_pt, "exists so "
                               "create_directory() failed");
                }
                break;
            } else {
                const fs::perms s_pms = itr->status(ec).permissions();
                if (ec) {
                    ++q->num_error;
                    if (verbose > 1)
                        pr4ser(ongoing_destin_pt, pt, "itr->status() failed",
                               ec);
                    break;
                }
                if ((s_pms & fs::perms::owner_write) !=
                    fs::perms::owner_write) {
                    // if source directory doesn't have owner_write then
                    // make sure destination does.
                    fs::permissions(ongoing_destin_pt, fs::perms::owner_write,
                                    fs::perm_options::add, ec);
                    if (ec) {
                        pr3ser(ongoing_destin_pt, "couldn't add owner_write "
                               "perm", ec);
                        ++q->num_error;
                        break;
                    }
                }
                ++q->num_dir_d_success;
                if (verbose > 4)
                    pr3ser(pt, "create_directory() ok");
            }
            break;
        case symlink:
            {
                // what to do about exclude_entry==true ?
                const fs::path target_pt = fs::read_symlink(pt, ec);
                if (ec) {
                    if (verbose > 1)
                        pr3ser(pt, "read_symlink() failed", ec);
                    auto err = ec.value();
                    if (err == EACCES)
                        ++q->num_reg_s_eacces;
                    else if (err == EPERM)
                        ++q->num_reg_s_eperm;
                    else
                        ++q->num_sym_s_dangle;
                    break;
                }
                const fs::path pt_parent_pt { pt.parent_path() };
                fs::path prox_pt { dst_pt };
                if (pt_parent_pt != src_pt) {
                    prox_pt /= fs::proximate(pt_parent_pt, src_pt, ec);
                    if (ec) {
                        pr3ser(pt_parent_pt, "symlink: proximate() failed",
                               ec);
                        ++q->num_error;
                        break;
                    }
                }
                fs::path d_lnk_pt { prox_pt / pt.filename() };
                if (! op->destin_all_new) {     /* may already exist */
                    const fs::file_type d_lnk_ftype =
                                fs::symlink_status(d_lnk_pt, ec).type();

                    if (ec) {
                        int v { ec.value() };

                        // ENOENT can happen with "dynamic" sysfs
                        if ((v == ENOENT) || (v == ENODEV) || (v == ENXIO)) {
                            ++q->num_reg_s_enoent_enodev_enxio;
                            if (verbose > 4)
                                pr3ser(d_lnk_pt, "symlink_status() failed",
                                       ec);
                        } else {
                            ++q->num_error;
                            if (verbose > 2)
                                pr3ser(d_lnk_pt, "symlink_status() failed",
                                       ec);
                        }
                        break;
                    }
                    if (d_lnk_ftype == fs::file_type::symlink)
                        break;
                    else if (d_lnk_ftype == fs::file_type::not_found)
                        ;       // drop through
                    else if (deref_entry &&
                             (d_lnk_ftype == fs::file_type::directory))
                        break;  // skip because destination is already dir
                    else {
                        pr3ser(d_lnk_pt, "unexpected d_lnk_ftype", ec);
                        ++q->num_error;
                        break;
                    }
                }

                if (deref_entry) {   /* symlink becomes directory */
                    deref_entry = false;
                    const fs::path join_pt { pt.parent_path() / target_pt };
                    const fs::path canon_targ_s_pt
                                        { fs::canonical(join_pt, ec) };
                    if (ec) {
                        if (verbose > 0) {
                            pr3ser(canon_targ_s_pt, "canonical() failed", ec);
                            pr3ser(pt, "<< symlink probably dangling");
                        }
                        ++q->num_sym_s_dangle;
                        break;
                    }
                    if (! path_contains_canon(op->source_pt,
                                              canon_targ_s_pt)) {
                        ++q->num_follow_sym_outside;
                        if (verbose > 0)
                            pr3ser(canon_targ_s_pt,
                                   "<< outside, fall back to symlink");
                        goto back_to_symlink;
                    }
                    const auto targ_s_ft
                                { fs::status(canon_targ_s_pt, ec).type() };
                    if (ec) {
                        if (verbose > 0)
                            pr3ser(canon_targ_s_pt, "fs::status() failed",
                                   ec);
                        ++q->num_error;
                        break;
                    }
                    if (targ_s_ft == fs::file_type::directory) {
                        // create dir when src is symlink and follow active
                        // no problem if already exists
                        fs::create_directory(d_lnk_pt, ec);
                        if (ec) {
                            if (verbose > 0)
                                pr3ser(d_lnk_pt, "create_directory() failed",
                                       ec);
                            ++q->num_error;
                            break;
                        }
                        const fs::path deep_d_pt
                                { ongoing_destin_pt / d_lnk_pt };

                        // N.B. recursive call!
                        if (op->max_depth_active &&
                            (dc_depth >= op->max_depth)) {
                            scerr << "clone_work() hits max_depth: "
                                  << canon_targ_s_pt << " [" << dc_depth
                                  << "], don't enter\n";
                            ++q->num_error;
                            ecc.assign(ELOOP, std::system_category());
                            return ecc;  // propagate
                        }
                        const auto d_sl_tgt { d_lnk_pt / symlink_src_tgt };
                        const auto ctspt { canon_targ_s_pt.string() + "\n" };
                        const char * ccp { ctspt.c_str() };
                        const uint8_t * bp
                                { reinterpret_cast<const uint8_t *>(ccp) };
                        std::vector<uint8_t> v(bp, bp + ctspt.size());

                        int res = xfr_vec2file(v, d_sl_tgt, def_file_perm, op);
                        if (res) {
                            ec.assign(res, std::system_category());
                            if (verbose > 3) {
                                pr3ser(pt, "xfr_vec2file() failed", ec);
                                ++q->num_error;
                            }
                        }
                        ++dc_depth;
                        ecc = clone_work(dc_depth, canon_targ_s_pt, deep_d_pt,
                                         op);
                        --dc_depth;
                        if (ecc) {
                            pr3ser(canon_targ_s_pt, "clone_work() failed",
                                   ecc);
                            ++q->num_error;
                            return ecc;  // propagate error
                        }
                    } else {
                        if (verbose > 0)
                            pr3ser(canon_targ_s_pt,
                                   "<< for non-dirs, fall back to symlink");
                        goto back_to_symlink;
                    }
                    break;
                }               // end of id (deref_entry)
back_to_symlink:
                fs::create_symlink(target_pt, d_lnk_pt, ec);
                if (ec) {
                    if (verbose > 0)
                        pr4ser(target_pt, d_lnk_pt, "create_symlink() failed",
                               ec);
                    ++q->num_error;
                } else {
                    ++q->num_sym_d_success;
                    if (verbose > 4)
                        pr4ser(target_pt, d_lnk_pt, "create_symlink() ok");
                    fs::path abs_target_pt { prox_pt / target_pt };
                    if (fs::exists(abs_target_pt, ec)) {
                        if (verbose > 4)
                            pr3ser(abs_target_pt, "<< symlink target exists");
                    } else if (ec) {
                        pr3ser(abs_target_pt, "fs::exists() failed", ec);
                        ++q->num_error;
                    } else
                        ++q->num_sym_d_dangle;
                }
            }
            break;
        case regular:
        case block:
        case character:
        case fifo:
        case socket:
        case unknown:
            if (exclude_entry)
                continue;
            ec = xfr_other_ft(s_sym_ftype, pt, src_stat, ongoing_destin_pt,
                              op);
            ec.clear();
            break;
        default:                // here when something no longer exists
            switch (targ_ftype) {
            using enum fs::file_type;

            case directory:
                if (op->max_depth_active && (depth >= op->max_depth)) {
                    if (verbose > 2)
                        scerr << "Source at max_depth: " << pt
                              << " [" << depth << "], don't enter\n";
                    itr.disable_recursion_pending();
                }
                break;
            case symlink:
                if (verbose > 2)
                    pr4ser(pt, "switch in switch symlink, skip");
                break;
            case regular:
                if (verbose > 2)
                    pr4ser(pt, "switch in switch regular file, skip");
                break;
            default:
                if (verbose > 2)
                    scerr << pt << ", switch in switch s_sym_ftype: "
                          << static_cast<int>(s_sym_ftype) << "\n";

                break;
            }
            break;
        }               // end of switch (s_sym_ftype)
    }                   // end of recursive_directory scan for loop
    if (ecc) {
        ++q->num_scan_failed;
        pr3ser(prev_rdi_pt,
               "<< previous, recursive_directory_iterator() failed", ecc);
    }
    return ecc;
}

static std::error_code
cache_src(int dc_depth, inmem_dir_t * odirp, const fs::path & src_pt,
          const struct opts_t * op)
{
    const bool possible_exclude
                { (dc_depth == 0) && (! op->exclude_v.empty()) };
    bool exclude_entry { false };
    bool possible_deref { (dc_depth == 0) && (! op->mutp->deref_v.empty()) };
    bool deref_entry { false };
    int depth { dc_depth };
    int prev_depth { dc_depth - 1 };    // assume descendind into directory
    int prev_dir_ind { -1 };
    struct stats_t * q { &op->mutp->stats };
    inmem_dir_t * l_odirp { odirp };
    inmem_dir_t * prev_odirp { };
    std::error_code ecc { };
    fs::path pt;
    fs::path par_pt;
    short_stat a_shstat;

    if (l_odirp == nullptr) {
        pr2ser("odirp is null ?");
        ecc.assign(EINVAL, std::system_category());
        return ecc;
    }
    if (dc_depth > 0) {
        // Check (when dc_depth > 0) whether src_pt is a directory
        if (op->do_extra) {
            bool src_pt_contained { path_contains_canon(op->source_pt,
                                                        src_pt) };

            if (! src_pt_contained) {
                scerr << __func__ << ": src: " << src_pt
                      << " NOT contained, bad\n";
                if (verbose > 0)
                    fprintf(stderr, "%s: dc_depth=%d, src_pt: %s\n", __func__,
                            dc_depth, src_pt.c_str());
                ecc.assign(EDOM, std::system_category());
                return ecc;
            }
        }
        /* assume src_pt is a directory */
    }

    const fs::recursive_directory_iterator end_itr { };

    for (fs::recursive_directory_iterator itr(src_pt, dir_opt, ecc);
         (! ecc) && itr != end_itr;
         itr.increment(ecc), prev_depth = depth) {

        std::error_code ec { };
        // since src_pt is in canonical form, assume entry.path()
        // will either be in canonical form, or absolute form if symlink
        pt = itr->path();
        prev_rdi_pt = pt;
        const sstring filename { pt.filename() };
        par_pt = pt.parent_path();
        depth = itr.depth();
        fs::file_type s_sym_ftype { itr->symlink_status(ec).type() };
        if (ec) {       // serious error
            ++q->num_error;
            if (verbose > 2)
                pr3ser(pt, "symlink_status() failed, continue", ec);
            // thought of using entry.refresh(ec) but no speed improvement
            continue;
        }
        const auto l_isdir = (s_sym_ftype == fs::file_type::directory);
        if (depth > prev_depth) {
            if (depth == (prev_depth + 1))
                prev_odirp = l_odirp;
            else
                prev_odirp = nullptr;
            if (prev_dir_ind >= 0)
                l_odirp = std::get_if<inmem_dir_t>(
                        &l_odirp->sdirs_sp->sdir_v[prev_dir_ind].derived);
        } else if (depth < prev_depth) {   // should never occur on first iter
            if (depth == (prev_depth - 1) && prev_odirp) {
                l_odirp = prev_odirp;   // short cut if backing up one
                prev_odirp = nullptr;
            } else {
                bool cont_recurse { false };
                prev_odirp = l_odirp;
                const std::vector<sstring> vs
                        { split_path(par_pt, src_pt, op, ec) };
                if (ec) {
                    pr3ser(par_pt, "split_path() failed", ec);
                    break;
                }
                // when depth reduces > 1, don't trust pointers any more
                // l_odirp = op->mutp->cache_rt_dirp;
                l_odirp = odirp;
                const auto it_end { l_odirp->sdirs_sp->sdir_fn_ind_m.end() };

                for (const auto & s : vs) {
                    const auto it
                        { l_odirp->sdirs_sp->sdir_fn_ind_m.find(s) };

                    if (it == it_end) {
                        pr4ser(par_pt, s, "unable to find that sub-path");
                        cont_recurse = true;
                        l_odirp = prev_odirp;
                        break;
                    }
                    inmem_t * childp
                        { &l_odirp->sdirs_sp->sdir_v[it->second] };
                    l_odirp = std::get_if<inmem_dir_t>(&childp->derived);
                    if (l_odirp == nullptr) {
                        pr4ser(par_pt, s,
                               "unable to find that sub-directory");
                        cont_recurse = true;
                        l_odirp = prev_odirp;
                        break;
                    }
                }
                if (cont_recurse) {
                    ++q->num_error;
                    continue;
                } else
                    prev_odirp = nullptr;
            }
        }

        ++q->num_node;
        // if (q->num_node >= 240000)
            // break;
        if (verbose > 6)
            pr3ser(pt, "about to scan this source entry");
        if (depth > q->max_depth)
            q->max_depth = depth;

        struct stat a_stat;

        if (lstat(pt.c_str(), &a_stat) < 0) {
            ec.assign(errno, std::system_category());
            pr3ser(src_pt, "lstat() failed", ec);
            ++q->num_error;
            continue;
        }
        a_shstat.st_dev = a_stat.st_dev;
        a_shstat.st_mode = a_stat.st_mode;
        // memcpy(&a_inm.shstat, &a_stat, short_stat_cp_sz);
        fs::file_type targ_ftype { fs::file_type::none };

        const bool me_hidden { ((! pt.empty()) && (filename[0] == '.')) };
        if (possible_exclude) {
            exclude_entry = std::ranges::binary_search(op->exclude_v, pt);
            if (exclude_entry)
                ++q->num_excluded;
            if (exclude_entry && (verbose > 3))
                pr3ser(pt, "matched for exclusion");
        }
        if (possible_deref && (s_sym_ftype == fs::file_type::symlink)) {
            const auto & pt_s { pt.string() };

            deref_entry = false;
            auto ind = binary_search_find_index(op->mutp->deref_v, pt_s);
            if (ind >= 0) {
                deref_entry = true;
                // make sure this match is not rematched by removing it!
                // auto it = std::remove(op->mutp->deref_v.begin(),
                                      // op->mutp->deref_v.end(), pt_s);
                op->mutp->deref_v.erase(op->mutp->deref_v.begin() + ind);
                if (op->mutp->deref_v.empty())
                    possible_deref = false;
                ++q->num_derefed;
                if (verbose > 3)
                    pr3ser(pt, "matched for dereference");
            }
        }
        if (op->want_stats > 0)
            update_stats(s_sym_ftype, targ_ftype, me_hidden, op);
        if (l_isdir) {
            if (exclude_entry) {
                itr.disable_recursion_pending();
                exclude_entry = false;
                continue;
            }
            if (op->max_depth_active && (depth >= op->max_depth)) {
                if (verbose > 2)
                    scerr << "Source at max_depth and this is a directory: "
                          << pt << ", don't enter\n";
                itr.disable_recursion_pending();
                continue;
            }
            if (! op->no_xdev) { // double negative ...
                if (a_stat.st_dev != op->mutp->starting_fs_inst) {
                    // do not visit this sub-branch: different fs instance
                    if (verbose > 1)
                        scerr << "Source trying to leave this fs instance "
                                 "at: " << pt << "\n";
                    itr.disable_recursion_pending();
                    ++q->num_oth_fs_skipped;
                    /* create this directory as possible mount point */
                }
            }
        } else if (exclude_entry) {
            exclude_entry = false;
            continue;
        }
        if ((! op->clone_hidden) && me_hidden) {
            ++q->num_hidden_skipped;
            if (s_sym_ftype == fs::file_type::directory)
                itr.disable_recursion_pending();
            continue;
        }

        switch (s_sym_ftype) {
        using enum fs::file_type;

        case symlink:
            {
                inmem_symlink_t a_sym;

                const auto target_pt = fs::read_symlink(pt, ec);
                if (ec) {
                    if (verbose > 2)
                        pr3ser(pt, "read_symlink() failed", ec);
                    auto err = ec.value();
                    if (err == EACCES)
                        ++q->num_reg_s_eacces;
                    else if (err == EPERM)
                        ++q->num_reg_s_eperm;
                    else
                        ++q->num_sym_s_dangle;
                    break;
                }
                a_sym.target = target_pt;
                if (deref_entry) {   /* symlink becomes directory */
                    deref_entry = false;
                    const fs::path join_pt { pt.parent_path() / target_pt };
                    const fs::path canon_targ_s_pt
                                        { fs::canonical(join_pt, ec) };
                    if (ec) {
                        if (verbose > 0) {
                            pr3ser(canon_targ_s_pt, "canonical() failed", ec);
                            pr3ser(pt, "<< symlink probably dangling");
                        }
                        ++q->num_sym_s_dangle;
                        break;
                    }
                    if (! path_contains_canon(op->source_pt,
                                              canon_targ_s_pt)) {
                        ++q->num_follow_sym_outside;
                        if (verbose > 0)
                            pr3ser(canon_targ_s_pt,
                                   "<< outside, fall back to symlink");
                        goto back_to_symlink;
                    }
                    const auto targ_s_ft
                                { fs::status(canon_targ_s_pt, ec).type() };
                    if (ec) {
                        if (verbose > 0)
                            pr3ser(canon_targ_s_pt, "fs::status() failed",
                                   ec);
                        ++q->num_error;
                        break;
                    }
                    if (targ_s_ft == fs::file_type::directory) {
                        prev_odirp = l_odirp;
                        inmem_dir_t a_dir;
                        a_dir.par_pt_s = par_pt.string();
                        a_dir.depth = depth;
                        auto ind = l_odirp->add_to_sdir_v(a_dir, filename,
                                                          a_shstat);
                        if (ind >= 0) {
                            l_odirp = std::get_if<inmem_dir_t>(
                                        &l_odirp->sdirs_sp->sdir_v
                                                [ind].derived);
                            if (l_odirp) {
                                const auto ctspt
                                        { canon_targ_s_pt.string() + "\n" };
                                const char * ccp { ctspt.c_str() };
                                const uint8_t * bp
                                   { reinterpret_cast<const uint8_t *>(ccp) };
                                std::vector<uint8_t> v(bp, bp + ctspt.size());
                                inmem_regular_t a_reg;

                                a_reg.contents.swap(v);
                                a_reg.always_use_contents = true;
                                short_stat b_shstat { a_shstat };
                                b_shstat.st_mode &= ~stat_perm_mask;
                                b_shstat.st_mode |= def_file_perm;
                                l_odirp->add_to_sdir_v(a_reg, symlink_src_tgt,
                                                       b_shstat);
                                ++depth;
                                ecc = cache_src(depth, l_odirp,
                                                canon_targ_s_pt, op);
                                --depth;
                                if (ecc)
                                    return ecc;
                            }
                            l_odirp = prev_odirp;
                        }
#if 0
                        // create dir when src is symlink and follow active
                        // no problem if already exists
                        fs::create_directory(d_lnk_pt, ec);
                        if (ec) {
                            if (verbose > 0)
                                pr3ser(d_lnk_pt, "create_directory() failed",
                                       ec);
                            ++q->num_error;
                            break;
                        }
                        const fs::path deep_d_pt
                                { ongoing_destin_pt / d_lnk_pt };

                        // N.B. recursive call!
                        if (op->max_depth_active &&
                            (dc_depth >= op->max_depth)) {
                            scerr << "clone_work() hits max_depth: "
                                  << canon_targ_s_pt << " [" << dc_depth
                                  << "], don't enter\n";
                            ++q->num_error;
                            ecc.assign(ELOOP, std::system_category());
                            return ecc;  // propagate
                        }
#endif
// write to symlink_tgt_fn contents of target_pt.filename()
                    }
                    break;
                }
back_to_symlink:
                l_odirp->add_to_sdir_v(a_sym, filename, a_shstat);
            }
            break;
        case directory:
            {
                inmem_dir_t a_dir;
                a_dir.par_pt_s = par_pt.string();
                a_dir.depth = depth;
                prev_dir_ind = l_odirp->add_to_sdir_v(a_dir, filename,
                                                      a_shstat);
            }
            break;
        case block:
            {
                inmem_device_t a_dev;
                a_dev.is_block_dev = true;
                a_dev.st_rdev = a_stat.st_rdev;
                l_odirp->add_to_sdir_v(a_dev, filename, a_shstat);
            }
            break;
        case character:
            {
                inmem_device_t a_dev;
                // a_dev.is_block_dev = false;
                a_dev.st_rdev = a_stat.st_rdev;
                l_odirp->add_to_sdir_v(a_dev, filename, a_shstat);
            }
            break;
        case fifo:
            if (verbose > 0)
                pr3ser(pt, "<< cloning file type: socket not supported");
            break;              // skip this file type
        case socket:
            if (verbose > 0)
                pr3ser(pt, "<< cloning file type: socket not supported");
            break;              // skip this file type
        case regular:
            {
                inmem_regular_t a_reg;
                size_t ind { l_odirp->add_to_sdir_v(a_reg, filename,
                                                    a_shstat) };
                if (op->cache_op_num > 1) {
                    auto iregp { std::get_if<inmem_regular_t>
                                (&l_odirp->sdirs_sp->sdir_v[ind].derived) };

                    if (iregp) {
                        if (int res { xfr_reg_file2inmem(pt, *iregp, op) }) {
                            ec.assign(res, std::system_category());
                            if (verbose > 3) {
                                pr3ser(pt, "xfr_reg_file2inmem() "
                                       "failed", ec);
                                ++q->num_error;
                            }
                        } else if (verbose > 5)
                            pr3ser(pt, "xfr_reg_file2inmem() ok");
                    }
                }
            }
            break;
        default:
            {
                inmem_regular_t a_oth;
                l_odirp->add_to_sdir_v(a_oth, filename, a_shstat);
            }
            break;
        }

    }
    if (ecc) {
        ++q->num_scan_failed;
        pr3ser(prev_rdi_pt,
               "<< previous, recursive_directory_iterator() failed", ecc);
    }
    return ecc;
}

static size_t
count_cache(const inmem_dir_t * odirp, bool recurse, const struct opts_t * op)
{
    size_t sz, k;

    if (odirp == nullptr)
        return 0;
    sz = odirp->sdirs_sp->sdir_v.size();
    if (! recurse) {
#if 0
        for (const auto & sub : odirp->sdirs_sp->sdir_v) {
            scerr << "  filename: " << sub.filename << "\n";
        }
#endif
        return sz;
    }
    for (k = 0; const auto & sub : odirp->sdirs_sp->sdir_v) {
        const auto * cdirp { std::get_if<inmem_dir_t>(&sub.derived) };

        ++k;
        if (cdirp)
            k += count_cache(cdirp, recurse, op);
    }
    return k;
}

static void
depth_count_cache(const inmem_dir_t * odirp, const struct opts_t * op,
                  std::vector<size_t> & ra, int depth = -1)
{
    size_t sz, v_sz;
    size_t d { static_cast<size_t>(depth + 1) };  // depth starts at -1

    if (odirp == nullptr)
        return;
    sz = odirp->sdirs_sp->sdir_v.size();
    v_sz = ra.size();
    if (d >= v_sz)
        ra.push_back(sz);
    else
        ra[d] += sz;
    for (const auto & sub : odirp->sdirs_sp->sdir_v) {
        if (const auto * cdirp { std::get_if<inmem_dir_t>(&sub.derived) }) {
            depth_count_cache(cdirp, op, ra, cdirp->depth);
        }
#if 0
 else if (const auto * cregp = std::get_if<inmem_regular_t>(&sub.derived)) {
            const auto * pinmp = cregp->get_basep();
            pinmp->debug();
        }
#endif
    }
}

static void
show_cache_not_dir(const inmem_t & a_nod,
                   [[maybe_unused]] const struct opts_t * op)
{
    if (std::get_if<inmem_other_t>(&a_nod.derived)) {
        scerr << "  other filename: " << a_nod.filename << "\n";
    } else if (const auto * csymp {
               std::get_if<inmem_symlink_t>(&a_nod.derived) }) {
        scerr << "  symlink link name: " << a_nod.filename
              << "  target filename: " << csymp->target.string() << "\n";
    } else if (std::get_if<inmem_regular_t>(&a_nod.derived)) {
        if (verbose > 4)
            fprintf(stderr, "%s: &a_nod=%p\n", __func__, (void *)&a_nod);
        scerr << "  regular filename: " << a_nod.filename << "\n";
    } else if (const auto * cdevp {
               std::get_if<inmem_device_t>(&a_nod.derived) }) {
        if (cdevp->is_block_dev)
            scerr << "  block device filename: " << a_nod.filename << "\n";
        else
            scerr << "  char device filename: " << a_nod.filename << "\n";
    } else
        scerr << "  unknown filename: " << a_nod.filename << "\n";
    if (verbose > 3)
        fprintf(stderr, "    &inmem_t: %p\n", (void *)&a_nod);
}

static void
show_cache(const inmem_t & a_nod, bool recurse, const struct opts_t * op)
{
    const inmem_dir_t * dirp { std::get_if<inmem_dir_t>(&a_nod.derived) };

    if (dirp == nullptr) {
        show_cache_not_dir(a_nod, op);
        return;
    }
    scerr << "  directory: " << dirp->par_pt_s << '/'
          << a_nod.filename << ", depth=" << dirp->depth << "\n";
    if (verbose > 3)
        fprintf(stderr, "    &inmem_t: %p\n", (void *)&a_nod);

    for (const auto & subd : dirp->sdirs_sp->sdir_v) {
        const auto * cdirp { std::get_if<inmem_dir_t>(&subd.derived) };

        if (recurse) {
            if (cdirp)
                show_cache(subd, recurse, op);
            else {
                scerr << __func__ << "\n";
                subd.debug();
                if (verbose > 3)
                    fprintf(stderr, "    &inmem_t: %p\n", (void *)&subd);
            }
        } else
            show_cache_not_dir(subd, op);
    }
}

// Transforms a source path to the corresponding destination path
static sstring
tranform_src_pt2dst(const sstring & src_pt, const struct opts_t * op)
{
    if (src_pt.size() < op->mutp->starting_src_sz)
        return "";
    return op->destination_pt.string() +
           sstring(src_pt.begin() + op->mutp->starting_src_sz, src_pt.end());
}

static std::error_code
unroll_cache_not_dir(const sstring & s_pt_s, const sstring & d_pt_s,
                     const inmem_t & a_nod, const struct opts_t * op)
{
    int res { };
    std::error_code ec { };
    struct stats_t * q { &op->mutp->stats };

    if (std::get_if<inmem_other_t>(&a_nod.derived)) {
        scerr << "  other filename: " << a_nod.filename << "\n";
    } else if (const auto * csymp {
               std::get_if<inmem_symlink_t>(&a_nod.derived) }) {
        fs::create_symlink(csymp->target, d_pt_s, ec);
        if (ec) {
            if (verbose > 1)
                pr4ser(csymp->target, d_pt_s, "[targ, link] "
                       "create_symlink() failed", ec);
            ++q->num_error;
        } else {
            ++q->num_sym_d_success;
            if (verbose > 4)
                pr4ser(csymp->target, d_pt_s, "[targ, link] "
                       "create_symlink() ok");
        }
    } else if (const auto * cdevp
               { std::get_if<inmem_device_t>(&a_nod.derived) }) {
        res = xfr_dev_inmem2file(*cdevp, d_pt_s, op);
        if (res)
            ec.assign(res, std::system_category());
//        if (res && (verbose > 4))
if (res)
            fprintf(stderr, "%s: failed to write dev file: %s, "
                    "res=%d\n", __func__, d_pt_s.c_str(), res);
// yyyyyyyyy
    } else if (const auto * cregp
                 { std::get_if<inmem_regular_t>(&a_nod.derived) } ) {
        if (cregp->always_use_contents || (op->cache_op_num > 1))
            res = xfr_reg_inmem2file(*cregp, d_pt_s, op);
        else if (op->cache_op_num == 1)
            res = xfr_reg_file2file(s_pt_s, d_pt_s, op);
        if (res)
            ec.assign(res, std::system_category());
        if (res && (verbose > 4))
            fprintf(stderr, "%s: failed to write dst regular file: %s, "
                    "res=%d\n", __func__, d_pt_s.c_str(), res);
    } else
        scerr << "  unknown filename: " << a_nod.filename << "\n";
    if (verbose > 3)
        fprintf(stderr, "    &inmem_t: %p\n", (void *)&a_nod);
    return ec;
}

static std::error_code
unroll_cache_is_dir(const sstring & dst_pt_s, const inmem_dir_t * dirp,
                    const struct opts_t * op)
{
    std::error_code ec { };
    struct stats_t * q { &op->mutp->stats };

    // just go with user's default permissions for this directory
    if (! fs::create_directory(dst_pt_s, ec)) {
        if (ec) {
            ++q->num_error;
            if (verbose > 1)
                pr4ser(dst_pt_s, std::to_string(dirp->depth),
                       "create_directory() failed", ec);
        } else if (dst_pt_s != op->destination_pt) {
            ++q->num_dir_d_exists;
            if (verbose > 2)
                pr4ser(dst_pt_s, std::to_string(dirp->depth),
                       "[dir, depth] exists so create_directory() ignored");
        }
    } else
        ++q->num_dir_d_success;
    return ec;
}

// Unroll cache into the destination.
static std::error_code
unroll_cache(const inmem_t & a_nod, const sstring & s_par_pt_s, bool recurse,
             const struct opts_t * op)
{
    std::error_code ec { };

    sstring src_dir_pt_s { s_par_pt_s + '/' + a_nod.filename };
    if (a_nod.filename.empty())
        src_dir_pt_s = s_par_pt_s;      // don't want trailing slash
    sstring dst_dir_pt_s { tranform_src_pt2dst(src_dir_pt_s, op) };
    const inmem_dir_t * dirp { std::get_if<inmem_dir_t>(&a_nod.derived) };

    if (dirp == nullptr)
        return unroll_cache_not_dir(src_dir_pt_s, dst_dir_pt_s, a_nod, op);

    ec = unroll_cache_is_dir(dst_dir_pt_s, dirp, op);
    if (ec)
        return ec;

    for (const auto & subd : dirp->sdirs_sp->sdir_v) {
        const auto * cdirp { std::get_if<inmem_dir_t>(&subd.derived) };

        if (cdirp && recurse) {
            ec = unroll_cache(subd, src_dir_pt_s, recurse, op);
            if (ec)
                break;
        } else {
            const auto & fn {subd.filename };
            sstring s_d_pt_s { src_dir_pt_s + '/' + fn };
            sstring d_d_pt_s { dst_dir_pt_s + '/' + fn };

            if (cdirp)
                ec = unroll_cache_is_dir(d_d_pt_s, cdirp, op);
            else
                ec = unroll_cache_not_dir(s_d_pt_s, d_d_pt_s, subd, op);
            if (ec)
                ec.clear();
        }
    }
    return ec;
}

static std::error_code
do_clone(const struct opts_t * op)
{
    struct stat root_stat;
    std::error_code ec { };
    auto start { chron::steady_clock::now() };

    if (stat(op->source_pt.c_str(), &root_stat) < 0) {
        ec.assign(errno, std::system_category());
        return ec;
    }
    op->mutp->starting_fs_inst = root_stat.st_dev;
    ec = clone_work(0, op->source_pt, op->destination_pt, op);
    if (ec)
        pr3ser(op->source_pt, "<< src; problem with clone_work()", ec);

    auto end { chron::steady_clock::now() };
    auto ms { chron::duration_cast<chron::milliseconds>(end - start).count() };
    auto secs { ms / 1000 };
    auto ms_remainder { ms % 1000 };
    char b[32];
    snprintf(b, sizeof(b), "%d.%03d", static_cast<int>(secs),
             static_cast<int>(ms_remainder));
    std:: cout << "Elapsed time: " << b << " seconds\n";

    if (op->want_stats > 0)
        show_stats(op);
    return ec;
}

static std::error_code
do_cache(inmem_t & src_rt_cache, const struct opts_t * op)
{
    struct stat root_stat;
    std::error_code ec { };
    auto start { chron::steady_clock::now() };

    if (stat(op->source_pt.c_str(), &root_stat) < 0) {
        ec.assign(errno, std::system_category());
        return ec;
    }
    op->mutp->starting_fs_inst = root_stat.st_dev;
    src_rt_cache.shstat.st_dev = root_stat.st_dev;
    src_rt_cache.shstat.st_mode = root_stat.st_mode;
    // src_rt_cache.filename = op->source_pt.filename();
    // src_rt_cache.odirp = nullptr;
#if 0
    inmem_dir_t rt_dir { .sdir_v { }, .pt = op->source_pt, .depth = -1 } ;
#else
    // inmem_dir_t rt_dir;
    // rt_dir.pt_s = op->source_pt;
#endif
    // inm_var_t iv_obj { rt_dir };
    // src_rt_cache.derived.swap(iv_obj);
    // auto odirp = std::get_if<inmem_dir_t>(&src_rt_cache.derived);

    // memcpy(&root_shstat, &root_stat, short_stat_cp_sz);
    uint8_t * sbrk_p { static_cast<uint8_t *>(sbrk(0)) };
    auto * rt_dirp { std::get_if<inmem_dir_t>(&src_rt_cache.derived) };
    op->mutp->cache_rt_dirp = rt_dirp;    // mutable to hold root directory ptr
    ec = cache_src(0, rt_dirp, op->source_pt, op);
    if (ec)
        pr3ser(op->source_pt, "<< src; problem with cache_src()", ec);

    auto end { chron::steady_clock::now() };
    auto ms
        { chron::duration_cast<chron::milliseconds>(end - start).count() };
    auto secs { ms / 1000 };
    auto ms_remainder { ms % 1000 };
    char b[32];
    snprintf(b, sizeof(b), "%d.%03d", static_cast<int>(secs),
             static_cast<int>(ms_remainder));
    scerr << "Caching time: " << b << " seconds\n";

    ec = unroll_cache(src_rt_cache, op->source_pt.string(), true, op);
    if (ec) {
        if (verbose > 0)
            pr2ser("unroll_cache() failed", ec);
        ec.clear();
    }

    if (op->do_extra) {
        long tree_sz { static_cast<const uint8_t *>(sbrk(0)) - sbrk_p };
        scerr << "Tree size: " << tree_sz << " bytes\n";

        size_t counted_nodes { 1 + count_cache(rt_dirp, false, op) };
        scerr << "Tree counted nodes: " << counted_nodes << " at top level\n";

        counted_nodes = 1 + count_cache(rt_dirp, true, op);
        scerr << "Tree counted nodes: " << counted_nodes << " recursive\n";

        std::vector<size_t> ra;
        depth_count_cache(rt_dirp, op, ra);
        scerr << "Depth count cache:\n";
        for (int d { 0 }; auto k : ra) {
            scerr << "  " << d << ": " << k << "\n";
            ++d;
        }
    }

    if (! op->no_destin) {
        // need to unroll cache into destination here
    }

    end = chron::steady_clock::now();
    ms = chron::duration_cast<chron::milliseconds>(end - start).count();
    secs = ms / 1000;
    ms_remainder = ms % 1000;
    snprintf(b, sizeof(b), "%d.%03d", static_cast<int>(secs),
             static_cast<int>(ms_remainder));
    scerr << "Caching plus unrolling time: " << b << " seconds\n";

    if (op->want_stats > 0)
        show_stats(op);
    return ec;
}


int
main(int argc, char * argv[])
{
    bool ex_glob_seen { false };
    int res { };
    int glob_opt;
    std::error_code ec { };
    const char * destination_clone_start { nullptr };
    const char * source_clone_start { nullptr };
    glob_t ex_paths { };
    struct opts_t opts { };
    struct opts_t * op = &opts;
    struct mut_opts_t mut_opts { };

    op->mutp = &mut_opts;
    struct stats_t * q { &op->mutp->stats };
    op->reglen = def_reglen;
    while (1) {
        int option_index { 0 };
        int c { getopt_long(argc, argv, "cd:De:hHm:Nr:R:s:SvVw:x",
                            long_options, &option_index) };
        if (c == -1)
            break;

        switch (c) {
        case 'c':
            ++op->cache_op_num;
            break;
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
            res = glob(optarg, glob_opt, nullptr, &ex_paths);
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
        case 'R':
            op->deref_given = true;
            op->mutp->deref_v.push_back(optarg);
            break;
        case 's':
            source_clone_start = optarg;
            op->source_given = true;
            break;
        case 'S':
            ++op->want_stats;
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
        case 'x':
            op->do_extra = true;
            break;
        default:
            scerr << "unrecognised option code 0x" << c << "\n";
            usage();
            return 1;
        }
    }
    if (optind < argc) {
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
    op->mutp->starting_src_sz = op->source_pt.string().size();

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
        fs::path d_pt { d_str };

        if (d_pt.filename().empty())
            d_pt = d_pt.parent_path(); // to handle trailing / as in /tmp/sys/
        if (fs::exists(d_pt, ec)) {
            if (fs::is_directory(d_pt, ec)) {
                op->destination_pt = fs::canonical(d_pt, ec);
                if (ec) {
                    pr3ser(d_pt, "canonical() failed", ec);
                    return 1;
                }
            } else {
                pr3ser(d_pt, "is not a directory", ec);
                return 1;
            }
        } else {
            fs::path d_p_pt { d_pt.parent_path() };

            if (fs::exists(d_p_pt, ec) && fs::is_directory(d_p_pt, ec)) {
                // create destination directory at DPATH
                // no problem if already exists
                fs::create_directory(d_pt, ec);
                if (ec) {
                    pr3ser(d_pt, "create_directory() failed", ec);
                    return 1;
                }
                op->destination_pt = fs::canonical(d_pt, ec);
                op->destin_all_new = true;
                if (ec) {
                    pr3ser(d_pt, "canonical() failed", ec);
                    return 1;
                }
            } else {
                pr3ser(d_p_pt, "needs to be an existing directory", ec);
                return 1;
            }
        }
        if (verbose > 5)
            fprintf(stderr, "op->source_pt: %s , op->destination_pt: %s\n",
                    op->source_pt.string().c_str(),
                    op->destination_pt.string().c_str());
        if (op->source_pt == op->destination_pt) {
            pr4ser(op->source_pt, op->destination_pt,
                   "source and destination seem to be the same. That is not "
                   "practical");
            return 1;
        }
    } else {
        if (op->destination_given) {
            pr2ser("the --destination= and the --no-dst options contradict, "
                   "please pick one");
            return 1;
        }
        if (! op->mutp->deref_v.empty())
            scerr << "Warning: --dereference=SPTSYM options ignored when "
                     "--no-destin option given\n";
    }

    if (op->reglen > def_reglen) {
        op->reg_buff_sp = std::make_shared<uint8_t []>((size_t)op->reglen, 0);
        if (! op->reg_buff_sp) {
            fprintf(stderr, "Unable to allocate %d bytes on heap, use "
                    "default [%d bytes] instead\n", op->reglen, def_reglen);
            op->reglen = def_reglen;
        }
    }

    auto ex_sz = op->exclude_v.size();  // will be zero here
    bool destin_excluded = false;

    if (ex_glob_seen) {
        bool first_reported = false;

        for(size_t k { }; k < ex_paths.gl_pathc; ++k) {
            fs::path ex_pt { ex_paths.gl_pathv[k] };
            bool is_absol = ex_pt.is_absolute();

            if (! is_absol) {       // then make absolute
                auto cur_pt { fs::current_path(ec) };
                if (ec) {
                    pr3ser(ex_pt, "unable to get current path, ignored", ec);
                    continue;
                }
                ex_pt = cur_pt / ex_pt;
            }
            fs::path c_ex_pt { fs::canonical(ex_pt, ec) };
            if (ec) {
                if (verbose > 1)
                    pr3ser(ex_pt, "exclude path rejected", ec);
            } else {
                if (path_contains_canon(op->source_pt, c_ex_pt)) {
                    op->exclude_v.push_back(ex_pt); // N.B. non-canonical
                    if (verbose > 3)
                        pr3ser(ex_pt, "accepted canonical exclude path");
                    if (c_ex_pt == op->destination_pt)
                        destin_excluded = true;
                } else if ((! first_reported) || (verbose > 0)) {
                    pr3ser(ex_pt,
                           "ignored as not contained in exclude source");
                    first_reported = true;
                }
            }
        }
        globfree(&ex_paths);
        ex_sz = op->exclude_v.size();

        if (verbose > 0)
            scerr << "--exclude= argument matched " << ex_sz << " files\n";
        if (ex_sz > 1) {
            if (! std::ranges::is_sorted(op->exclude_v)) {
                if (verbose > 2)
                    pr2ser("need to sort exclude vector");
                std::ranges::sort(op->exclude_v);
            }
            const auto ret { std::ranges::unique(op->exclude_v) };
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
        if (! op->mutp->deref_v.empty()) {
            static const char * rm_marker { "zzzzzzzz" };

            for (auto & sl : op->mutp->deref_v) {
                fs::path pt { sl };
                bool is_absol = pt.is_absolute();

                if (! is_absol) {       // then make absolute
                    auto cur_pt { fs::current_path(ec) };
                    if (ec) {
                        pr3ser(sl, "unable to get current path, ignored", ec);
                        sl = rm_marker;    // unlikely name to remove later
                        continue;
                    }
                    pt = cur_pt / pt;
                }
                auto lnk_name { pt.filename() };
                // want parent path in canonical form
                auto parent_pt = pt.parent_path();
                auto npath { parent_pt / lnk_name };
                if (path_contains_canon(op->source_pt, npath) &&
                    (op->source_pt != sl)) {
                    auto ftyp { fs::symlink_status(npath, ec).type() };

                    if (ec) {
                        pr3ser(npath, "unable to 'stat' that file, ignored",
                               ec);
                        sl = rm_marker;
                        continue;
                    } else if (ftyp != fs::file_type::symlink) {
                        pr3ser(npath, "is not a symlink, ignored", ec);
                        sl = rm_marker;
                        continue;
                    } else {
                        sl = npath.string();
                        if (verbose > 5)
                            pr3ser(npath,
                                   "is a candidate symlink, will deep copy");
                    }
                } else {
                    pr3ser(npath, "expected to be under SPATH");
                    sl = rm_marker;
                    continue;
                }

            }
            // compact vector by removing "zzzzzzzz" entries, trim with
            // 'erase()' then sort so can do binary search later
            auto it = std::remove(op->mutp->deref_v.begin(),
                                  op->mutp->deref_v.end(), rm_marker);
            op->mutp->deref_v.erase(it, op->mutp->deref_v.end());
            if (op->mutp->deref_v.size() > 1) {
                std::ranges::sort(op->mutp->deref_v);
                // remove duplicates
                const auto ret { std::ranges::unique(op->mutp->deref_v) };
                op->mutp->deref_v.erase(ret.begin(), ret.end());
            }
        }
    }

    if (op->cache_op_num > 0) {
        inmem_dir_t s_inm_rt;
        s_inm_rt.par_pt_s = op->source_pt;
        s_inm_rt.depth = -1;
        inm_var_t iv_obj { s_inm_rt };
        // only SPATH root has empty filename (2nd argument in following)
        inmem_t src_rt_cache(iv_obj, "", short_stat(), 0);

        if (verbose > 4) {
            src_rt_cache.debug("empty cache tree");
            show_cache(src_rt_cache, true, op);
        }
        ec = do_cache(src_rt_cache, op);  // src ==> cache ==> destination
        if (ec)
            res = 1;
        if (verbose > 4) {
            scerr << ">>> final cache tree:\n";
            show_cache(src_rt_cache, true, op);
            src_rt_cache.debug(">>> final debug of root directory");
        }
    } else {
        ec = do_clone(op);      // Single pass
        if (ec) {
            res = 1;
            pr2ser("do_clone() failed");
        }
    }
    if ((op->want_stats == 0) && (op->destination_given == false) &&
        (op->source_given == false) && (op->no_destin == false) && (res = 0))
        scout << "Successfully cloned " << sysfs_root << " to "
              << def_destin_root << "\n";

    if ((! op->want_stats) && (q->num_scan_failed > 0)) {
        pr2ser("Warning: scan of source truncated, may need to re-run");
        res = 1;
    }
    return res;
}
