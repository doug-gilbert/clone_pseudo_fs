cmake_minimum_required ( VERSION 3.10 )

project(clone_pseudo_fs VERSION 0.89.9 LANGUAGES CXX)
# Use version number: x.y.9 to indicate pre-release of x.(y+1) 
# Example: VERSION 0.89.9 is pre-release of 0.90

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

string(TIMESTAMP BUILD_TIME "%Y%m%d %H:%M:%S")

option ( BUILD_SHARED_LIBS "Build using shared libraries" ON)
option ( ENABLE_DEBUG "Turn on debugging (extra warnings)" OFF)

set (CMAKE_CXX_STANDARD 20 )
set (CMAKE_CXX_STANDARD_REQUIRED ON)
set (CMAKE_CXX_EXTENSIONS OFF)

set(OS_LINUX 0)
set(SG_LIB_LINUX 0)
set(OS_ANDROID 0)
set(OS_FREEBSD 0)
set(SG_LIB_FREEBSD 0)
set(OS_SOLARIS 0)
set(SG_LIB_SOLARIS 0)
set(OS_AIX 0)
set(OS_WINDOWS 0)
set(OS_CYGWIN 0)
set(OS_MINGW 0)

if(CYGWIN)
    message(STATUS "Cygwin")
    set(OS_CYGWIN 1)
elseif(MINGW)
    message(STATUS "MinGW")
    set(OS_MINGW 1)
elseif(WIN32)
    message(STATUS "Windows")
    set(OS_WINDOWS 1)
elseif(ANDROID)
    message(STATUS "Android")
    set(OS_ANDROID 1)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(STATUS "Linux")
    set(OS_LINUX 1)
    # set(SG_LIB_LINUX 1)
    # CHECK_INCLUDE_FILE( "linux/types.h" HAVE_LINUX_TYPES_H )
    # CHECK_INCLUDE_FILE( "linux/bsg.h" HAVE_LINUX_BSG_H )
    # CHECK_INCLUDE_FILE( "linux/kdev_t.h" HAVE_LINUX_KDEV_T_H )
elseif(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
    message(STATUS "FreeBSD")
    set(OS_FREEBSD 1)
    # set(SG_LIB_FREEBSD 1)
    target_include_directories( smp_utils PRIVATE /usr/local/include )
elseif(CMAKE_SYSTEM_NAME STREQUAL "SunOS")
    message(STATUS "Solaris")
    set(OS_SOLARIS 1)
    # set(SG_LIB_SOLARIS 1)
elseif(CMAKE_SYSTEM_NAME STREQUAL "AIX")
    message(STATUS "AIX")
    set(OS_AIX 1)
elseif(APPLE)
    message(STATUS "macOS")
    set(OS_MACOS 1)
else()
    message(STATUS "Unknown system: ${CMAKE_SYSTEM_NAME}")
endif()

include(CheckCXXSourceCompiles)

check_cxx_source_compiles("
#include <format>

#ifndef __cpp_lib_format
#error __cpp_lib_format not defined
#endif

#if __cpp_lib_format < 201907L
#error format too old
#endif

int main()
{
    auto s = std::format(\"{}\", 123);
    return 0;
}
" HAVE_STD_FORMAT)

if(HAVE_STD_FORMAT)
    message(STATUS \"std::format supported\")
else()
    message(STATUS \"std::format NOT supported\")
endif()

configure_file( ${CMAKE_SOURCE_DIR}/config.h.in.cmake ${CMAKE_BINARY_DIR}/config.h )
add_definitions ( -DHAVE_CONFIG_H )

add_subdirectory(src)

add_subdirectory(doc)

add_subdirectory(examples)

install(FILES ChangeLog DESTINATION "${CMAKE_INSTALL_DOCDIR}")
install(FILES AUTHORS DESTINATION "${CMAKE_INSTALL_DOCDIR}")
install(FILES CREDITS DESTINATION "${CMAKE_INSTALL_DOCDIR}")
install(FILES README DESTINATION "${CMAKE_INSTALL_DOCDIR}")
install(FILES INSTALL DESTINATION "${CMAKE_INSTALL_DOCDIR}")


set(CPACK_PROJECT_NAME ${PROJECT_NAME})
set(CPACK_PROJECT_VERSION ${PROJECT_VERSION})

SET(CPACK_DEBIAN_PACKAGE_MAINTAINER "Douglas Gilbert") #required
SET(CPACK_DEBIAN_PACKAGE_SECTION "admin")
SET(CPACK_DEBIAN_PACKAGE_DEPENDS "")

SET(CPACK_RPM_PACKAGE_DESCRIPTION "Allows pseudo file systems (e.g. sysfs) to be copied/cloned")
SET(CPACK_RPM_PACKAGE_LICENSE "BSD-2-clause")
SET(CPACK_RPM_PACKAGE_VENDOR "Douglas Gilbert")
SET(CPACK_RPM_PACKAGE_DEPENDS "")

SET(CPACK_GENERATOR "DEB;RPM")

include(CPack)
include(CTest)
