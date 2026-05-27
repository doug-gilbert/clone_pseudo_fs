
#cmakedefine OS_LINUX 1
#cmakedefine OS_FREEBSD 1
#cmakedefine OS_SOLARIS 1

#cmakedefine HAVE_STD_FORMAT 1

#cmakedefine HAVE_POSIX_MEMALIGN 1
#cmakedefine IGNORE_FAST_LEBE 1
#cmakedefine HAVE_BYTESWAP_H 1


#define BUILD_TIME "@BUILD_TIME@"

// #define SMP_LIB_LINUX @OS_LINUX@
// #define SMP_LIB_FREEBSD @OS_FREEBSD@
// #cmakedefine01 SMP_LIB_AIX @SMP_LIB_AIX@

// # This will generate a line in the output_file. Then in CMLists.txt:
// #      set(FEATURE_COMMENT "//")
// @FEATURE_COMMENT@#define OPTIONAL_SETTING 1

