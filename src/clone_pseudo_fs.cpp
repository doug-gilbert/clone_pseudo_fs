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

static const char * const version_str = "0.90 20231220 [svn: r28]";

#include <iostream>
#include <fstream>
#include <cstdint>
#include <filesystem>
#include <vector>
#include <map>
#include <bit>
#include <span>
#include <ranges>
#include <variant>
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

// Bill Weinman's header library for C++20 follows. Expect to drop if moved
// to >= C++23 and then s/bw::print/std::print/
#include "bwprint.hpp"

static const unsigned int def_reglen { 256 };
static const int reg_re_read_sz { 1024 };

namespace fs = std::filesystem;
namespace chron = std::chrono;

using sstring = std::string;

// The "inmem*" structs are associated with using the '--cache' option.
// Files are divided into 6 categories: regular, directory, symlink,
// device (char or block), fifo_or_socket and other. Data in common is
// placed in the inmem_base_t struct. A std::variant called inm_var_t
// holds one of these variants. Then the inmem_t struct is derived from
// that variant. inmem_t holds no data directory but it has constructors
// to make instances that contain one of the six categories. The simplest
// category is "other" and its is named first in the variant and should
// not appear in the "unroll"-ed tree.

// Forward declarations
// struct inmem_t;
struct inmem_subdirs_t;
struct inmem_dir_t;

struct opts_t;

static auto & scout { std::cout };
static auto & scerr { std::cerr };
static fs::path prev_rdi_pt;

static int cpf_verbose;  // in 'struct opts_t' and file scope here ..

#ifdef DEBUG    // use './configure --enable-debug' to get this set
// the DEBUG define is set when './configure --enable-debug' is used
// sloc is an abbreviation for "source_location" which is new in C++20
static bool want_sloc { };      // set if DEBUG and -vV not given
#endif

// from the https://en.cppreference.com description of std::variant
// "helper type for the visitor #4". Not used currently.
template<class... Ts>
struct overloaded : Ts... { using Ts::operator()...; };

template<typename... Args>
    constexpr void pr_err(int vb_ge, const std::string_view str_fmt,
                          Args&&... args) noexcept {
        if (vb_ge >= cpf_verbose)       // vb_ge==-1 always prints
            return;
        try {
        fputs(BWP_FMTNS::vformat(str_fmt,
                                 BWP_FMTNS::make_format_args(args...)).c_str(),
                                 stderr);
        }
        catch (...) {
            scerr << "pr_err: vformat: threw exception, str_fmt: "
                  << str_fmt << "\n";
        }
}

// to print to stdout use bw::print(str_fmt, ....);

template<typename... Args>
    constexpr std::string fmt_to_str(const std::string_view str_fmt,
                                     Args&&... args) noexcept {
        try {
        return BWP_FMTNS::vformat(str_fmt,
                                  BWP_FMTNS::make_format_args(args...));
        }
        catch (...) {
            scerr << "fmt_to_str: vformat: threw exception, str_fmt: "
                  << str_fmt << "\n";
        }
}


// This enum is used with the inmem_base_t::prune_mask and the reason is
// symlinks again. When the target of a symlink lands on a node that is
// within the cache and is marked prune_all_below then there is nothing more
// to do. If that node is marked prune_up_chain then (prune_all_below)
// marking is required from that node and below.
enum prune_mask_e : uint8_t {
    prune_none = 0,             // need this case for default initialization
    prune_exact = 1,            // as identified by a --prune= option
    prune_all_below = 2,        // from exact match and above
    prune_up_chain = 4,         // from exact match to the root, or
                                // from a symlink target to the root
};

struct short_stat {     // stripped down version of 'struct stat'
    dev_t   st_dev;     // ID of device containing file
    mode_t  st_mode;    // File type and mode
};

struct inmem_base_t {
    inmem_base_t() = default;

    inmem_base_t(const sstring & fn, const short_stat & a_shstat,
                 unsigned int a_par_dir_ind) noexcept
                : filename(fn), shstat(a_shstat),
                  par_dir_ind(a_par_dir_ind) { }

    // Note that filename is just a filename, it does not have a path. All
    // nodes (regular, directory, symlink and special files) have
    // non-empty filenames.
    sstring filename { };  // link name if symlink

    struct short_stat shstat { };

    // This value is this object's index in its parent's
    // inmem_dir_t::sdirs_sp->sdir_v vector. Holding parent pointers is
    // dangerous as the parent itself is a vector member which can be
    // relocated when a new parent sibling is added. Usually when adding
    // new nodes we already know the parent's address. We assume the
    // following: when adding a node at depth <d>, no vectors holding
    // lesser depths (i.e. depths [0 ... (<d> - 1)] ) are modified.
    // Also new vector elements are always added to the end of the vector
    // so existing indexes remain valid.
    unsigned int  par_dir_ind { };      // save a few bytes

    // damn stupid compiler, prune_mask_e is defined as a unit8_t enum
    mutable uint8_t prune_mask { }; // used when op->prune_given is true

    uint8_t is_root { };

    void debug_base(const sstring & intro = "") const noexcept;
};

// If instances of this class appear in the output then either an unexpected
// directory element object has been found or there is a logic error.
struct inmem_other_t : inmem_base_t {
    inmem_other_t() = default;  // important for std::variant
    inmem_other_t(const sstring & filename_, const short_stat & a_shstat)
                noexcept : inmem_base_t(filename_, a_shstat, 0) { }

    // no data members

    sstring get_filename() const noexcept { return filename; }

    void debug(const sstring & intro = "") const noexcept;
};

struct inmem_symlink_t : inmem_base_t {
    inmem_symlink_t() = default;
    inmem_symlink_t(const sstring & filename_, const short_stat & a_shstat)
                noexcept : inmem_base_t(filename_, a_shstat, 0) { }

    // typically a relative path against directory path symlink is found in
    fs::path target;

    // this will be the filename
    sstring get_filename() const noexcept { return filename; }

    void debug(const sstring & intro = "") const noexcept;
};

struct inmem_device_t : inmem_base_t { // block of char
    inmem_device_t() = default;
    inmem_device_t(const sstring & filename_, const short_stat & a_shstat)
                noexcept : inmem_base_t(filename_, a_shstat, 0) { }

    bool is_block_dev { false };

    dev_t st_rdev { };

    sstring get_filename() const { return filename; }

    void debug(const sstring & intro = "") const noexcept;
};

struct inmem_fifo_socket_t : inmem_base_t {
    // no data members (recognized but not cloned)

    sstring get_filename() const noexcept { return filename; }

    void debug(const sstring & intro = "") const noexcept;
};

struct inmem_regular_t : inmem_base_t {
    inmem_regular_t() = default;
    inmem_regular_t(const sstring & filename_, const short_stat & a_shstat)
                noexcept : inmem_base_t(filename_, a_shstat, 0) { }

    inmem_regular_t(const inmem_regular_t & oth) noexcept :
        inmem_base_t(oth), contents(oth.contents),
        read_found_nothing(oth.read_found_nothing),
        always_use_contents(oth.always_use_contents)
        { }

    inmem_regular_t(inmem_regular_t && oth) noexcept :
        inmem_base_t(oth), contents(oth.contents),
        read_found_nothing(oth.read_found_nothing),
        always_use_contents(oth.always_use_contents)
        { }

    std::vector<uint8_t> contents { };

    bool read_found_nothing { };

    bool always_use_contents { };  // set when src_symlink_tgt_path inserted

    sstring get_filename() const noexcept { return filename; }

    void debug(const sstring & intro = "") const noexcept;
};

// All members of this variant have default constructors so no need for
// std::monostate . inmem_other_t objects should be rare.
using inm_var_t = std::variant<inmem_other_t,
                               inmem_dir_t,
                               inmem_symlink_t,
                               inmem_device_t,
                               inmem_fifo_socket_t,
                               inmem_regular_t>;

struct inmem_dir_t : inmem_base_t {
    inmem_dir_t() noexcept;   // cannot use designated initializer if given

    inmem_dir_t(const sstring & filename_,
                const short_stat & a_shstat) noexcept;

    inmem_dir_t(const inmem_dir_t & oth) noexcept :
                inmem_base_t(oth),
                sdirs_sp(oth.sdirs_sp),
                par_pt_s(oth.par_pt_s), depth(oth.depth) { }
    inmem_dir_t(inmem_dir_t && oth) noexcept : inmem_base_t(oth),
                                      sdirs_sp(oth.sdirs_sp),
                                      par_pt_s(oth.par_pt_s),
                                      depth(oth.depth) { }

    ~inmem_dir_t() { }

    // >>> Pivotal member: smart pointer to object holding sub-directories
    std::shared_ptr<inmem_subdirs_t> sdirs_sp;

    // directory absolute path: par_pt_s + '/' + get_filename()
    sstring par_pt_s { };  // empty in one case: when root directory: '/'

    int depth { -3 };   // purposely invalid. SPATH root has depth=-1

    // these add_to_sdir_v() functions all return the index position in
    // the sdirs_sp->sdir_v[] vector into which an inmem_t object was
    // inserted. The inmem_t object is formed from the passed argument.
    size_t add_to_sdir_v(inmem_other_t & n_oth) noexcept;
    size_t add_to_sdir_v(inmem_dir_t & n_dir) noexcept;
    size_t add_to_sdir_v(inmem_symlink_t & n_sym) noexcept;
    size_t add_to_sdir_v(inmem_device_t & n_dev) noexcept;
    size_t add_to_sdir_v(inmem_fifo_socket_t & n_fs) noexcept;
    size_t add_to_sdir_v(inmem_regular_t & n_reg) noexcept;

    sstring get_filename() const noexcept { return filename; }

    inm_var_t * get_subd_ivp(size_t index) noexcept;

    void debug(const sstring & intro = "") const noexcept;
};

// <<< instances of this struct are stored in a in-memory tree that uses lots
// <<< of std::vector<inmem_t> objects.
struct inmem_t : inm_var_t {
    inmem_t() = default;

    inmem_t(const inmem_other_t & i_oth) noexcept : inm_var_t(i_oth) { }
    inmem_t(const inmem_dir_t & i_dir) noexcept : inm_var_t(i_dir) { }
    inmem_t(const inmem_symlink_t & i_symlink) noexcept
                : inm_var_t(i_symlink) { }
    inmem_t(const inmem_device_t & i_device) noexcept
                : inm_var_t(i_device) { }
    inmem_t(const inmem_fifo_socket_t & i_fifo_socket) noexcept
                : inm_var_t(i_fifo_socket) { }
    inmem_t(const inmem_regular_t & i_regular) noexcept
                : inm_var_t(i_regular) { }

    inmem_t(const inmem_t & oth) noexcept : inm_var_t(oth) { }

    inmem_t(inmem_t && oth) noexcept : inm_var_t(oth) { }

    inmem_base_t * get_basep() noexcept
                { return reinterpret_cast<inmem_base_t *>(this); }
    const inmem_base_t * get_basep() const noexcept
                { return reinterpret_cast<const inmem_base_t *>(this); }

    sstring get_filename() const noexcept;

    void debug(const sstring & intro = "") const noexcept;
};

struct inmem_subdirs_t {
    inmem_subdirs_t() = default;

    inmem_subdirs_t(size_t vec_init_sz) noexcept;

    // vector of a directory's contents including subdirectories
    std::vector<inmem_t> sdir_v;

    // Using pointers for parent directories is dangerous due to std::vector
    // re-allocating when it expands. Backing up 1 level (depth) seems safe,
    // so when backing up two or more levels, instead use the new path to
    // navigate down from the root. The following map speeds directory name
    // lookup for for that latter case.
    std::map<sstring, size_t> sdir_fn_ind_m;  // sdir filename to index

    void debug(const sstring & intro = "") const noexcept;
};

enum class inmem_var_e {
    var_other = 0,      // matching inmem_t::derived.index()
    var_dir,
    var_symlink,
    var_device,
    var_fifo_socket,
    var_regular,
};

// Use "node" for an instance of any file type
struct stats_t {
    unsigned int num_node;      /* accessed in the source scan (pass 1) */
    unsigned int num_dir;       /* directories that are not symlinks */
    unsigned int num_sym2dir;
    unsigned int num_sym2reg;
    unsigned int num_sym2sym;
    unsigned int num_sym2block;
    unsigned int num_sym2char;
    unsigned int num_sym_other;
    unsigned int num_symlink;
    unsigned int num_sym_s_eacces;
    unsigned int num_sym_s_eperm;
    unsigned int num_sym_s_enoent;
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
    unsigned int num_excl_fn;
    unsigned int num_derefed;
    unsigned int num_dir_d_success;
    unsigned int num_dir_d_exists;
    unsigned int num_dir_d_fail;
    unsigned int num_sym_d_success;
    unsigned int num_sym_d_dangle;
    unsigned int num_mknod_d_fail;
    unsigned int num_mknod_d_eacces;
    unsigned int num_mknod_d_eperm;
    unsigned int num_prune_exact;
    unsigned int num_pruned_node;
    unsigned int num_prune_sym_pt_err;
    unsigned int num_prune_sym_outside;
    unsigned int num_prune_err;
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
    unsigned int num_reg_from_cache_err;
    int max_depth;
};

struct mut_opts_t {
    bool prune_take_all { };    // for '--src=/sys --prune=/sys'
    bool clone_work_subseq { };
    bool cache_src_subseq { };
    size_t starting_src_sz { };
    dev_t starting_fs_inst { };
    inmem_dir_t * cache_rt_dirp { };
    struct stats_t stats { };
    // following two are sorted to enable binary search
    std::vector<sstring> deref_v;
    std::vector<sstring> prune_v;
    std::vector<sstring> glob_exclude_v;  // vector of canonical paths
};

struct opts_t {
    bool destination_given;
    bool deref_given;       // one or more --deref= options
    bool exclude_given;     // one or more --exclude= options
    bool excl_fn_given;     // one or more --excl_fn= options
    bool prune_given;       // one or more --prune= options
    bool source_given;      // once source is given, won't default destination
    bool verbose_given;
    bool version_given;
    bool wait_given;        // --wait=0 seems sufficient to cope waiting reads
    bool destin_all_new;    // checks for existing can be skipped if all_new
    bool max_depth_active;  // for depth: 0 means one level below source_pt
    bool no_destin;         // -D
    bool clone_hidden;      // copy files starting with '.' (default: don't)
    bool no_xdev;           // -N : 'find(1) -xdev' means don't scan outside
                            // original fs so no_xdev is a double negative.
                            // (default for this utility: don't scan outside)
    unsigned int reglen;    // maximum bytes read from regular file
    unsigned int wait_ms;   // to cope with waiting reads (e.g. /proc/kmsg)
    int cache_op_num;       // -c : cache SPATH to meomory then ...
    int do_extra;           // do more checking and scans
    int max_depth;          // one less than given on command line
    int want_stats;         // should this be the default ? ?
    int verbose;            // make file scope
    const char * dst_cli;   // destination given on command line
    const char * src_cli;   // source given on command line
    struct mut_opts_t * mutp;
    fs::path source_pt;         // src root directory in absolute form
    fs::path destination_pt;    // (will be) a directory in canonical form
    std::shared_ptr<uint8_t[]> reg_buff_sp;
    std::vector<sstring> cl_exclude_v;  // command line --exclude arguments
    std::vector<sstring> excl_fn_v;  // vector of exclude filenames
};

static const struct option long_options[] {
    {"cache", no_argument, 0, 'c'},
    {"dereference", required_argument, 0, 'R'},
    {"deref", required_argument, 0, 'R'},
    {"destination", required_argument, 0, 'd'},
    {"dst", required_argument, 0, 'd'},
    {"exclude", required_argument, 0, 'e'},
    {"excl-fn", required_argument, 0, 'E'},
    {"excl_fn", required_argument, 0, 'E'},
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
    {"prune", required_argument, 0, 'p'},
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

// directory paths should not have a trailing '/' apart from the root!
static const sstring sysfs_root { "/sys" };   // default source (normalized)
static const sstring def_destin_root { "/tmp/sys" };
static const mode_t stat_perm_mask { 0x1ff };         /* bottom 9 bits */
static const mode_t def_file_perm { S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH };
static const char * src_symlink_tgt_path { "0_source_symlink_target_path" };

static const auto dir_opt { fs::directory_options::skip_permission_denied };

static std::error_code clone_work(const fs::path & src_pt,
                                  const fs::path & dst_pt,
                                  const struct opts_t * op) noexcept;
static std::error_code cache_src(inmem_dir_t * start_dirp,
                                 const fs::path & src_pt,
                                 const struct opts_t * op) noexcept;
static void prune_prop_dir(const inmem_dir_t * a_dirp,
                           const sstring & s_par_pt_s,
                           bool in_prune, const struct opts_t * op) noexcept;

/**
 * @param v - sorted vector instance
 * @param data - value to search
 * @return 0-based index if data found, -1 otherwise
*/
template <typename T>
    int binary_search_find_index(std::vector<T> v, const T & data) noexcept
    {
        auto it = std::lower_bound(v.begin(), v.end(), data);
        if (it == v.end() || *it != data) {
            return -1;
        } else {
            std::size_t index = std::distance(v.begin(), it);
            return static_cast<int>(index);
        }
    }

#ifdef DEBUG
static sstring
l(const std::error_code & ec = { },
  const std::source_location loc = std::source_location::current())
{
     sstring ss {
        want_sloc ? (sstring("; ") + loc.function_name() + "; ln=" +
                     std::to_string(loc.line()))
                  : sstring()
                };
    if (ec)
        ss += sstring("; ec: ") + ec.message();
    return ss;
}

#else

static sstring
l(const std::error_code & ec = { })
{
    return ec ? sstring("; ec: ") + ec.message() : sstring();
}

#endif

static inline sstring s(const fs::path & pt) { return pt.string(); }


static const char * const usage_message1 {
    "Usage: clone_pseudo_fs [--cache] [--dereference=SYML] "
    "[--destination=DPATH]\n"
    "                       [--exclude=PATT] [--excl-fn=EFN] [--extra] "
    "[--help]\n"
    "                       [--hidden] [--max-depth=MAXD] [--no-dst] "
    "[--no-xdev]\n"
    "                       [--prune=T_PT] [--reglen=RLEN] "
    "[--source=SPATH]\n"
    "                       [--statistics] [--verbose] [--version] "
    "[--wait=MS_R]\n"
    "  where:\n"
    "    --cache|-c         first cache SPATH to in-memory tree, then dump "
    "to\n"
    "                       DPATH. If used twice, also cache regular file\n"
    "                       contents\n"
    "    --dereference=SYML|-R SYML    SYML should be a symlink within "
    "SPATH\n"
    "                                  which will become a directory "
    "under\n"
    "                                  DPATH (i.e. a 'deep' copy)\n"
    "    --destination=DPATH|-d DPATH    DPATH is clone destination (def:\n"
    "                                    /tmp/sys (no default if SPATH "
    "given))\n"
    "    --exclude=PATT|-e PATT    PATT is a glob pattern, matching nodes\n"
    "                              (including directories) in SPATH to be "
    "excluded\n"
    "    --excl-fn=EFN|-E EFN    exclude nodes whose filenames match EFN. "
    "If node\n"
    "                            is symlink exclude matches on link "
    "filename\n"
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
    "    --prune=T_PT|-p T_PT    output will only contain files exactly "
    "matching\n"
    "                            or under T_PT (take path). Symlinks are "
    "followed\n"
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
    "\n"
};

static const char * const usage_message2 {
    "By default, this utility will clone /sys to /tmp/sys . The resulting "
    "subtree\nis a frozen snapshot that may be useful for later analysis. "
    "Hidden files\nare skipped and symlinks are created, even if dangling. "
    "The default is only\nto copy a maximum of 256 bytes from regular files."
    " If the --cache option\nis given, a two pass clone is used; the first "
    "pass creates an in memory\ntree. The --dereference=SYML , "
    "--exclude=PATT and --prune=T_PT options\ncan be invoked multiple "
    "times.\n"
};

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

void
inmem_base_t::debug_base(const sstring & intro) const noexcept
{
    if (intro.size() > 0)
        pr_err(-1, "{}\n", intro);
    pr_err(-1, "filename: {}\n", filename);
    pr_err(-1, "prune_mask: 0x{:x}\n", prune_mask);
    pr_err(-1, "parent_index: {}\n", par_dir_ind);
    pr_err(-1, "shstat.st_dev: 0x{:x}\n", shstat.st_dev);
    pr_err(-1, "shstat.st_mode: 0x{:x}\n", shstat.st_mode);
    pr_err(4, "  this={:p}\n", static_cast<const void *>(this));
}

inmem_subdirs_t:: inmem_subdirs_t(size_t vec_init_sz) noexcept :
                sdir_v(vec_init_sz), sdir_fn_ind_m()
{
}

void
inmem_subdirs_t::debug(const sstring & intro) const noexcept
{
    size_t sdir_v_sz { sdir_v.size() };
    size_t sdir_fn_ind_m_sz { sdir_fn_ind_m.size() };

    if (intro.size() > 0)
        pr_err(-1, "{}\n", intro);
    pr_err(-1, "  sdir_v.size: {}\n", sdir_v_sz);
    pr_err(-1, "  sdir_fn_ind_m.size: {}\n", sdir_fn_ind_m_sz);
    if ((cpf_verbose > 0) && (sdir_fn_ind_m_sz > 0)) {
        pr_err(0, "  sdir_fn_ind_m map:\n");
        for (auto && [n, v] : sdir_fn_ind_m)
            pr_err(0, "    [{}]--> {}\n", n, v);
    }
    if ((cpf_verbose > 1) && (sdir_v_sz > 0)) {
        pr_err(1, "  sdir_v vector:\n");
        for (int k { }; auto && v : sdir_v) {
            pr_err(1, "    {}:  {}, filename: {:s}\n", k,
                   inmem_var_str(v.index()), v.get_basep()->filename);
            ++k;
        }
    }
}

void
inmem_other_t::debug(const sstring & intro) const noexcept
{
    debug_base(intro);
    pr_err(-1, "  other file type\n");
    pr_err(4, "     this: {:p}\n", (void *) this);
}

// inmem_dir_t constructor
inmem_dir_t::inmem_dir_t() noexcept
        : sdirs_sp(std::make_shared<inmem_subdirs_t>())
{
}

inmem_dir_t::inmem_dir_t(const sstring & filename_,
                         const short_stat & a_shstat) noexcept
                : inmem_base_t(filename_, a_shstat, 0),
                  sdirs_sp(std::make_shared<inmem_subdirs_t>())
{
}

inm_var_t *
inmem_dir_t::get_subd_ivp(size_t index) noexcept
{
    return (index < sdirs_sp->sdir_v.size()) ?
           &sdirs_sp->sdir_v[index] : nullptr;
}

size_t
inmem_dir_t::add_to_sdir_v(inmem_other_t & n_oth) noexcept
{
    if (sdirs_sp) {
        size_t sz = sdirs_sp->sdir_v.size();
        sdirs_sp->sdir_v.push_back(n_oth);
        return sz;
    }
    return 0;
}

size_t
inmem_dir_t::add_to_sdir_v(inmem_dir_t & n_dir) noexcept
{
    if (sdirs_sp) {
        size_t sz { sdirs_sp->sdir_v.size() };
        const sstring fn { n_dir.filename };

        sdirs_sp->sdir_v.push_back(n_dir);
        sdirs_sp->sdir_fn_ind_m[fn] = sz; // only for directories
        return sz;
    }
    return 0;
}

size_t
inmem_dir_t::add_to_sdir_v(inmem_symlink_t & n_sym) noexcept
{
    if (sdirs_sp) {
        size_t sz { sdirs_sp->sdir_v.size() };
        sdirs_sp->sdir_v.push_back(n_sym);
        return sz;
    }
    return 0;
}

size_t
inmem_dir_t::add_to_sdir_v(inmem_device_t & n_dev) noexcept
{
    if (sdirs_sp) {
        size_t sz { sdirs_sp->sdir_v.size() };
        sdirs_sp->sdir_v.push_back(n_dev);
        return sz;
    }
    return 0;
}

size_t
inmem_dir_t::add_to_sdir_v(inmem_fifo_socket_t & n_fs) noexcept
{
    if (sdirs_sp) {
        size_t sz { sdirs_sp->sdir_v.size() };
        sdirs_sp->sdir_v.push_back(n_fs);
        return sz;
    }
    return 0;
}

size_t
inmem_dir_t::add_to_sdir_v(inmem_regular_t & n_reg) noexcept
{
    if (sdirs_sp) {
        size_t sz { sdirs_sp->sdir_v.size() };
        sdirs_sp->sdir_v.push_back(n_reg);
        return sz;
    }
    return 0;
}

void
inmem_dir_t::debug(const sstring & intro) const noexcept
{
    debug_base(intro);
    pr_err(-1, "  directory\n");
    pr_err(-1, "  parent_path: {}\n", par_pt_s.c_str());
    pr_err(-1, "  depth: {}\n", depth);
    pr_err(4, "     this: {:p}\n", (void *) this);
    if (sdirs_sp)
        sdirs_sp->debug();
}

void
inmem_symlink_t::debug(const sstring & intro) const noexcept
{
    debug_base(intro);
    pr_err(-1, "  symlink\n");
    pr_err(-1, "  target: {}\n", s(target));
}

void
inmem_device_t::debug(const sstring & intro) const noexcept
{
    debug_base(intro);
    pr_err(-1, "  device type: {}\n", is_block_dev ? "block" : "char");
    pr_err(-1, "  st_rdev: 0x{:x}\n", st_rdev);
}

void
inmem_fifo_socket_t::debug(const sstring & intro) const noexcept
{
    debug_base(intro);
    pr_err(-1, "  {}\n", "FIFO or socket");
}

void
inmem_regular_t::debug(const sstring & intro) const noexcept
{
    debug_base(intro);
    pr_err(-1, "  regular file:\n");
    if (read_found_nothing)
        pr_err(-1, "  read of contents found nothing\n");
    else if (contents.empty())
        pr_err(-1, "  empty\n");
    else
        pr_err(-1, "  file is {} bytes long\n", contents.size());
}

// inmem_t is derived from inm_var_t which is a std::variant of file_types
// held in the in-memory cache. All of those file_types (e.g. inmem_dir_t)
// have a get_filename() methods. Using std::visit() is an alternative to
// using a switch statement to decide which get_filename() to call.
sstring
inmem_t::get_filename() const noexcept
{
    return std::visit
        ([](auto&& arg) noexcept
            { return arg.get_filename(); },
         *this);
}

void
inmem_t::debug(const sstring & intro) const noexcept
{
    std::visit
        ([& intro](auto&& arg) noexcept
            { arg.debug(intro); },
         *this);
}

// In returned pair .first is true if pt found in vec, else false. The
// .second is false if rm_if_found is true and vec is empty after erase,
//  else true.
static std::pair<bool, bool>
find_in_sorted_vec(std::vector<sstring> & vec, const sstring & pt,
                   bool rm_if_found = false)
{
    std::pair<bool, bool> res { std::make_pair(false, true) };
    const auto ind { binary_search_find_index(vec, pt) };

    if (ind >= 0) {
        res.first = true;
        if (rm_if_found) {
            vec.erase(vec.begin() + ind);
            if (vec.empty())
                res.second = false;
        }
    }
    return res;
}

// This assumes both paths are in canonical form. Will still work if
// needle_c_pt is in absolute form (e.g. when needle_c_pt is the link
// path of a symlink).
static bool
path_contains_canon(const fs::path & haystack_c_pt,
                    const fs::path & needle_c_pt) noexcept
{
    auto hay_c_sz { s(haystack_c_pt).size() };
    auto need_c_sz { s(needle_c_pt).size() };

    if (need_c_sz == hay_c_sz)
        return needle_c_pt == haystack_c_pt;
    else if (need_c_sz < hay_c_sz)
        return false;

    auto c_need { needle_c_pt };        // since while loop modifies c_need

    do {
        // step needle back to its parent
        c_need = c_need.parent_path();
        need_c_sz = s(c_need).size();
    } while (need_c_sz > hay_c_sz);

    if (need_c_sz < hay_c_sz)
        return false;
    return c_need == haystack_c_pt;
}

static void
reg_s_err_stats(int err, struct stats_t * q) noexcept
{
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
}

static void
reg_d_err_stats(int err, struct stats_t * q) noexcept
{
    if (err == EACCES)
        ++q->num_reg_d_eacces;
    else if (err == EPERM)
        ++q->num_reg_d_eperm;
    else if (err == EIO)
        ++q->num_reg_d_eio;
    else if ((err == ENOENT) || (err == ENODEV) || (err == ENXIO))
        ++q->num_reg_d_enoent_enodev_enxio;
    else
        ++q->num_reg_d_e_other;
}

// Splits the parent path (par_pt_s) into a vector of strings containing the
// split up path, starting with (but not including) the initial SPATH path
// (base_pt_s). Those paths should be lexically normal (e.g. no embedded
// components like 'bus/../devices'). If an error is detected, ec is set.
static std::vector<sstring>
split_path(const sstring & par_pt_s, const sstring & base_pt_s,
           const struct opts_t *op, std::error_code & ec) noexcept
{
    std::vector<sstring> res;
    const char * pp_cp { par_pt_s.c_str() };
    const size_t base_pt_sz { base_pt_s.size() };
    size_t par_pt_sz { par_pt_s.size() };

    ec.clear();
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
    fs::path pt { pp_cp + base_pt_sz };
    // iterate over path components. Needs absolute path and '/' is the first
    // component found (surprisingly), so exclude it from the returned vector
    for (const auto & comp : pt) {
        const auto & comp_s { s(comp) };
        auto sz = comp_s.size();
        if (sz > 0) {
            if (comp_s[0] != '/')
                res.push_back(comp_s);
        }
    }
    return res;
}

// This function is the same as split_path() above but instead of
// returning a vector of path components, it returns the size of
// that vector. Both functions expect absolute, canonical paths.
static size_t
path_depth(const sstring & par_pt_s, const sstring & base_pt_s,
           const struct opts_t *op, std::error_code & ec) noexcept
{
    size_t res { 0 };
    const char * pp_cp { par_pt_s.c_str() };
    const size_t base_pt_sz { base_pt_s.size() };
    size_t par_pt_sz { par_pt_s.size() };

    ec.clear();
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
    fs::path pt { pp_cp + base_pt_sz };
    // iterate over path components. Needs absolute path and '/' is the first
    // component found (surprisingly), so exclude it from the returned vector
    for (const auto & comp : pt) {
        const auto & comp_s { s(comp) };
        auto sz = comp_s.size();
        if (sz > 0) {
            if (comp_s[0] != '/')
                ++res;
        }
    }
    return res;
}

// Returns number of bytes read, -1 for general error, -2 for timeout
static int
read_err_wait(int from_fd, uint8_t * bp, int err,
              const struct opts_t *op) noexcept
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
    reg_s_err_stats(err, q);
    return num;
}

// Returns 0 on success, else a Unix like errno value is returned.
// st_mode can be 0 in which case def_file_perm are used.
static int
xfr_vec2file(const std::vector<uint8_t> & v, const sstring & destin_file,
             mode_t st_mode, const struct opts_t * op) noexcept
{
    int res { };
    int destin_fd { -1 };
    int num { static_cast<int>(v.size()) };
    int num2;
    // need S_IWUSR set if non-root and want later overwrite
    mode_t from_perms
        { static_cast<mode_t>((st_mode | def_file_perm) & stat_perm_mask) };
    const uint8_t * bp;
    const char * destin_nm { destin_file.c_str() };
    struct stats_t * q { &op->mutp->stats };

    bp = (v.empty() ? nullptr : &v[0]);
    if (op->destin_all_new) {
        destin_fd = creat(destin_nm, from_perms);
        if (destin_fd < 0) {
            reg_d_err_stats(errno, q);
            goto fini;
        }
    } else {
        destin_fd = open(destin_nm, O_RDWR | O_CREAT | O_TRUNC, from_perms);
        if (destin_fd < 0) {
            reg_d_err_stats(errno, q);
            goto fini;
        }
    }
    if (bp && (num > 0)) {
        num2 = write(destin_fd, bp, num);
        if (num2 < 0) {
            reg_d_err_stats(errno, q);
            goto fini;
        }
        if (num2 < num)
            pr_err(0, "short write() to dst: {}, strange{}\n",
                   destin_nm, l());
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
                   const struct opts_t * op) noexcept
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
                reg_s_err_stats(errno, q);
                goto fini;
            }
            res = 0;
            from_perms = from_stat.st_mode & stat_perm_mask;
            num = 0;
            goto store;
        }
        reg_s_err_stats(res, q);
        goto fini;
    }
    if (fstat(from_fd, &from_stat) < 0) {
        res = errno;
        ++q->num_reg_s_e_other;  // not expected if open() is good
        goto fini;
    }
    from_perms = from_stat.st_mode & stat_perm_mask;
    if (op->reglen > 0) {
        int off {};

        do {
            num = read(from_fd, bp + off, op->reglen - off);
            if (num < 0) {
                res = errno;
                num = read_err_wait(from_fd, bp, res, op);
                if (num < 0) {
                    if (num == -2)
                        pr_err(0, "timed out waiting for this file: {}{}\n",
                               from_file, l());
                    num = 0;
                    res = 0;
                    close(from_fd);
                    goto store;
                }
            } else {
                off += num;
                if (num < reg_re_read_sz)
                    break;
            }
        } while (true);
        num = off;
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
    ireg.shstat.st_mode = from_perms;

fini:
    if (from_fd >= 0)
        close(from_fd);
    return res;
}

// Returns 0 on success, else a Unix like errno value is returned.
static int
xfr_reg_inmem2file(const inmem_regular_t & ireg, const sstring & destin_file,
                   const struct opts_t * op) noexcept
{
    mode_t from_perms
        { static_cast<mode_t>(ireg.shstat.st_mode & stat_perm_mask) };

    return xfr_vec2file(ireg.contents, destin_file, from_perms, op);
}

// N.B. Only root can successfully invoke the mknod(2) system call
static int
xfr_dev_inmem2file(const inmem_device_t & idev, const sstring & destin_file,
                   const struct opts_t * op) noexcept
{
    int res { };
    struct stats_t * q { &op->mutp->stats };

    if (mknod(destin_file.c_str(), idev.shstat.st_mode, idev.st_rdev) < 0) {
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
                  const struct opts_t * op) noexcept
{
    int res { 0 };
    int from_fd { -1 };
    int rd_flags { O_RDONLY };
    int num { };
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
                reg_s_err_stats(errno, q);
                goto fini;
            }
            from_perms = from_stat.st_mode & stat_perm_mask;
            num = 0;
            goto do_destin;
        }
        reg_s_err_stats(res, q);
        goto fini;
    }
    if (fstat(from_fd, &from_stat) < 0) {
        res = errno;
        ++q->num_reg_s_e_other;  // not expected if open() is good
        goto fini;
    }
    from_perms = from_stat.st_mode & stat_perm_mask;
    if (op->reglen > 0) {
        int off {};

        do {
            num = read(from_fd, bp + off, op->reglen - off);
            if (num < 0) {
                res = errno;
                num = read_err_wait(from_fd, bp, res, op);
                if (num < 0) {
                    if (num == -2)
                        pr_err(0, "timed out waiting for this file: {}{}\n",
                               from_file, l());
                    num = 0;
                    close(from_fd);
                    goto do_destin;
                }
            } else {
                off += num;
                if (num < reg_re_read_sz)
                    break;
            }
        } while (true);
        num = off;
    } else
        num = 0;
    // closing now might help in this function is multi-threaded
    close(from_fd);
    if (static_cast<unsigned int>(num) >= op->reglen)
        ++q->num_reg_s_at_reglen;
do_destin:
    from_fd = -1;
    if (num >= 0) {
        if (num > 0)
            res = xfr_vec2file(std::vector<uint8_t>(bp, bp + num),
                               destin_file, from_perms, op);
        else
            res = xfr_vec2file(std::vector<uint8_t>(), destin_file,
                               from_perms, op);
    }
fini:
    if (from_fd >= 0)
        close(from_fd);
    return res;
}

static std::error_code
xfr_other_ft(fs::file_type ft, const fs::path & src_pt,
             const struct stat & src_stat, const fs::path & dst_pt,
             const struct opts_t * op) noexcept
{
    int res { };
    std::error_code ec { };
    struct stats_t * q { &op->mutp->stats };

    pr_err(3, "{}: ft={}, src_pt: {}, dst_pt: {}\n", __func__,
           static_cast<int>(ft), s(src_pt), s(dst_pt));
    switch (ft) {
    using enum fs::file_type;

    case regular:
        res = xfr_reg_file2file(src_pt, dst_pt, op);
        if (res) {
            ec.assign(res, std::system_category());
            pr_err(3, "{} --> {}: xfr_reg_file2file() failed{}\n", s(src_pt),
                   s(dst_pt), l(ec));
            ++q->num_error;
        } else
            pr_err(5, "{} --> {}: xfr_reg_file2file() ok{}\n", s(src_pt),
                   s(dst_pt), l(ec));
        break;
    case block:
    case character:
        // N.B. Only root can successfully invoke the mknod(2) system call
        if (mknod(dst_pt.c_str(), src_stat.st_mode,
                  src_stat.st_rdev) < 0) {
            res = errno;
            ec.assign(res, std::system_category());
            pr_err(3, "{} --> {}: mknod() failed{}\n", s(src_pt), s(dst_pt),
                   l(ec));
            if (res == EACCES)
                ++q->num_mknod_d_eacces;
            else if (res == EPERM)
                ++q->num_mknod_d_eperm;
            else
                ++q->num_mknod_d_fail;
        } else
            pr_err(5, "{} --> {}: mknod() ok{}\n", s(src_pt), s(dst_pt));
        break;
    case fifo:
        pr_err(0, "source: {}; file type: fifo not supported{}\n",
               s(src_pt), l());
        break;
    case socket:
        pr_err(0, "source: {}; file type: socket not supported{}\n",
               s(src_pt), l());
        break;              // skip these file types
    default:                // here when something no longer exists
        pr_err(3, "unexpected file_type={}{}\n", static_cast<int>(ft),
               l());
        break;
    }
    return ec;
}

static void
update_stats(fs::file_type s_sym_ftype, fs::file_type s_ftype, bool hidden,
             const struct opts_t * op) noexcept
{
    struct stats_t * q { &op->mutp->stats };

    if (hidden)
        ++q->num_hidden;
    if (s_sym_ftype == fs::file_type::symlink) {
        if (s_ftype == fs::file_type::directory)
            ++q->num_sym2dir;
        else if (s_ftype == fs::file_type::regular)
            ++q->num_sym2reg;
        else if (s_ftype == fs::file_type::block)
            ++q->num_sym2block;
        else if (s_ftype == fs::file_type::character)
            ++q->num_sym2char;
        else if (s_ftype == fs::file_type::none)
            ++q->num_symlink;
        else if (s_ftype == fs::file_type::not_found)
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
show_stats(const struct opts_t * op) noexcept
{
    bool extra { (op->want_stats > 1) || (cpf_verbose > 0) };
    bool eagain_likely { (op->wait_given && (op->reglen > 0)) };
    struct stats_t * q { &op->mutp->stats };

    scout << "Statistics:\n";
    scout << "Number of nodes: " << q->num_node << "\n";
    scout << "Number of regular files: " << q->num_regular << "\n";
    scout << "Number of directories: " << q->num_dir << "\n";
    scout << "Number of symlinks to directories: " << q->num_sym2dir << "\n";
    scout << "Number of symlinks to regular files: " << q->num_sym2reg
          << "\n";
    scout << "Number of symlinks to symlinks: " << q->num_sym2sym << "\n";
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
    if ((q->num_sym_s_eacces > 0) || (q->num_sym_s_eperm > 0) ||
        (q->num_sym_s_enoent > 0))
        scout << "Number of src symlink EACCES, EPERM, ENOENT errors: "
              << q->num_sym_s_eacces <<  ", " << q->num_sym_s_eperm << ", "
              << q->num_sym_s_enoent <<  "\n";
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
    if (q->num_hidden_skipped == 0)
        scout << "Number of filenames starting with '.': " << q->num_hidden
              << "\n";
    if (! op->no_destin) {
        scout << "Number of dst created directories: "
              << q->num_dir_d_success << "\n";
        scout << "Number of already existing dst directories: "
              << q->num_dir_d_exists << "\n";
        scout << "Number of dst created directory failures: "
              << q->num_dir_d_fail << "\n";
        scout << "Number of dst created symlinks: "
              << q->num_sym_d_success << "\n";
        if (op->do_extra > 0)
            scout << "Number of dst dangling symlinks: "
                  << q->num_sym_d_dangle
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
    if (op->exclude_given)
        scout << "Number of pathnames excluded: " << q->num_excluded << "\n";
    if (op->excl_fn_given)
        scout << "Number of filenames excluded: " << q->num_excl_fn << "\n";
    if (op->deref_given)
        scout << "Number of dereferenced symlinks: " << q->num_derefed
              << "\n";
    // N.B. recursive_directory_iterator::depth() is one less than expected
    scout << "Maximum depth of source scan: " << q->max_depth + 1 << "\n";
    if (op->prune_given) {
        scout << "Number of prune exact matches: " << q->num_prune_exact
              << "\n";
        scout << "Number of pruned nodes: " << q->num_pruned_node << "\n";
        if ((q->num_prune_err + q->num_prune_sym_pt_err +
             q->num_prune_sym_outside) > 0) {
            scout << "Number of prune symlink target path errors: "
                  << q->num_prune_sym_pt_err << "\n";
            scout << "Number of prune symlink target paths outside SPATH: "
                  << q->num_prune_sym_outside << "\n";
            scout << "Number of prune errors: " << q->num_prune_err << "\n";
        }
    }
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
        if (q->num_reg_from_cache_err)
            scout << "Number of cache to regular file errors: "
                  << q->num_reg_from_cache_err << "\n";
    }
    scout << "Number of files " << op->reglen << " bytes or longer: "
          << q->num_reg_s_at_reglen << "\n";
}

static fs::path
read_symlink(const fs::path & pt, const struct opts_t * op,
             std::error_code & ec) noexcept
{
    struct stats_t * q { &op->mutp->stats };
    const auto target_pt = fs::read_symlink(pt, ec);

    if (ec) {
        pr_err(2, "{}: read_symlink() failed{}\n", s(pt), l(ec));
        auto err = ec.value();
        if (err == EACCES)
            ++q->num_sym_s_eacces;
        else if (err == EPERM)
            ++q->num_sym_s_eperm;
        else if (err == ENOENT)
            ++q->num_sym_s_enoent;
        else
            ++q->num_sym_s_dangle;
        return target_pt;
    }

    if (op->want_stats > 0) {
        const fs::path join_pt { pt.parent_path() / target_pt };

        if (fs::symlink_status(join_pt, ec).type() == fs::file_type::symlink)
            ++q->num_sym2sym;
        else if (ec)
            pr_err(2, "{}: read_symlink({}) sym2sym failed{}\n", s(pt),
                   s(target_pt), l(ec));
        ec.clear();
    }
    pr_err(5, "{}: link pt: {}, target pt: {}{}\n", __func__, s(pt),
           s(target_pt), l(ec));
    pr_err(6, "   lexically_normal target pt: {}\n",
           s(target_pt.lexically_normal()));
    return target_pt;
}

// Returns pair <error_code ec, bool serious>. If ec is true (holds error)
// caller should only consider it serious if that (second) flag is true.
static std::pair<std::error_code, bool>
symlink_clone_work(const fs::path & pt, const fs::path & prox_pt,
                   const fs::path & ongoing_d_pt, bool deref_entry,
                   const struct opts_t * op) noexcept
{
    std::error_code ec { };
    struct stats_t * q { &op->mutp->stats };
    const fs::path target_pt = read_symlink(pt, op, ec);

    if (ec)
        return {ec, false};
    fs::path d_lnk_pt { prox_pt / pt.filename() };
    if (! op->destin_all_new) {     /* may already exist */
        const auto d_lnk_ftype { fs::symlink_status(d_lnk_pt, ec).type() };

        if (ec) {
            int v { ec.value() };

            // ENOENT can happen with "dynamic" sysfs
            if (v == ENOENT) {
                ++q->num_sym_s_enoent;
                pr_err(4, "{}: symlink_status() failed{}\n", s(d_lnk_pt),
                       l(ec));
            } else {
                ++q->num_sym_s_dangle;
                pr_err(2, "{}: symlink_status() failed{}\n", s(d_lnk_pt),
                       l(ec));
            }
            return {ec, false};
        }
        if (d_lnk_ftype == fs::file_type::symlink)
            return {ec, false};
        else if (d_lnk_ftype == fs::file_type::not_found)
            ;       // drop through
        else if (deref_entry && (d_lnk_ftype == fs::file_type::directory))
            return {ec, false};  // skip because destination is already dir
        else {
            pr_err(-1, "{}: unexpected d_lnk_ftype{}\n", s(d_lnk_pt), l(ec));
            ++q->num_error;
            return {ec, false};
        }
    }

    if (deref_entry) {
        const fs::path join_pt { pt.parent_path() / target_pt };
        const fs::path canon_s_sl_targ_pt { fs::canonical(join_pt, ec) };
        if (ec) {
            pr_err(0, "{}: canonical() failed{}\n", s(join_pt), l(ec));
            pr_err(0, "{}: symlink probably dangling{}\n", s(pt), l());
            ++q->num_sym_s_dangle;
            return {ec, false};
        }
        if (! path_contains_canon(op->source_pt, canon_s_sl_targ_pt)) {
            ++q->num_follow_sym_outside;
            pr_err(0, "{}: outside, fall back to symlink{}\n",
                   s(canon_s_sl_targ_pt), l());
            goto process_as_symlink;
        }
        const auto s_targ_ftype { fs::status(canon_s_sl_targ_pt, ec).type() };
        if (ec) {
            pr_err(0, "{}: fs::status() failed{}\n", s(canon_s_sl_targ_pt),
                   l(ec));
            ++q->num_error;
            return {ec, false};
        }
        /* symlink to directory becomes directory */
        if (s_targ_ftype == fs::file_type::directory) {
            // create dir when src is symlink and follow active
            // no problem if already exists
            fs::create_directory(d_lnk_pt, ec);
            if (ec) {
                pr_err(0, "{}: create_directory() failed{}\n", s(d_lnk_pt),
                       l(ec));
                ++q->num_dir_d_fail;
                return {ec, false};
            }
            const fs::path deep_d_pt { ongoing_d_pt / d_lnk_pt };
            const auto d_sl_tgt { d_lnk_pt / src_symlink_tgt_path };
            const auto ctspt { s(canon_s_sl_targ_pt) + "\n" };
            const char * ccp { ctspt.c_str() };
            const uint8_t * bp { reinterpret_cast<const uint8_t *>(ccp) };
            std::vector<uint8_t> v(bp, bp + ctspt.size());

            int res = xfr_vec2file(v, d_sl_tgt, 0, op);
            if (res) {
                ec.assign(res, std::system_category());
                pr_err(3, "{}: xfr_vec2file() failed{}\n", s(d_sl_tgt),
                       l(ec));
                ++q->num_error;
            }
            ec = clone_work(canon_s_sl_targ_pt, deep_d_pt, op);
            if (ec) {
                pr_err(-1, "{}: clone_work() failed{}\n",
                       s(canon_s_sl_targ_pt), l(ec));
                return {ec, true};
            }
        } else if (s_targ_ftype == fs::file_type::regular) {
            struct stat src_stat { };       // not needed for reg->reg
            ec = xfr_other_ft(fs::file_type::regular, canon_s_sl_targ_pt,
                              src_stat, d_lnk_pt, op);
            ec.clear();
        } else {
            pr_err(0, "{}: deref other than sl->dir or sl->reg, fall back "
                   "to symlink{}\n", s(canon_s_sl_targ_pt), l());
            goto process_as_symlink;
        }
        return {ec, false};
    }               // end of id (deref_entry)
process_as_symlink:
    fs::create_symlink(target_pt, d_lnk_pt, ec);
    if (ec) {
        pr_err(0, "{} --> {}: create_symlink() failed\n", s(d_lnk_pt),
               s(target_pt), l(ec));
        ++q->num_error;
    } else {
        ++q->num_sym_d_success;
        pr_err(4, "{} --> {}: create_symlink() ok\n", s(d_lnk_pt),
               s(target_pt), l());
        if (op->do_extra > 0) {
            fs::path abs_target_pt { prox_pt / target_pt };
            if (fs::exists(abs_target_pt, ec))
                pr_err(4, "{}: symlink target exists{}\n", s(abs_target_pt),
                       l());
            else if (ec) {
                pr_err(-1, "fs::exists({}) failed{}\n", s(abs_target_pt),
                       l(ec));
                ++q->num_error;
            } else
                ++q->num_sym_d_dangle;
        }
    }
    return {ec, false};
}

static void
dir_clone_work(const fs::path & pt, fs::recursive_directory_iterator & itr,
               dev_t st_dev, fs::perms s_perms, const fs::path & ongoing_d_pt,
               const struct opts_t * op, std::error_code & ec) noexcept
{
    struct stats_t * q { &op->mutp->stats };

    if (! op->no_xdev) { // double negative ...
        if (st_dev != op->mutp->starting_fs_inst) {
            // do not visit this sub-branch: different fs instance
            pr_err(1, "Source trying to leave this fs instance at: {}\n",
                   s(pt));
            itr.disable_recursion_pending();
            ++q->num_oth_fs_skipped;
        }
    }
    if (! op->destin_all_new) { /* may already exist */
        if (fs::exists(ongoing_d_pt, ec)) {
            if (fs::is_directory(ongoing_d_pt, ec)) {
                ++q->num_dir_d_exists;
            } else if (ec) {
                pr_err(-1, "is_directory({}) failed\n", s(ongoing_d_pt),
                       l(ec));
                ++q->num_error;
            } else
                pr_err(0, "{}: exists but not directory, skip{}\n",
                       s(ongoing_d_pt), l());
            return;
        } else if (ec) {
            pr_err(-1, "{}: exists() failed{}\n", s(ongoing_d_pt), l(ec));
            ++q->num_error;
            return;
        } else {
            ;   // drop through to create_dir
        }
    }
    if (fs::create_directory(ongoing_d_pt, pt, ec)) {
        const fs::perms s_pms = s_perms;
        if ((s_pms & fs::perms::owner_write) != fs::perms::owner_write) {
            // if source directory doesn't have owner_write then
            // make sure destination does.
            fs::permissions(ongoing_d_pt, fs::perms::owner_write,
                            fs::perm_options::add, ec);
            if (ec) {
                pr_err(-1, "{}: couldn't add owner_write perm{}\n",
                       s(ongoing_d_pt), l(ec));
                ++q->num_error;
                return;
            }
        }
        ++q->num_dir_d_success;
        pr_err(5, "{}: create_directory() ok{}\n", s(ongoing_d_pt), l(ec));
    } else {
        if (ec) {
            ++q->num_dir_d_fail;
            pr_err(1, "{}: create_directory() failed{}\n", s(ongoing_d_pt),
                   l(ec));
        } else {
            ++q->num_dir_d_exists;
            pr_err(2, "{}: create_directory() failed{}\n", s(ongoing_d_pt),
                   l());
        }
    }
}

// Called from do_clone() and if --deref= given may call itself recursively.
// There are two levels of error reporting, when ecc is set it will cause
// the immediate return of that value. If this function has been called
// recursively, the recursive stack will be quickly unwound. The other
// variety of errors are placed in 'ec' and are reported in the statistics
// and may cause processing of the currently node to be stopped and
// processing will continue to the next node.
static std::error_code
clone_work(const fs::path & src_pt, const fs::path & dst_pt,
           const struct opts_t * op) noexcept
{
    struct mut_opts_t * omutp { op->mutp };
    bool possible_exclude { ! omutp->glob_exclude_v.empty() };
    bool possible_excl_fn { ! op->excl_fn_v.empty() };
    bool possible_deref { ! omutp->deref_v.empty() };
    struct stats_t * q { &omutp->stats };
    struct stat src_stat;
    std::error_code ecc { };

    if (! omutp->clone_work_subseq)
        omutp->clone_work_subseq = true;
    else {      // not first call but all after
        if (op->do_extra) {
            bool src_pt_contained { path_contains_canon(op->source_pt,
                                                        src_pt) };
            bool dst_pt_contained { true };
            if (! op->no_destin)
                dst_pt_contained = path_contains_canon(op->destination_pt,
                                                       dst_pt);
            bool bad { false };

            if (src_pt_contained && dst_pt_contained) {
                pr_err(3, "{}: both src and dst contained, good\n", __func__);
            } else if (! src_pt_contained) {
                pr_err(-1, "{}: src: {} NOT contained, bad\n", __func__,
                       s(src_pt));
                bad = true;
            } else {
                pr_err(-1, "{}: dst: {} NOT contained, bad\n", __func__,
                       s(dst_pt));
                bad = true;
            }
            if (op->no_destin)
                pr_err(0, "{}: src_pt: {}\n", __func__, s(src_pt));
            else
                pr_err(0, "{}: src_pt: {}, dst_pt: {}\n", __func__,
                       s(src_pt), s(dst_pt));
            if (bad) {
                ecc.assign(EDOM, std::system_category());
                return ecc;
            }
        }
        const auto s_ftype { fs::status(src_pt, ecc).type() };

        if (ecc) {
            pr_err(-1, "{}: failed getting file type{}\n", s(src_pt), l(ecc));
            return ecc;
        }
        if (s_ftype != fs::file_type::directory) {
            if (stat(src_pt.c_str(), &src_stat) < 0) {
                ecc.assign(errno, std::system_category());
                pr_err(-1, "{}: stat() failed{}\n", s(src_pt), l(ecc));
                ++q->num_error;
                return ecc;
            }
            if (! op->no_destin)
                ecc =  xfr_other_ft(s_ftype, src_pt, src_stat, dst_pt, op);
            return ecc;
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
        const auto & pt_s { s(pt) };
        const auto depth { itr.depth() };
        bool exclude_entry { false };
        bool deref_entry { false };
        prev_rdi_pt = pt;

        ++q->num_node;
        pr_err(6, "{}: about to scan this source entry{}\n", s(pt), l());
        const auto s_sym_ftype = itr->symlink_status(ec).type();
        if (ec) {       // serious error
            ++q->num_error;
            pr_err(2, "symlink_status({}) failed, continue{}\n", s(pt),
                   l(ec));
            // thought of using entry.refresh(ec) but no speed improvement
            continue;
        }
        const auto itr_status { itr->status(ec) };
        fs::file_type s_ftype { fs::file_type::none };
        if (ec) {
            if (s_sym_ftype == fs::file_type::symlink)
                ++q->num_sym_s_dangle;
            else
                ++q->num_error;
            pr_err(4, "itr->status({}) failed, continue{}\n", s(pt), l(ec));
            // continue;
        } else
            s_ftype = itr_status.type();

        if (depth > q->max_depth)
            q->max_depth = depth;
        if (op->max_depth_active &&
            (s_sym_ftype == fs::file_type::directory) &&
            (depth >= op->max_depth)) {
            pr_err(2, "Source at max_depth and this is a directory: {}, "
                   "don't enter\n", s(pt));
            itr.disable_recursion_pending();
        }

        const bool hidden_entry = ((! pt.empty()) &&
                                  (s(pt.filename())[0] == '.'));
        if (possible_deref && (s_sym_ftype == fs::file_type::symlink)) {
            std::tie(deref_entry, possible_deref) =
                        find_in_sorted_vec(omutp->deref_v, pt_s, true);
            if (deref_entry) {
                ++q->num_derefed;
                pr_err(3, "{}: matched for dereference{}\n", s(pt), l());
            }
        }
        if (! deref_entry) {    // deref trumps exclude
            if (possible_exclude) {
                std::tie(exclude_entry, possible_exclude) =
                    find_in_sorted_vec(omutp->glob_exclude_v, pt_s, true);
                if (exclude_entry) {
                    ++q->num_excluded;
                    pr_err(3, "{}: matched for exclusion{}\n", s(pt), l());
                }
            }
            if (possible_excl_fn) {
                if (std::ranges::binary_search(op->excl_fn_v,
                                               s(pt.filename()))) {
                    ++q->num_excl_fn;
                    exclude_entry = true;
                }
            }
        }
        if (op->want_stats > 0)
            update_stats(s_sym_ftype, s_ftype, hidden_entry, op);
        if (op->no_destin) {
            // for --no-dst only collecting stats after excludes and derefs
            if (deref_entry) {
                const fs::path target_pt = read_symlink(pt, op, ec);
                if (ec)
                    continue;
                const fs::path join_pt { pt.parent_path() / target_pt };
                const fs::path canon_s_sl_targ_pt
                                    { fs::canonical(join_pt, ec) };
                if (ec) {
                    pr_err(0, "canonical({}) failed{}\n",
                           s(canon_s_sl_targ_pt), l(ec));
                    pr_err(0, "{}: symlink probably dangling{}\n", s(pt),
                           l());
                    ++q->num_sym_s_dangle;
                    continue;
                }
                ecc = clone_work(canon_s_sl_targ_pt, "", op);
                if (ecc) {
                    pr_err(-1, "clone_work({}) failed{}\n",
                           s(canon_s_sl_targ_pt), l(ecc));
                    ++q->num_error;
                    return ecc;  // propagate error
                }
            } else if (exclude_entry)
                itr.disable_recursion_pending();
            else if ((s_sym_ftype == fs::file_type::directory) &&
                     op->max_depth_active && (depth >= op->max_depth)) {
                if (cpf_verbose > 2)
                    pr_err(-1, "{}: hits max_depth={}, don't enter {}{}\n",
                           __func__, depth, s(pt), l());
                itr.disable_recursion_pending();
            }
            continue;
        }
        if ((! op->clone_hidden) && hidden_entry) {
            ++q->num_hidden_skipped;
            if (s_sym_ftype == fs::file_type::directory)
                itr.disable_recursion_pending();
            continue;
        }
        if ((s_ftype != fs::file_type::none) &&
            (stat(pt.c_str(), &src_stat) < 0)) {
            ec.assign(errno, std::system_category());
            pr_err(-1, "stat({}) failed{}\n", s(pt), l(ec));
            ++q->num_error;
            continue;
        }
        fs::path rel_pt { fs::proximate(pt, src_pt, ec) };
        if (ec) {
            pr_err(1, "{}: proximate() failed{}\n", s(pt), l(ec));
            ++q->num_error;
            continue;
        }
        fs::path ongoing_d_pt { dst_pt / rel_pt };
        if (cpf_verbose > 4)
        pr_err(4, "{}: pt: {}, rel_path: {}, ongoing_d_pt: {}\n", __func__,
               s(pt), s(rel_pt), s(ongoing_d_pt));

        switch (s_sym_ftype) {
        using enum fs::file_type;

        case directory:
            if (exclude_entry) {
                itr.disable_recursion_pending();
                continue;
            }
            dir_clone_work(pt, itr, src_stat.st_dev,
                           itr_status.permissions(), ongoing_d_pt, op, ec);
            break;
        case symlink:
            {
                if (exclude_entry)
                    break;
                const fs::path parent_pt { pt.parent_path() };
                fs::path prox_pt { dst_pt };
                if (parent_pt != src_pt) {
                    prox_pt /= fs::proximate(parent_pt, src_pt, ec);
                    if (ec) {
                        pr_err(-1, "symlink: proximate({}) failed{}\n",
                               s(parent_pt), l(ec));
                        ++q->num_error;
                        break;
                    }
                }
                bool serious;

                std::tie(ec, serious) =
                        symlink_clone_work(pt, prox_pt, ongoing_d_pt,
                                           deref_entry, op);
                if (serious)
                    return ec;
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
            ec = xfr_other_ft(s_sym_ftype, pt, src_stat, ongoing_d_pt, op);
            ec.clear();
            break;
        default:                // here when something no longer exists
            switch (s_ftype) {
            using enum fs::file_type;

            case directory:
                if (op->max_depth_active && (depth >= op->max_depth)) {
                    pr_err(2, "Source: {} at max_depth: {}, don't enter\n",
                           s(pt), depth);
                    itr.disable_recursion_pending();
                }
                break;
            case symlink:
                pr_err(2, "{}: switch in switch symlink, skip\n", s(pt));
                break;
            case regular:
                pr_err(2, "{}: switch in switch regular file, skip\n", s(pt));
                break;
            default:
                pr_err(2, "{}, switch in switch s_sym_ftype: {}\n", s(pt),
                       static_cast<int>(s_sym_ftype));

                break;
            }
            break;
        }               // end of switch (s_sym_ftype)
    }                   // end of recursive_directory scan for loop
    if (ecc) {
        ++q->num_scan_failed;
        pr_err(-1, "recursive_directory_iterator() failed, prior entry: "
               "{}{}\n", s(prev_rdi_pt), l(ecc));
    }
    return ecc;
}

static void
cache_reg(inmem_dir_t * l_odirp, const short_stat & a_shstat,
          const fs::path & s_pt, bool mark_prune,
          const struct opts_t * op) noexcept
{
    std::error_code ec { };
    struct stats_t * q { &op->mutp->stats };
    const sstring filename { s_pt.filename() };
    inmem_regular_t a_reg(filename, a_shstat);
    if (mark_prune) {
        a_reg.prune_mask |= prune_exact;
        ++q->num_prune_exact;
    }

    size_t ind { l_odirp->add_to_sdir_v(a_reg) };
    auto iregp { std::get_if<inmem_regular_t> (l_odirp->get_subd_ivp(ind)) };

    if (iregp && (op->cache_op_num > 1)) {
        if (int res { xfr_reg_file2inmem(s_pt, *iregp, op) }) {
            ec.assign(res, std::system_category());
            pr_err(3, "{}: xfr_reg_file2inmem({}) failed{}\n", __func__,
                   s(s_pt), l(ec));
            ++q->num_reg_from_cache_err;
        } else
            pr_err(5, "{}: xfr_reg_file2inmem({}) ok{}\n", __func__, s(s_pt),
                   l());
    }
}

// Returns pair <error_code ec, bool serious>. If ec is true (holds error)
// caller should only consider it serious if that (second) flag is true.
// Note: this function is called by cache_src() and may in turn call
// cache_src(), that is: it can be part of a recursion loop.
static std::pair<std::error_code, bool>
symlink_cache_src(const fs::path & pt, const short_stat & a_shstat,
                  inmem_dir_t * & l_odirp, inmem_dir_t * prev_odirp,
                  bool deref_entry, bool got_prune_exact,
                  const struct opts_t * op) noexcept
{
    std::error_code ec { };
    struct stats_t * q { &op->mutp->stats };
    const auto filename_pt = pt.filename();
    const auto par_pt = pt.parent_path();

    const auto target_pt = read_symlink(pt, op, ec);
    if (ec)
        return {ec, false};
    inmem_symlink_t a_sym(filename_pt, a_shstat);
    a_sym.target = target_pt;
    if (got_prune_exact) {
        a_sym.prune_mask |= prune_exact;
        ++q->num_prune_exact;
    }
    if (deref_entry) {   /* symlink becomes directory */
        const fs::path canon_s_targ_pt
                        { fs::canonical(par_pt / target_pt, ec) };
        if (ec) {
            pr_err(0, "canonical({}) failed{}\n", s(canon_s_targ_pt), l(ec));
            pr_err(0, "{}: symlink probably dangling{}\n", s(pt), l());
            ++q->num_sym_s_dangle;
            return {ec, false};
        }
        if (! path_contains_canon(op->source_pt, canon_s_targ_pt)) {
            ++q->num_follow_sym_outside;
            pr_err(0, "{}: outside, fall back to symlink{}\n",
                   s(canon_s_targ_pt), l());
            goto process_as_symlink;
        }
        const auto s_targ_ftype { fs::status(canon_s_targ_pt, ec).type() };
        if (ec) {
            pr_err(0, "fs::status({}) failed{}\n", s(canon_s_targ_pt), l(ec));
            ++q->num_error;
            return {ec, false};
        }
        if (s_targ_ftype == fs::file_type::directory) {
            prev_odirp = l_odirp;
            inmem_dir_t a_dir(filename_pt, a_shstat);
            a_dir.par_pt_s = s(par_pt);
            auto depth = path_depth(s(par_pt), op->source_pt, op, ec);
            if (ec)
                depth = 0;
            a_dir.depth = depth + 1;    // asked depth of parent ...
            auto ind = l_odirp->add_to_sdir_v(a_dir);
            l_odirp = std::get_if<inmem_dir_t> (l_odirp->get_subd_ivp(ind));
            if (l_odirp) {
                const auto ctspt { s(canon_s_targ_pt) + "\n" };
                const char * ccp { ctspt.c_str() };
                const uint8_t * bp { reinterpret_cast<const uint8_t *>(ccp) };
                std::vector<uint8_t> v(bp, bp + ctspt.size());
                short_stat b_shstat { a_shstat };
                b_shstat.st_mode &= ~stat_perm_mask;
                b_shstat.st_mode |= def_file_perm;
                inmem_regular_t a_reg(src_symlink_tgt_path, b_shstat);

                a_reg.contents.swap(v);
                a_reg.always_use_contents = true;
                l_odirp->add_to_sdir_v(a_reg);
                ec = cache_src(l_odirp, canon_s_targ_pt, op);
                if (ec)
                    return {ec, false};         /* was true dpg 20231219 */
            }
            l_odirp = prev_odirp;
        } else if (s_targ_ftype == fs::file_type::regular) {
            cache_reg(l_odirp, a_shstat, pt, false, op);
            pr_err(3, "{}: symlink to regular file\n", s(canon_s_targ_pt));
        }
        return {ec, false};
    }
process_as_symlink:
    l_odirp->add_to_sdir_v(a_sym);
    return {ec, false};
}

// Tracking a directory recursive descent iterator for the purposes of
// building an in-memory hierarchial representation is relatively easy
// until the iterator goes back up (i.e. towards the root) 2 or more
// directories. The algorithm here is to split the parent path (par_pt)
// up till source root path (osrc_pt) into its component parts and then
// step down from the root (in odirp). Output is placed in l_odirp which
// is an in-out parameter. Returns true on success. Part of pass 1.
static bool
cache_recalc_grandparent(const fs::path & par_pt,
                         const fs::path & osrc_pt, inmem_dir_t * odirp,
                         inmem_dir_t * & l_odirp,
                         const struct opts_t * op, std::error_code & ec)
                         noexcept
{
    const std::vector<sstring> vs { split_path(par_pt, osrc_pt, op, ec) };
    if (ec) {
        pr_err(-1, "{}: split_path({}, {}) failed{}\n", __func__, s(par_pt),
               s(osrc_pt), l(ec));
        return false;
    }
    // par_pt is most likely associated with the target (directory) of a
    // symlink and a component of its par_pt may match an element of
    // op->excl_fn_v . If so do nothing (return pair of nullptr_s).
    if (! op->excl_fn_v.empty()) {
        bool excl_fn_match { };
        for (const auto & c : vs) {
            if (std::ranges::binary_search(op->excl_fn_v, c)) {
                excl_fn_match = true;
                break;
            }
        }
        if (excl_fn_match) {
            pr_err(3, "{}: component of parent path: {} matches an EFN, "
                   "skip\n", __func__, s(par_pt));
            return false;
        }
    }

    inmem_dir_t * prev_odirp = l_odirp;
    l_odirp = odirp;

    for (const auto & ss : vs) {
        const auto & mm { l_odirp->sdirs_sp->sdir_fn_ind_m };
        const auto it { mm.find(ss) };

        if (it == mm.end()) {
            ec.assign(ENOENT, std::system_category());
            pr_err(1, "{} {}: unable to find that sub-path{}\n", s(par_pt),
                   ss, l());
            l_odirp = prev_odirp;
            return false;
        }
        inmem_t * childp { &l_odirp->sdirs_sp->sdir_v[it->second] };
        l_odirp = std::get_if<inmem_dir_t>(childp);
        if (l_odirp == nullptr) {
            ec.assign(ENOTDIR, std::system_category());
            pr_err(1, "{} {}: node was not sub-directory{}\n", s(par_pt),
                   ss, l());
            l_odirp = prev_odirp;
            return false;
        }
    }
    return true;
}

// This function will mark all nodes from par_pt to osrc_pt with the
// prune_up_chain unless nodes are already marked. Similar algorithm
// to the cache_recalc_grandparent function.
// Returns two pointers in a pair which can both be nullptr_s or one,
// but not both, is a valid pointer. Part of pass 2.
static std::pair<inmem_dir_t *, inmem_regular_t *>
prune_mark_up_chain(const fs::path & par_pt, const struct opts_t * op,
                    std::error_code & ec) noexcept
{
    inmem_dir_t * l_odirp { };
    struct stats_t * q { &op->mutp->stats };
    std::pair<inmem_dir_t *, inmem_regular_t *> res { };
    const fs::path & osrc_pt { op->source_pt };

    const std::vector<sstring> vs { split_path(par_pt, osrc_pt, op, ec) };
    if (ec) {
        pr_err(-1, "{}: split_path({}) failed{}\n", __func__, s(par_pt),
               l(ec));
        return res;     // pair of nullptr_s
    }
    l_odirp = op->mutp->cache_rt_dirp;
    if (l_odirp->prune_mask == 0) {
        l_odirp->prune_mask |= prune_up_chain;
        ++q->num_pruned_node;
    }
    // step down hierarchy from source root
    for (const auto & ss : vs) {
        const auto & mm { l_odirp->sdirs_sp->sdir_fn_ind_m };
        const auto it { mm.find(ss) };

        if (it == mm.end()) {   // not there, or not a directory
            for (auto & sub : l_odirp->sdirs_sp->sdir_v) {
                if (sub.get_filename() == ss) {
                    res.second = std::get_if<inmem_regular_t>(&sub);
                    if (res.second)
                        return res;
                }
            }
            ec.assign(ENOENT, std::system_category());
            pr_err(1, "{}: path: {}, component: {} not found{}\n", __func__,
                   s(par_pt), ss, l());
            return res;         // pair of nullptr_s
        }
        inmem_t * childp { &l_odirp->sdirs_sp->sdir_v[it->second] };

        l_odirp = std::get_if<inmem_dir_t>(childp);
        if (l_odirp == nullptr) {
            ec.assign(ENOTDIR, std::system_category());
            pr_err(0, "node: {} was not sub-directory in: {}{}\n", ss,
                   s(par_pt), l());
            return res;
        }
        inmem_base_t * ibp = childp->get_basep();
        if (ibp->prune_mask == 0) {
            ibp->prune_mask |= prune_up_chain;
            ++q->num_pruned_node;
        }
    }
    res.first = l_odirp;
    return res;
}

// This function is the pass 1 when --cache given or implied. It scans
// the source tree and forms an in-memory representation of it.
// Exclusions, either from -exclude= or --excl-fn= , reduce the number
// of nodes that would otherwise be placed in the in-memory tree.
// Note: cache_src() may be called recursively via the symlink_cache_src()
// function when dereferencing the symlink's target.
static std::error_code
cache_src(inmem_dir_t * start_dirp, const fs::path & osrc_pt,
          const struct opts_t * op) noexcept
{
    struct mut_opts_t * omutp { op->mutp };
    bool cache_src_first = ! omutp->cache_src_subseq;
    bool possible_exclude
                { cache_src_first && (! omutp->glob_exclude_v.empty()) };
    bool possible_excl_fn { ! op->excl_fn_v.empty() };
    bool possible_deref { cache_src_first && (! omutp->deref_v.empty()) };
    bool possible_prune { cache_src_first && (! omutp->prune_v.empty()) };
    int depth;
    int prev_depth { -1 };    // assume descending into directory
    int prev_dir_ind { -1 };
    inmem_dir_t * l_odirp { start_dirp };
    inmem_dir_t * prev_odirp { };
    struct stats_t * q { &omutp->stats };
    std::error_code ecc { };
    short_stat a_shstat;

    if (l_odirp == nullptr) {
        pr_err(-1, "odirp is null ?{}\n", l());
        ecc.assign(EINVAL, std::system_category());
        return ecc;
    }
    if (omutp->cache_src_subseq) {
        if (op->do_extra) {
            bool src_pt_contained { path_contains_canon(op->source_pt,
                                                        osrc_pt) };

            if (! src_pt_contained) {
                pr_err(-1, "{}: src: {} NOT contained{}{}\n", __func__,
                       s(osrc_pt), l());
                ecc.assign(EDOM, std::system_category());
                return ecc;
            }
        }
        /* assume osrc_pt is a directory */
    } else {
        omutp->cache_src_subseq = true;

        if (possible_prune) {        // for prune on root node
            bool rt_prune_exact { };

            std::tie(rt_prune_exact, possible_prune) =
                    find_in_sorted_vec(omutp->prune_v, op->source_pt, true);
            if (rt_prune_exact) {
                omutp->prune_take_all = true;
                ++q->num_prune_exact;
            }
        }
    }

    const fs::recursive_directory_iterator end_itr { };

    for (fs::recursive_directory_iterator itr(osrc_pt, dir_opt, ecc);
         (! ecc) && itr != end_itr;
         itr.increment(ecc), prev_depth = depth) {

        std::error_code ec { };
        // since osrc_pt is in canonical form, assume entry.path()
        // will either be in canonical form, or absolute form if symlink
        const auto & pt = itr->path();
        const auto & pt_s { s(pt) };
        prev_rdi_pt = pt;
        const sstring filename { pt.filename() };
        const auto par_pt = pt.parent_path();
        depth = itr.depth();
        bool exclude_entry { false };
        bool deref_entry { false };
        bool got_prune_exact { false };

        const auto s_sym_ftype { itr->symlink_status(ec).type() };
        if (ec) {       // serious error
            ++q->num_error;
            pr_err(2, "symlink_status({}) failed, continue{}\n", s(pt),
                   l(ec));
            // thought of using entry.refresh(ec) but no speed improvement
            continue;
        }
        const auto l_isdir = (s_sym_ftype == fs::file_type::directory);
        if (depth > prev_depth) {
            if (depth == (prev_depth + 1))
                prev_odirp = l_odirp;
            else
                prev_odirp = nullptr;
            if (prev_dir_ind >= 0) {
                l_odirp = std::get_if<inmem_dir_t>(
                        &l_odirp->sdirs_sp->sdir_v[prev_dir_ind]);
            } else
                pr_err(5, "{}: probably source root because blank: {}{}\n",
                       __func__, l_odirp->filename, l());
        } else if (depth < prev_depth) {   // should never occur on first iter
            if (depth == (prev_depth - 1) && prev_odirp) {
                l_odirp = prev_odirp;   // short cut if backing up one
                prev_odirp = nullptr;
            } else {    // need to recalculate parent's dirp
                bool ok = cache_recalc_grandparent(par_pt, osrc_pt,
                                                   start_dirp, l_odirp, op,
                                                   ec);
                if (ok)
                    prev_odirp = nullptr;
                else {
                    pr_err(-1, "{}: cannot find {} in {}, fatal\n", __func__,
                           s(par_pt), s(osrc_pt));
                    return ec;
                }
            }
        }

        ++q->num_node;
        // if (q->num_node >= 240000)
            // break;
        pr_err(6, "about to scan this source entry: {}{}\n", s(pt), l());
        if (depth > q->max_depth)
            q->max_depth = depth;
        if (op->max_depth_active && l_isdir && (depth >= op->max_depth)) {
            pr_err(2, "Source: {} at max_depth: {}, don't enter\n",
                   s(pt), depth);
            itr.disable_recursion_pending();
        }
        struct stat a_stat;

        if (lstat(pt.c_str(), &a_stat) < 0) {
            ec.assign(errno, std::system_category());
            pr_err(-1, "lstat({}) failed{}\n", s(osrc_pt), l(ec));
            ++q->num_error;
            continue;
        }
        bool is_symlink { (a_stat.st_mode & S_IFMT) == S_IFLNK };
        a_shstat.st_dev = a_stat.st_dev;
        a_shstat.st_mode = a_stat.st_mode;
        auto s_ftype { itr->status(ec).type() };
        if (ec) {
            if (is_symlink && (ec.value() == ENOENT)) {
                s_ftype = fs::file_type::none;
                ++q->num_sym_s_dangle;
            } else {
                ++q->num_error;
                pr_err(2, "itr->status({}) failed, continue{}\n", s(pt),
                       l(ec));
                continue;
            }
        }

        const bool hidden_entry { ((! pt.empty()) && (filename[0] == '.')) };
        if (possible_deref && (s_sym_ftype == fs::file_type::symlink)) {
            std::tie(deref_entry, possible_deref) =
                        find_in_sorted_vec(omutp->deref_v, pt_s, true);
            if (deref_entry) {
                ++q->num_derefed;
                pr_err(3, "{}: matched for dereference{}\n", s(pt), l());
            }
        }
        if (! deref_entry) {    // deref trumps exclude
            if (possible_exclude) {
                std::tie(exclude_entry, possible_exclude) =
                    find_in_sorted_vec(omutp->glob_exclude_v, pt_s, true);
                if (exclude_entry) {
                    ++q->num_excluded;
                    pr_err(3, "{}: matched for exclusion{}\n", s(pt), l());
                }
            }
            if (possible_excl_fn) {
                if (std::ranges::binary_search(op->excl_fn_v, filename)) {
                    ++q->num_excl_fn;
                    exclude_entry = true;
                    pr_err(3, "{}: matched {} for excl_fn{}\n", __func__,
                           s(pt), l());
                }
            }
        }
        if (possible_prune && ((s_sym_ftype == fs::file_type::directory) ||
                               (s_sym_ftype == fs::file_type::symlink) ||
                               (s_sym_ftype == fs::file_type::regular))) {
            bool prune_entry { };

            std::tie(prune_entry, possible_prune) =
                        find_in_sorted_vec(omutp->prune_v, pt_s, true);
            if (prune_entry)
                got_prune_exact = true;
        }
        if (op->want_stats > 0)
            update_stats(s_sym_ftype, s_ftype, hidden_entry, op);
        if (l_isdir) {
            if (exclude_entry) {
                itr.disable_recursion_pending();
                continue;
            }
            if (op->max_depth_active && (depth >= op->max_depth)) {
                pr_err(2, "Source at max_depth={} and this is a directory: "
                       "{}, don't enter{}\n", depth, s(pt), l());
                itr.disable_recursion_pending();
                continue;
            }
            if (! op->no_xdev) { // double negative ...
                if (a_stat.st_dev != omutp->starting_fs_inst) {
                    // do not visit this sub-branch: different fs instance
                    pr_err(1, "Source trying to leave this fs instance at: "
                           "{}{}\n", s(pt), l());
                    itr.disable_recursion_pending();
                    ++q->num_oth_fs_skipped;
                    /* create this directory as possible mount point */
                }
            }
        } else if (exclude_entry)
            continue;

        if ((! op->clone_hidden) && hidden_entry) {
            ++q->num_hidden_skipped;
            if (s_sym_ftype == fs::file_type::directory)
                itr.disable_recursion_pending();
            continue;
        }

        switch (s_sym_ftype) {
        using enum fs::file_type;

        case symlink:
            {
                bool serious { };

                std::tie(ec, serious) =
                    symlink_cache_src(pt, a_shstat, l_odirp, prev_odirp,
                                      deref_entry, got_prune_exact, op);
                if (serious)
                    return ec;
            }
            break;
        case directory:
            {
                inmem_dir_t a_dir(filename, a_shstat);
                a_dir.par_pt_s = s(par_pt);
                a_dir.depth = depth;
                if (got_prune_exact) {
                    a_dir.prune_mask |= prune_exact;
                    ++q->num_prune_exact;
                }
                prev_dir_ind = l_odirp->add_to_sdir_v(a_dir);
            }
            break;
        case block:
            {
                inmem_device_t a_dev(filename, a_shstat);
                a_dev.is_block_dev = true;
                a_dev.st_rdev = a_stat.st_rdev;
                l_odirp->add_to_sdir_v(a_dev);
            }
            break;
        case character:
            {
                inmem_device_t a_dev(filename, a_shstat);
                // a_dev.is_block_dev = false;
                a_dev.st_rdev = a_stat.st_rdev;
                l_odirp->add_to_sdir_v(a_dev);
            }
            break;
        case fifo:
            pr_err(0, "{}: file type: socket not supported{}\n", s(pt), l());
            break;              // skip this file type
        case socket:
            pr_err(0, "{}: file type: fifo not supported{}\n", s(pt), l());
            break;              // skip this file type
        case regular:
            cache_reg(l_odirp, a_shstat, pt, got_prune_exact, op);
            break;
        default:
            {
                inmem_other_t a_oth(filename, a_shstat);
                l_odirp->add_to_sdir_v(a_oth);
            }
            break;
        }
    }
    if (ecc) {
        ++q->num_scan_failed;
        pr_err(-1, "recursive_directory_iterator() failed, prior entry: "
               "{}{}\n", s(prev_rdi_pt), l(ecc));
    }
    return ecc;
}

static size_t
count_cache(const inmem_dir_t * odirp, bool recurse,
            const struct opts_t * op) noexcept
{
    size_t sz, k;

    if (odirp == nullptr)
        return 0;
    sz = odirp->sdirs_sp->sdir_v.size();
    if (! recurse) {
        return sz;
    }
    for (k = 0; const auto & sub : odirp->sdirs_sp->sdir_v) {
        const auto * cdirp { std::get_if<inmem_dir_t>(&sub) };

        ++k;
        if (cdirp)
            k += count_cache(cdirp, recurse, op);
    }
    return k;
}

// For simplicity and speed, exclusions and dereferences are ignored.
static void
depth_count_cache(const inmem_dir_t * odirp, std::vector<size_t> & ra,
                  int depth = -1) noexcept
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
        if (const auto * cdirp { std::get_if<inmem_dir_t>(&sub) }) {
            depth_count_cache(cdirp, ra, cdirp->depth);
        }
    }
}

// For simplicity and speed, exclusions and dereferences are ignored.
static void
depth_count_src(const fs::path & src_pt, std::vector<size_t> & ra) noexcept
{
    size_t v_sz;
    fs::path prev_pt { src_pt };
    std::error_code ecc { };
    const fs::recursive_directory_iterator end_itr { };

    for (fs::recursive_directory_iterator itr(src_pt, dir_opt, ecc);
         (! ecc) && itr != end_itr;
         itr.increment(ecc) ) {

        // since src_pt is in canonical form, assume entry.path()
        // will either be in canonical form, or absolute form if symlink
        std::error_code ec { };
        const fs::path pt { itr->path() };
        const auto depth { itr.depth() };

        if (depth >= 0) {
            v_sz = ra.size();
            if (static_cast<size_t>(depth) >= v_sz)
                ra.push_back(1);    // will always be 1 greater than
            else
                ++ra[depth];
        }
    }
    if (ecc)
        pr_err(-1, "path: {} prior to depth_count_src() failure{}\n",
               s(prev_pt), l());
}

static void
show_cache_not_dir(const inmem_t & a_nod,
                   [[maybe_unused]] const struct opts_t * op) noexcept
{
    if (const auto * cothp { std::get_if<inmem_other_t>(&a_nod) }) {
        pr_err(-1, "  other filename: {}\n", cothp->filename);
    } else if (const auto * csymp {
               std::get_if<inmem_symlink_t>(&a_nod) }) {
        pr_err(-1, "  symlink link name: {}  target filename: {}\n",
               csymp->filename, s(csymp->target));
    } else if (const auto * cregp { std::get_if<inmem_regular_t>(&a_nod) }) {
        pr_err(4, "{}: &a_nod={:p}\n", __func__, (void *)&a_nod);
        pr_err(-1, "  regular filename: {}\n", cregp->filename);
    } else if (const auto * cdevp {
               std::get_if<inmem_device_t>(&a_nod) }) {
        if (cdevp->is_block_dev)
            pr_err(-1, "  block device filename: {}\n", cdevp->filename);
        else
            pr_err(-1, "  char device filename: {}\n", cdevp->filename);
    } else if (const auto * cfsp {
               std::get_if<inmem_fifo_socket_t>(&a_nod) }) {
        pr_err(-1, "  fifo/socket filename: {}\n", cfsp->filename);
    } else
        pr_err(-1, "  unknown type\n");
    pr_err(5, "    &inmem_t: {:p}\n", (void *)&a_nod);
}

// Note that this function will call itself recursively if recurse is true
static void
show_cache(const inmem_t & a_nod, bool recurse,
           const struct opts_t * op) noexcept
{
    const inmem_dir_t * dirp { std::get_if<inmem_dir_t>(&a_nod) };

    if (dirp == nullptr) {
        show_cache_not_dir(a_nod, op);
        pr_err(-1, "\n");       // blank line separator
        return;
    }
    // pr_err(-1, "\n");        // blank line separator
    pr_err(-1, "<< directory: {}/{}, depth={} >>\n", dirp->par_pt_s,
           dirp->filename, dirp->depth);
    pr_err(5, "    &inmem_t: {:p}\n", (void *)&a_nod);
    a_nod.debug();

    for (const auto & subd : dirp->sdirs_sp->sdir_v) {
        const auto * cdirp { std::get_if<inmem_dir_t>(&subd) };

        if (recurse) {
            if (cdirp) {
                show_cache(subd, recurse, op);
                pr_err(-1, " << return to directory: {}/{} >>\n",
                       dirp->par_pt_s, dirp->filename);
            } else {
                pr_err(-1, "{}:\n", __func__);
                subd.debug();
                pr_err(5, "    &inmem_t: {:p}\n", (void *)&subd);
                pr_err(-1, "\n");       // blank line separator
            }
        } else {
            show_cache_not_dir(subd, op);
            pr_err(-1, "\n");   // blank line separator
        }
    }
}

// Transforms a source path to the corresponding destination path
static sstring
transform_src_pt2dst(const sstring & src_pt_s,
                     const struct opts_t * op) noexcept
{
    if (src_pt_s.size() < op->mutp->starting_src_sz)
        return "";
    return s(op->destination_pt) +
       sstring(src_pt_s.begin() + op->mutp->starting_src_sz, src_pt_s.end());
}

static std::error_code
unroll_cache_not_dir(const sstring & s_pt_s, const sstring & d_pt_s,
                     const inmem_t & a_nod, const struct opts_t * op) noexcept
{
    int res { };
    std::error_code ec { };
    struct stats_t * q { &op->mutp->stats };

    if (const auto * cothp { std::get_if<inmem_other_t>(&a_nod) }) {
        pr_err(-1, "  other filename: {}\n", cothp->filename);
    } else if (const auto * csymp {
               std::get_if<inmem_symlink_t>(&a_nod) }) {
        fs::create_symlink(csymp->target, d_pt_s, ec);
        if (ec) {
            pr_err(1,"{} --> {}: create_symlink() failed{}\n", d_pt_s,
                   s(csymp->target), l(ec));
            ++q->num_error;
        } else {
            ++q->num_sym_d_success;
            pr_err(5,"{} --> {}: create_symlink() ok\n", d_pt_s,
                   s(csymp->target));
            if (op->do_extra > 0) {
                fs::path par_pt { fs::path(d_pt_s).parent_path() };
                fs::path abs_targ_pt =
                        fs::weakly_canonical(par_pt / csymp->target, ec);
                if (ec)
                    pr_err(2, "weakly_canonical({}) failed{}\n",
                           s(par_pt / csymp->target), l());
                else {
                    if (fs::exists(abs_targ_pt, ec))
                        pr_err(5, "symlink target: {} exists{}\n",
                               s(abs_targ_pt), l());
                    else if (ec) {
                        pr_err(0, "fs::exists({}) failed{}\n", s(abs_targ_pt),
                               l(ec));
                        ++q->num_error;
                    } else
                        ++q->num_sym_d_dangle;
                }
            }
        }
    } else if (const auto * cdevp
               { std::get_if<inmem_device_t>(&a_nod) }) {
        res = xfr_dev_inmem2file(*cdevp, d_pt_s, op);
        if (res) {
            ec.assign(res, std::system_category());
            pr_err(4, "{}: failed to write dev file: {}{}\n", __func__,
                   d_pt_s, l(ec));
        }
    } else if (const auto * cregp
                 { std::get_if<inmem_regular_t>(&a_nod) } ) {
        if (cregp->always_use_contents || (op->cache_op_num > 1))
            res = xfr_reg_inmem2file(*cregp, d_pt_s, op);
        else if (op->cache_op_num == 1)
            res = xfr_reg_file2file(s_pt_s, d_pt_s, op);
        if (res) {
            ec.assign(res, std::system_category());
            pr_err(4, "{}: failed to write dst regular file: {}{}\n",
                   __func__, d_pt_s, l(ec));
        }
    } else if (const auto * cfsp
                 { std::get_if<inmem_fifo_socket_t>(&a_nod) }) {
        pr_err(-1, "{}:  fifo/socket filename: {}\n", __func__,
               cfsp->filename);
    } else
        pr_err(-1, "{}:  unknown type\n", __func__);
    pr_err(5, "    &inmem_t: {:p}\n", (void *)&a_nod);
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
            ++q->num_dir_d_fail;
            pr_err(1, "{}: create_directory({}), depth={} failed{}\n",
                   __func__, dst_pt_s, dirp->depth, l(ec));
        } else if (dst_pt_s != op->destination_pt) {
            ++q->num_dir_d_exists;
            pr_err(2, "{}, depth={}: exists so create_directory() ignored\n",
                   dst_pt_s, dirp->depth);
        }
    } else
        ++q->num_dir_d_success;
    return ec;
}

// Unroll cache into the destination. This function calls itself recursively.
// This is the last pass (second or third) when the --cache or --prune=
// option is used.
static std::error_code
unroll_cache(const inmem_t & a_nod, const sstring & s_par_pt_s, bool recurse,
             const struct opts_t * op) noexcept
{
    std::error_code ec { };

    const auto sz { s_par_pt_s.size() };
    const fs::path src_dir_pt { ((sz == 1) && (s_par_pt_s[0] == '/'))
                                ? '/' + a_nod.get_filename()
                                : s_par_pt_s + '/' + a_nod.get_filename() };
    // following suppresses nuisance '/'is such as in '//sys'
    sstring src_dir_pt_s { src_dir_pt.lexically_normal() };
    sstring dst_dir_pt_s { transform_src_pt2dst(src_dir_pt_s, op) };
    if (op->prune_given && (a_nod.get_basep()->prune_mask == 0)) {
        pr_err(6, "leaving unroll_cache({}){}\n", s(src_dir_pt_s), l());
        return ec;
    }
    const inmem_dir_t * dirp { std::get_if<inmem_dir_t>(&a_nod) };

    if (dirp == nullptr)
        return unroll_cache_not_dir(src_dir_pt_s, dst_dir_pt_s, a_nod, op);

    ec = unroll_cache_is_dir(dst_dir_pt_s, dirp, op);
    if (ec)
        return ec;

    for (const auto & subd : dirp->sdirs_sp->sdir_v) {
        if (op->prune_given && (subd.get_basep()->prune_mask == 0))
            continue;
        const auto * cdirp { std::get_if<inmem_dir_t>(&subd) };

        if (cdirp && recurse) {
            ec = unroll_cache(subd, src_dir_pt_s, recurse, op);
            if (ec)
                break;
        } else {
            const auto & fn {subd.get_filename() };
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

// Process symlink whose target is a directory or regular file during pass 2.
// Returns true for success or false for skip this symlink and process next.
static bool
prune_prop_symlink(const inmem_symlink_t * csymp,
                   const sstring & src_dir_pt_s,
                   const struct opts_t * op) noexcept
{
    std::error_code ec { };
    struct stats_t * q { &op->mutp->stats };

    if (csymp->prune_mask == 0)
        ++q->num_pruned_node;
    csymp->prune_mask |= prune_up_chain;
    fs::path target = csymp->target;
    if (target.is_relative())
        target = fs::path(src_dir_pt_s) / target;
    // target = target.lexically_normal();
    fs::path target_c = fs::canonical(target, ec);
    if (ec) {
        pr_err(1, "prune bad symlink target path: {}{}\n", s(target), l(ec));
        ++q->num_prune_sym_pt_err;
        return false;
    }
    if (path_contains_canon(op->source_pt, target_c)) {
        bool at_src_rt { };
        fs::path p_target { target_c.parent_path() };
        std::pair<inmem_dir_t *, inmem_regular_t *> res { };

        if (op->source_pt == target_c) {   // this is ugly
            at_src_rt = true;
            pr_err(0, "{}: symlink target is source root, ignore\n",
                   __func__);
        } else {
            res = prune_mark_up_chain(target_c, op, ec);
            if (ec) {
                ++q->num_prune_sym_pt_err;
                return false;
            }
        }
        if (res.second) {       // symlink -> regular file
            res.second->prune_mask |= prune_all_below;
            return true;
        }
        inmem_dir_t * l_odirp = res.first;
        if (l_odirp) {          // symlink -> directory
            if ((l_odirp->prune_mask & prune_all_below) || at_src_rt)
                return false;
            prune_prop_dir(l_odirp, p_target, true, op);
            return true;
        }
    } else {
        ++q->num_prune_sym_outside;
        pr_err(1, "prune symlink target path: {} outside SPATH{}\n",
               s(target_c), l());
    }
    return false;
}

// Process regular file during pass 2.
static void
prune_prop_reg(const inmem_regular_t * a_regp, const sstring & s_par_pt_s,
               bool in_prune, const struct opts_t * op) noexcept
{
    // bool start_in_prune { };
    std::error_code ec { };
    struct stats_t * q { &op->mutp->stats };

    if (in_prune) {
        if (a_regp->prune_mask == 0)
            ++q->num_pruned_node;
        if (a_regp->prune_mask & prune_all_below)
            return;
        if (a_regp->prune_mask & prune_up_chain)
            a_regp->prune_mask &= ~prune_up_chain;
        else {
            prune_mark_up_chain(s_par_pt_s, op, ec);
            if (ec) {
                ++q->num_prune_err;
                return;
            }
        }
        a_regp->prune_mask |= prune_all_below;
    } else {
        if (a_regp->prune_mask & prune_exact) {
            a_regp->prune_mask |= prune_all_below;
            ++q->num_pruned_node;
            prune_mark_up_chain(s_par_pt_s, op, ec);
            if (ec) {
                ++q->num_prune_err;
                return;
            }
        }
    }
}

// This is the main function of pass 2 active when the --prune= option is
// given. Prune propagate (for a directory) searches for prune_exact marks
// (that were set in pass 1). For each match, additionally marks are added
// up the in-memory tree (i.e. toward the root). These are styled as
// 'prune_up_chain' marks. Also all the nodes in the sub-tree below each
// match are styled as 'prune_all_below' marks.
// Expects a_nod to be a directory. s_par_pt_s is the parent path of a_nod .
static void
prune_prop_dir(const inmem_dir_t * a_dirp, const sstring & s_par_pt_s,
               bool in_prune, const struct opts_t * op) noexcept
{
    bool at_src_rt { };
    // bool start_in_prune { };
    std::error_code ec { };
    struct stats_t * q { &op->mutp->stats };
    sstring src_dir_pt_s { s_par_pt_s };
    const sstring & a_dir_fn_s { a_dirp->filename };

    if (a_dirp->is_root)
        at_src_rt = true;
    else
        src_dir_pt_s += '/' + a_dir_fn_s;
    if (in_prune) {
        if (a_dirp->prune_mask & prune_all_below)
            return;
        if (a_dirp->prune_mask == 0)
            ++q->num_pruned_node;
        if (! at_src_rt) {
            if (a_dirp->prune_mask & prune_up_chain)
                a_dirp->prune_mask &= ~prune_up_chain;
            else {
                prune_mark_up_chain(s_par_pt_s, op, ec);
                if (ec) {
                    ++q->num_prune_err;
                    return;
                }
            }
        }
        a_dirp->prune_mask |= prune_all_below;
    } else {
        if (a_dirp->prune_mask & prune_exact) {
            in_prune = true;
            ++q->num_pruned_node;
            a_dirp->prune_mask |= prune_all_below;
            if (! at_src_rt) {
                prune_mark_up_chain(s_par_pt_s, op, ec);
                if (ec) {
                    ++q->num_prune_err;
                    return;
                }
            }
        }
    }

    for (auto & subd : a_dirp->sdirs_sp->sdir_v) {
        inmem_base_t * sibp = subd.get_basep();
        if (sibp->prune_mask & prune_all_below)
            continue;       // all done here and below
        else if (in_prune) {
            if (sibp->prune_mask & prune_up_chain)
                sibp->prune_mask &= ~prune_up_chain;    // clear it
        }
        if (const auto * dirp { std::get_if<inmem_dir_t>(&subd) }) {
            prune_prop_dir(dirp, src_dir_pt_s, in_prune, op);
        } else if (const auto * csymp
                            { std::get_if<inmem_symlink_t>(&subd) }) {
            if (in_prune) {
                if (! prune_prop_symlink(csymp, src_dir_pt_s, op))
                    continue;
            } else if (csymp->prune_mask & prune_exact) {
                if (! (sibp->prune_mask & prune_all_below))
                    sibp->prune_mask |= prune_up_chain;
                if (! prune_prop_symlink(csymp, src_dir_pt_s, op))
                    continue;
                prune_mark_up_chain(src_dir_pt_s, op, ec);
            }
        } else if (const auto * cregp {std::get_if<inmem_regular_t>(&subd) })
            prune_prop_reg(cregp, src_dir_pt_s, in_prune, op);
        else  if (in_prune) {
            if (sibp->prune_mask == 0)
                ++q->num_pruned_node;
            sibp->prune_mask |= prune_all_below;
        }
    }       // end of range based loop on sdir_v
}

// Called from main(). Starts single pass clone/copy when neither the --cache
// nor --prune= option is given.
static std::error_code
do_clone(const struct opts_t * op) noexcept
{
    std::error_code ec { };
    struct mut_opts_t * omutp { op->mutp };
    struct stats_t * q { &omutp->stats };
    auto ch_start { chron::steady_clock::now() };
    struct stat root_stat { };

    if (stat(op->source_pt.c_str(), &root_stat) < 0) {
        ec.assign(errno, std::system_category());
        return ec;
    }
    omutp->starting_fs_inst = root_stat.st_dev;
    q->num_node = 1;    // count the source root node
    ec = clone_work(op->source_pt, op->destination_pt, op);
    if (ec)
        pr_err(-1, "problem with clone_work({}){}\n", s(op->source_pt),
                l(ec));

    if (op->do_extra) {
        std::vector<size_t> ra;
        depth_count_src(op->source_pt, ra);
        pr_err(-1, "Depth count of source:\n");
        for (int d { 0 }; auto k : ra) {
            pr_err(-1,  "  {}: {}\n", d, k);
            ++d;
        }
    }

    auto ch_end { chron::steady_clock::now() };
    auto ms { chron::duration_cast<chron::milliseconds>
                                        (ch_end - ch_start).count() };
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

// Called from main(). Starts pass 1 (caching) when --cache or --prune=
// option is given. Also invokes pass 2 if op->prune_given and then invokes
// the unroll (in-memory tree rolled out into the destination) which is the
// final pass.
static std::error_code
do_cache(const inmem_t & src_rt_cache, const struct opts_t * op) noexcept
{
    int pass { 1 };
    std::error_code ec { };
    uint8_t * sbrk_p { static_cast<uint8_t *>(sbrk(0)) };
    struct mut_opts_t * omutp { op->mutp };
    struct stats_t * q { &omutp->stats };
    auto ch_start { chron::steady_clock::now() };

    q->num_node = 1;    // count the source root node
    pr_err(5, "\n{}: >> start of pass {} (cache source)\n", __func__, pass);
    ec = cache_src(omutp->cache_rt_dirp, op->source_pt, op);
    if (ec)
        pr_err(-1, "{}: problem with cache_src({}){}\n", __func__,
               s(op->source_pt), l(ec));

    auto ch_end { chron::steady_clock::now() };
    auto ms
        { chron::duration_cast<chron::milliseconds>
                                        (ch_end - ch_start).count() };
    auto total_ms { ms };
    auto secs { ms / 1000 };
    auto ms_remainder { ms % 1000 };
    char b[32];
    snprintf(b, sizeof(b), "%d.%03d", static_cast<int>(secs),
             static_cast<int>(ms_remainder));
    scout << "Caching time: " << b << " seconds\n";

    bool skip_destin = op->no_destin;

    if (op->prune_given) {
        if (q->num_prune_exact > 0) {
            auto start_of_prune { ch_end };

            ++pass;
            pr_err(5, "\n{}: >> start of pass {} (prune propagate)\n",
                   __func__, pass);
            prune_prop_dir(omutp->cache_rt_dirp, s(op->source_pt),
                           omutp->prune_take_all, op);
            ch_end = chron::steady_clock::now();
            ms = chron::duration_cast<chron::milliseconds>
                                        (ch_end - start_of_prune).count();
            total_ms += ms;
            secs = ms / 1000;
            ms_remainder = ms % 1000;
            snprintf(b, sizeof(b), "%d.%03d", static_cast<int>(secs),
                     static_cast<int>(ms_remainder));
            scout << "Prune propagate time: " << b << " seconds\n";
        } else {
            pr_err(-1, "prune requested but no nodes found so no output\n");
            skip_destin = true;
        }
    }
    ++pass;
    pr_err(5, "\n{}: >> start of pass {} (unroll)\n", __func__, pass);
    auto start_of_unroll { ch_end };
    bool do_unroll { false };
    if (! skip_destin) {
        do_unroll = true;
        ec = unroll_cache(src_rt_cache, op->source_pt.parent_path(),
                          true, op);
        if (ec)
            pr_err(0, "unroll_cache() failed{}\n", l(ec));
    }

    if (op->do_extra) {
        long tree_sz { static_cast<const uint8_t *>(sbrk(0)) - sbrk_p };
        pr_err(-1, "Tree size: {} bytes\n", tree_sz);

        size_t counted_nodes { 1 + count_cache(omutp->cache_rt_dirp, false,
                                               op) };
        pr_err(-1, "Tree counted nodes: {} at top level\n", counted_nodes);

        counted_nodes = 1 + count_cache(omutp->cache_rt_dirp, true, op);
        pr_err(-1, "Tree counted nodes: {} [recursive]\n", counted_nodes);

        std::vector<size_t> ra;
        depth_count_cache(omutp->cache_rt_dirp, ra);
        pr_err(-1, "Depth count cache:\n");
        for (int d { 0 }; auto k : ra) {
            pr_err(-1, "  {}: {}\n", d, k);
            ++d;
        }
    }

    ch_end = chron::steady_clock::now();
    ms = chron::duration_cast<chron::milliseconds>
                                (ch_end - start_of_unroll).count();
    total_ms += ms;
    secs = ms / 1000;
    ms_remainder = ms % 1000;
    if (do_unroll) {
        snprintf(b, sizeof(b), "%d.%03d", static_cast<int>(secs),
                 static_cast<int>(ms_remainder));
        scout << "Cache unrolling time: " << b << " seconds\n";
    }
    secs = total_ms / 1000;
    ms_remainder = total_ms % 1000;
    snprintf(b, sizeof(b), "%d.%03d", static_cast<int>(secs),
             static_cast<int>(ms_remainder));
    scout << "Total processing time: " << b << " seconds\n";

    if (op->want_stats > 0)
        show_stats(op);
    return ec;
}

static void
run_unique_and_erase(std::vector<sstring> &v)
{
#if __clang__ && (__clang_major__ < 16)
// #warning ">>> got CLANG 15"
    // CLANG 15.x has issue with std::ranges::unique() so fallback
    // tp std::unique()
    const auto ret { std::unique(v.begin(), v.end()) };
    v.erase(ret, v.end());
#else
// #warning ">>> not CLANG 15"
    // std::ranges::unique() returns a sub-range
    const auto [new_log_end, old_end] { std::ranges::unique(v) };
    v.erase(new_log_end, old_end);
#endif
}

static int
parse_cmd_line(struct opts_t * op, int argc,  char * argv[])
{
    bool help_request { };
    std::error_code ec { };
    size_t sz;
    fs::path l_pt;

    while ( true ) {
        int option_index { 0 };
        int c { getopt_long(argc, argv, "cd:De:E:hHm:Np:r:R:s:SvVw:x",
                            long_options, &option_index) };
        if (c == -1)
            break;

        switch (c) {
        case 'c':
            ++op->cache_op_num;
            break;
        case 'd':
            if (op->destination_given) {
                pr_err(-1, "only one destination location option can be "
                       "given\n");
                return 1;
            }
            op->dst_cli = optarg;
            op->destination_given = true;
            break;
        case 'D':
            op->no_destin = true;
            break;
        case 'e':
            sz = strlen(optarg);
            if (sz > 1) {       // chop off any trailing '/'
                if ('/' == optarg[sz - 1]) {
                    sstring ss(optarg, sz - 1);
                    op->cl_exclude_v.push_back(ss);
                    break;
                }
            }
            op->cl_exclude_v.push_back(optarg);
            op->exclude_given = true;
            break;
        case 'E':
            if (strchr(optarg, '/')) {
                pr_err(-1, "{}: EFN must be a filename without a path, "
                       "ignore\n", optarg);
                break;
            }
            op->excl_fn_v.push_back(optarg);
            op->excl_fn_given = true;
            break;
        case 'h':
            help_request = true;
            break;
        case 'H':
            op->clone_hidden = true;
            break;
        case 'm':
            if (1 != sscanf(optarg, "%d", &op->max_depth)) {
                pr_err(-1, "unable to decode integer for "
                       "--max-depth=MAXD{}\n", l());
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
        case 'p':
            op->prune_given = true;
            if (fs::is_symlink(optarg, ec))
                l_pt = fs::absolute(optarg, ec);
            else if (! ec)
                l_pt = fs::canonical(optarg, ec);
            if (ec)
                pr_err(-1, "<< failed to find {}; ignored\n", optarg, l(ec));
            else
                op->mutp->prune_v.push_back(s(l_pt));
            break;
        case 'r':
            if (1 != sscanf(optarg, "%u", &op->reglen)) {
                pr_err(-1, "unable to decode integer for --reglen=RLEN{}\n",
                       l());
                return 1;
            }
            break;
        case 'R':
            op->deref_given = true;
            sz = strlen(optarg);
            if (sz > 1) {       // chop off any trailing '/'
                if ('/' == optarg[sz - 1]) {
                    sstring ss(optarg, sz - 1);
                    op->mutp->deref_v.push_back(ss);
                    break;
                }
            }
            op->mutp->deref_v.push_back(optarg);
            break;
        case 's':
            if (op->source_given) {
                pr_err(-1, "only one source location option can be given\n");
                return 1;
            }
            op->src_cli = optarg;
            op->source_given = true;
            break;
        case 'S':
            ++op->want_stats;
            break;
        case 'v':
            ++cpf_verbose;
            ++op->verbose;
            op->verbose_given = true;
            break;
        case 'V':
            op->version_given = true;
            break;
        case 'w':
            if (1 != sscanf(optarg, "%u", &op->wait_ms)) {
                pr_err(-1, "unable to decode integer for --wait=MS_R{}\n",
                       l());
                return 1;
            }
            op->wait_given = true;
            break;
        case 'x':
            ++op->do_extra;
            break;
        default:
            pr_err(-1, "unrecognised option code: 0x{:x}\n", c);
            usage();
            return 1;
        }
    }
    if (optind < argc) {
        if (optind < argc) {
            for (; optind < argc; ++optind)
                pr_err(-1, "Unexpected extra argument: {}\n\n", argv[optind]);
            usage();
            return 1;
        }
    }
    if (help_request) {
        usage();
        return -1;
    }
#ifdef DEBUG
    pr_err(-1, "In DEBUG mode, ");
    if (op->verbose_given && op->version_given) {
        pr_err(-1, "but override: '-vV' given, zero verbose and continue\n");
        op->verbose_given = false;
        op->version_given = false;
        op->verbose = 0;
        cpf_verbose = 0;
        want_sloc = false;
    } else if (! op->verbose_given) {
        pr_err(-1, "set '-vv'\n");
        if (op->verbose < 2) {
            op->verbose = 2;
            cpf_verbose = 2;
        }
        want_sloc = true;
    } else {
        pr_err(-1, "keep verbose={}\n", op->verbose);
        want_sloc = true;
    }
#else
    if (op->verbose_given && op->version_given)
        pr_err(-1, "Not in DEBUG mode, so '-vV' has no special action\n");
#endif
    if (op->version_given) {
        scout << version_str << "\n";
        return -1;
    }
    return 0;
}


int
main(int argc, char * argv[])
{
    bool ex_glob_seen { false };
    int res { };
    int glob_opt;
    std::error_code ec { };
    glob_t ex_paths { };
    struct opts_t opts { };
    struct opts_t * op = &opts;
    struct mut_opts_t mut_opts { };
    fs::path l_pt;

    op->mutp = &mut_opts;
    struct stats_t * q { &op->mutp->stats };
    op->reglen = def_reglen;

    res = parse_cmd_line(op, argc, argv);
    if (res)
        return (res < 0) ? 0 : res;

    // expect source to be either an existing directory or a symlink to
    // an existing directory.
    if (op->source_given) {
        auto sz = strlen(op->src_cli);
        if (sz > 1) {       // chop off any trailing '/'
            if ('/' == op->src_cli[sz - 1])
                --sz;
        }
        fs::path pt { sstring(op->src_cli, op->src_cli + sz) };

        if (pt.is_absolute())
            op->source_pt = pt.lexically_normal();
        else {
            op->source_pt = fs::absolute(pt, ec).lexically_normal();
            if (ec) {
                pr_err(-1, "fs::absolute({}) failed{}\n", s(pt), l(ec));
                return 1;
            }
        }
        auto str { s(op->source_pt) };
        sz = str.size();
        if ((sz > 1) && ('/' == str[sz - 1]))
             str.erase(str.end() - 1, str.end());
        op->source_pt = str;
    } else { // expect sysfs_root to be an absolute path
        op->source_pt = sysfs_root;
    }
    fs::file_status src_fstatus { fs::status(op->source_pt, ec) };

    if (ec) {
        pr_err(-1, "default SPATH: {} problem{}\n", s(op->source_pt), l(ec));
        return 1;
    }
    if (! fs::is_directory(src_fstatus)) {
        pr_err(-1, "expected SPATH: {} to be a directory, or a symlink to "
               "a directory\n", s(op->source_pt));
        return 1;
    }

    auto src_sz = s(op->source_pt).size();
    if ((src_sz == 1) && (s(op->source_pt)[0] == '/'))
        src_sz = 0;
    op->mutp->starting_src_sz = src_sz;

    if (! op->no_destin) {
        sstring d_str;

        if (op->destination_given) {
            auto sz = strlen(op->dst_cli);
            if (sz > 1) {       // chop off any trailing '/'
                if ('/' == op->dst_cli[sz - 1])
                    --sz;
            }
            d_str = sstring(op->dst_cli, op->dst_cli + sz);
        } else if (op->source_given) {
            pr_err(-1, "When --source= given, need also to give "
                   "--destination= (or --no-dst){}\n", l());
            return 1;
        } else
            d_str = def_destin_root;
        if (d_str.size() == 0) {
            pr_err(-1, "Confused, what is destination? [Got empty "
                   "string]{}\n", l());
            return 1;
        }
        fs::path d_pt { d_str };

        if (d_pt.is_relative()) {
            auto cur_pt { fs::current_path(ec) };
            if (ec) {
                pr_err(-1, "unable to get current path of destination, "
                       "exit{}\n", l(ec));
                return 1;
            }
            d_pt = cur_pt / d_pt;
        }
        if (d_pt.filename().empty())
            d_pt = d_pt.parent_path(); // to handle trailing / as in /tmp/sys/
        if (fs::exists(d_pt, ec)) {
            if (fs::is_directory(d_pt, ec)) {
                op->destination_pt = fs::canonical(d_pt, ec);
                if (ec) {
                    pr_err(-1, "canonical({}) failed{}\n", s(d_pt), l(ec));
                    return 1;
                }
            } else {
                pr_err(-1, "{}: is not a directory\n", s(d_pt), l());
                return 1;
            }
        } else {
            fs::path d_p_pt { d_pt.parent_path() };

            if (fs::exists(d_p_pt, ec) && fs::is_directory(d_p_pt, ec)) {
                // create destination directory at DPATH
                // no problem if already exists
                fs::create_directory(d_pt, ec);
                if (ec) {
                    pr_err(-1, "create_directory({}) failed{}\n", s(d_pt),
                           l(ec));
                    return 1;
                }
                pr_err(0, "In DPATH directory: {} created a new directory: "
                       "{}\n", s(d_p_pt), s(d_pt.filename()));
                op->destination_pt = fs::canonical(d_pt, ec);
                op->destin_all_new = true;
                if (ec) {
                    pr_err(-1, "canonical({}) failed{}\n", s(d_pt), l(ec));
                    return 1;
                }
            } else {
                pr_err(-1, "{}: needs to be an existing directory{}\n",
                       s(d_p_pt), l(ec));
                return 1;
            }
        }
        pr_err(5, "op->source_pt: {} , op->destination_pt: {}\n",
                    s(op->source_pt), s(op->destination_pt));
        if (op->source_pt == op->destination_pt) {
            pr_err(-1, "source: {}, and destination: {} seem to be the same. "
                   "That is not practical\n", s(op->source_pt),
                   s(op->destination_pt));
            return 1;
        }
    } else {
        if (op->destination_given) {
            pr_err(-1, "the --destination= and the --no-dst options "
                   "contradict, please pick one{}\n", l());
            return 1;
        }
        if (! op->mutp->deref_v.empty())
            pr_err(-1, "Warning: --dereference=SYML options ignored when "
                   "--no-destin option given\n");
    }

    if (op->reglen > def_reglen) {
        op->reg_buff_sp = std::make_shared<uint8_t []>((size_t)op->reglen, 0);
        if (! op->reg_buff_sp) {
            pr_err(-1, "Unable to allocate {} bytes on heap, use default "
                   "[{} bytes] instead\n", op->reglen, def_reglen);
            op->reglen = def_reglen;
        }
    }

    size_t ex_sz = 0;
    bool destin_excluded = false;

    // Creates a sorted vector (without duplicates) of paths that are to be
    // excluded. As SPATH is scanned, any hits lead to that path being
    // removed from op->mutp->glob_exclude_v . That is why that vector is
    // placed under op->mutp-> . This algorithm improves source scan time
    // as the number and size of the binary searches will move toward zero.
    // There may be downsides ...
    if (op->exclude_given) {
        bool excl_warning_issued {};

        for (const auto & ss : op->cl_exclude_v) {
            const char * ex_ccp = ss.c_str();

            if (ex_glob_seen)
                glob_opt = GLOB_APPEND;
            else {
                ex_glob_seen = true;
                glob_opt = 0;
            }
            res = glob(ex_ccp, glob_opt, nullptr, &ex_paths);
            if (res != 0) {
                if (res == GLOB_NOMATCH)
                    pr_err(-1, "Warning: --exclude={} did not match any "
                           "files, continue\n", ex_ccp);
                else
                    pr_err(-1, "glob() failed with --exclude={}, ignore\n",
                           ex_ccp);
                excl_warning_issued = true;
            }
        }

        if (ex_glob_seen) {
            for(size_t k { }; k < ex_paths.gl_pathc; ++k) {
                fs::path ex_pt { ex_paths.gl_pathv[k] };
                bool is_absol = ex_pt.is_absolute();

                if (! is_absol) {       // then make absolute
                    auto cur_pt { fs::current_path(ec) };
                    if (ec) {
                        pr_err(-1, "unable to get current path, "
                               "{} ignored{}\n", s(ex_pt), l(ec));
                        excl_warning_issued = true;
                        continue;
                    }
                    ex_pt = cur_pt / ex_pt;
                }
                fs::path c_ex_pt { fs::canonical(ex_pt, ec) };
                if (ec) {
                    excl_warning_issued = true;
                    pr_err(-1, "{}: exclude path rejected{}\n", s(ex_pt),
                           l(ec));
                } else {
                    if (path_contains_canon(op->source_pt, c_ex_pt)) {
                        op->mutp->glob_exclude_v.push_back(s(ex_pt));
                        pr_err(5, "accepted canonical exclude path: {}\n",
                               s(ex_pt));
                        if (c_ex_pt == op->destination_pt)
                            destin_excluded = true;
                    } else if (! excl_warning_issued) {
                        pr_err(-1, "ignored {} as not contained in source: "
                               "{}\n", s(ex_pt), s(op->source_pt));
                        excl_warning_issued = true;
                    }
                }
            }
            globfree(&ex_paths);
            ex_sz = op->mutp->glob_exclude_v.size();

            pr_err(1, "--exclude= argument matched {} files\n", ex_sz);
            if (ex_sz > 1) {
                if (! std::ranges::is_sorted(op->mutp->glob_exclude_v)) {
                    pr_err(2, "need to sort exclude vector{}\n", l());
                    std::ranges::sort(op->mutp->glob_exclude_v);
                }
                run_unique_and_erase(op->mutp->glob_exclude_v);
                ex_sz = op->mutp->glob_exclude_v.size(); // could be less now
                pr_err(0, "exclude vector size after sort then unique is "
                       "{}\n", ex_sz);
            }
        }
        if (excl_warning_issued)
            pr_err(-1, "\n");   // to help user see the warning
    }
    if (op->prune_given) {
        auto pr_sz = op->mutp->prune_v.size();
        pr_err(1, "--prune= argument(s) matched {} files\n", pr_sz);
        if (pr_sz > 1) {
            if (! std::ranges::is_sorted(op->mutp->prune_v)) {
                pr_err(2, "need to sort prune vector{}\n", l());
                std::ranges::sort(op->mutp->prune_v);
            }
            run_unique_and_erase(op->mutp->prune_v);
            pr_sz = op->mutp->prune_v.size(); // could be less now
            pr_err(0, "prune vector size after sort then unique is "
                       "{}\n", pr_sz);
        }
        bool prun_1_is_contained {};
        for (const auto & tt : op->mutp->prune_v) {
            prun_1_is_contained = path_contains_canon(op->source_pt, tt);
            if (prun_1_is_contained)
                break;
        }
        if (! prun_1_is_contained)
            pr_err(-1, "--prune= option given but argument(s) not contained "
                   "in source: {}\n", s(op->source_pt));
    }
    // Creates a sorted vector (without duplicates) of filenames (containing
    // no '/' characters). The resultant op->excl_fn_v vector is not modified
    // once this block has finished with it. During the source scan all
    // directory names and symlink (link) names are searched for in this
    //  vector. A binary search is done in each case.
    if (op->excl_fn_given) {
        ex_sz = op->excl_fn_v.size();
        pr_err(1, "--excl_fn= argument matched {} files\n", ex_sz);
        if (ex_sz > 1) {
            if (! std::ranges::is_sorted(op->excl_fn_v)) {
                pr_err(2, "need to sort excl_fn vector{}\n", l());
                std::ranges::sort(op->excl_fn_v);
            }
            run_unique_and_erase(op->excl_fn_v);
            ex_sz = op->excl_fn_v.size(); // could be less now
            pr_err(0, "excl_fn vector size after sort then unique is "
                       "{}\n", ex_sz);
        }
    }

    if (! op->no_destin) {
        if (path_contains_canon(op->source_pt, op->destination_pt)) {
            pr_err(-1, "Source contains destination, infinite recursion "
                   "possible{}\n", l());
            if ((op->max_depth == 0) && (ex_sz == 0)) {
                pr_err(-1, "exit, due to no --max-depth= and no "
                       "--exclude={}\n", l());
                return 1;
            } else if (! destin_excluded)
                pr_err(-1, "Probably best to --exclude= destination, will "
                       "continue{}\n", l());
        } else {
            if (cpf_verbose > 0)
                pr_err(-1, "Source does NOT contain destination (good){}\n",
                       l());
            if (path_contains_canon(op->destination_pt, op->source_pt)) {
                pr_err(-1, "Strange: destination contains source, is "
                       "infinite recursion possible ?{}\n", l());
                pr_err(2, "destination does NOT contain source (also "
                       "good){}\n", l());
            }
        }
        if (! op->mutp->deref_v.empty()) {
            static const char * rm_marker { "zzzzzzzz" };

            for (auto & sl : op->mutp->deref_v) {
                fs::path pt { sl };
                const bool is_absol = pt.is_absolute();

                if (! is_absol) {       // then make absolute
                    auto cur_pt { fs::current_path(ec) };
                    if (ec) {
                        pr_err(-1, "{}: unable to get current path, "
                               "ignored{}\n", sl, l(ec));
                        sl = rm_marker;    // unlikely name to remove later
                        continue;
                    }
                    pt = cur_pt / pt;
                }
                const auto lnk_name { pt.filename() };
                // want parent path in canonical form
                const auto parent_pt = pt.parent_path();
                const auto lpath { parent_pt / lnk_name };
                // following should remove .. components in path
                const auto npath { lpath.lexically_normal() };
                if (path_contains_canon(op->source_pt, npath) &&
                           (op->source_pt != sl)) {
                    const auto ftyp { fs::symlink_status(npath, ec).type() };

                    if (ec) {
                        pr_err(-1, "unable to 'stat' {}, ignored{}\n",
                               s(npath), l(ec));
                        sl = rm_marker;
                        continue;
                    } else if (ftyp != fs::file_type::symlink) {
                        pr_err(-1, "{}: is not a symlink, ignored\n",
                               s(npath));
                        sl = rm_marker;
                        continue;
                    } else {
                        sl = s(npath);
                        pr_err(5, "{}: is a candidate symlink, will deep "
                               "copy\n", s(npath));
                    }
                } else {
                    pr_err(-1, "{}: expected to be under SPATH{}\n", s(npath),
                           l());
                    sl = rm_marker;
                    continue;
                }
            }
            // compact vector by removing "zzzzzzzz" entries, trim with
            // 'erase()' then sort+unique so can do binary search later
            auto it = std::remove(op->mutp->deref_v.begin(),
                                  op->mutp->deref_v.end(), rm_marker);
            op->mutp->deref_v.erase(it, op->mutp->deref_v.end());
            if (op->mutp->deref_v.size() > 1) {
                std::ranges::sort(op->mutp->deref_v);
                // remove duplicates
                run_unique_and_erase(op->mutp->deref_v);
            }
        }
    }

    if (op->prune_given) {
        if (op->cache_op_num == 0) {
            ++op->cache_op_num;
            pr_err(0, ">> since --prune= given, set --cache implicitly{}\n",
                   l());
        }

    }

    if (op->cache_op_num > 0) {
        inmem_dir_t s_inm_rt(op->source_pt.filename(), short_stat());
        struct stat root_stat;

        s_inm_rt.is_root = 1;
        fs::path s_p_pt { op->source_pt.parent_path() };
        if (0 == s_p_pt.compare(op->source_pt.root_path())) {
            s_inm_rt.par_pt_s.clear();
        } else {
            s_inm_rt.par_pt_s = s_p_pt;
        }

        s_inm_rt.depth = -1;
        if (stat(op->source_pt.c_str(), &root_stat) < 0) {
            ec.assign(errno, std::system_category());
            pr_err(-1, "stat(source) failed{}\n", l(ec));
            return 1;
        }
        op->mutp->starting_fs_inst = root_stat.st_dev;

        // auto * rt_dirp { &s_inm_rt };
        s_inm_rt.shstat.st_dev = root_stat.st_dev;
        s_inm_rt.shstat.st_mode = root_stat.st_mode;
        if (op->prune_given)
            s_inm_rt.prune_mask = prune_up_chain;
        inmem_t src_rt_cache(s_inm_rt);
        op->mutp->cache_rt_dirp = std::get_if<inmem_dir_t>(&src_rt_cache);

        if (cpf_verbose > 4) {
            pr_err(4, ">>> initial, empty cache tree:");
            show_cache(src_rt_cache, true, op);
        }
        ec = do_cache(src_rt_cache, op);  // src ==> cache ==> destination
        if (ec)
            res = 1;
        if (cpf_verbose > 4) {
            pr_err(4, ">>> final cache tree:\n");
            show_cache(src_rt_cache, true, op);
        }
    } else {
        ec = do_clone(op);      // Single pass
        if (ec) {
            res = 1;
            pr_err(-1, "do_clone() failed{}\n", l());
        }
    }
    if ((op->want_stats == 0) && (op->destination_given == false) &&
        (op->source_given == false) && (op->no_destin == false)) {
        if (res == 0)
            scout << "Successfully cloned " << sysfs_root << " to "
                  << def_destin_root << "\n";
        else
            scout << "Problem cloning " << sysfs_root << " to "
                  << def_destin_root << "\n";
    }
    if ((! op->want_stats) && (q->num_scan_failed > 0)) {
        pr_err(-1, "Warning: scan of source truncated, may need to "
               "re-run{}\n", l());
        res = 1;
    }
    return res;
}
