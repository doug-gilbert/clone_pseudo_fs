
#include <filesystem>
#include <string>
#include <cstdio>
#include <cstring>

namespace fs = std::filesystem;

const fs::path def_pt { fs::path() };
const fs::path mt_pt { fs::path("") };
const fs::path rt_pt { fs::path("/") };
const fs::path red1_rt_pt { fs::path("//") };
const fs::path red2_rt_pt { fs::path("/..") };
const fs::path red3_rt_pt { fs::path("/../") };
const fs::path red4_rt_pt { fs::path("/..//") };
const fs::path sys_pt { fs::path("/sys") };
const fs::path sys_trail_pt { fs::path("/sys/") };
const fs::path red1_sys_pt { fs::path("//sys") };
const fs::path typical1_pt { fs::path("/sys/class/typec") };
const fs::path typical1_trail_pt { fs::path("/sys/class/typec/") };

static const char *
cstr(const fs::path & pt)
{
    int sz = pt.string().size();
    static char b[256];

    if (sz > 255) {
	b[255] = '\0';
	sz = 254;
    }
    memcpy(b, pt.string().c_str(), sz + 1);
    return b;
}

static void
print_fs_attrs(const fs::path & pt)
{
    if (pt.is_relative())
        printf("   is_relative\n");
    else
        printf("   is_absolute\n");
    const fs::path ln_pt = pt.lexically_normal();
    printf("   lexically_normal(): %s\n", cstr(ln_pt));
    printf("   filename(): %s\n", cstr(pt.filename()));
    fs::path nc_pt { pt };
    printf("   make_preferred(): %s\n", cstr(nc_pt.make_preferred()));
    printf("   root_path(): %s\n", cstr(pt.root_path()));
    printf("       parent_path(): %s\n", cstr(pt.parent_path()));

    printf("   split lexically_normal with iterator:\n");
    for (const auto & comp : ln_pt)
	printf("        %s\n", cstr(comp));

    printf("\n");
}

int
main()
{
    fs::path pt = def_pt;

    printf("filesystem default path : %s\n", cstr(pt));
    print_fs_attrs(pt);

    pt = mt_pt;
    printf("filesystem empty path : %s\n", cstr(pt));
    print_fs_attrs(pt);

    pt = rt_pt;
    printf("filesystem root path : %s\n", cstr(pt));
    print_fs_attrs(pt);

    pt = red1_rt_pt;
    printf("filesystem first redundant root path : %s\n", cstr(pt));
    print_fs_attrs(pt);

    pt = red2_rt_pt;
    printf("filesystem second redundant root path : %s\n", cstr(pt));
    print_fs_attrs(pt);

    pt = red3_rt_pt;
    printf("filesystem third redundant root path : %s\n", cstr(pt));
    print_fs_attrs(pt);

    pt = red4_rt_pt;
    printf("filesystem fourth redundant root path : %s\n", cstr(pt));
    print_fs_attrs(pt);

    pt = sys_pt;
    printf("filesystem /sys path : %s\n", cstr(pt));
    print_fs_attrs(pt);

    pt = sys_trail_pt;
    printf("filesystem /sys/ path : %s\n", cstr(pt));
    print_fs_attrs(pt);

    pt = red1_sys_pt;
    printf("filesystem first redendant /sys path : %s\n", cstr(pt));
    print_fs_attrs(pt);

    pt = typical1_pt;
    printf("filesystem first typical path : %s\n", cstr(pt));
    print_fs_attrs(pt);

    pt = typical1_trail_pt;
    printf("filesystem first typical path, trailing slash : %s\n", cstr(pt));
    print_fs_attrs(pt);

}
