cmake_minimum_required (VERSION 3.0)

project(clone_pseudo_fs VERSION 0.89.2)

set (CMAKE_CXX_STANDARD 20 )
set (CMAKE_CXX_STANDARD_REQUIRED ON)
set (CMAKE_CXX_EXTENSIONS OFF)

add_executable (clone_pseudo_fs src/clone_pseudo_fs.cpp)

MESSAGE( "CMAKE_INSTALL_PREFIX : ${CMAKE_INSTALL_PREFIX}" )

install(TARGETS clone_pseudo_fs RUNTIME DESTINATION bin)

include(GNUInstallDirs)
file(ARCHIVE_CREATE OUTPUT clone_pseudo_fs.8.gz PATHS doc/clone_pseudo_fs.8 FORMAT raw COMPRESSION GZip)
install(FILES clone_pseudo_fs.8.gz DESTINATION "${CMAKE_INSTALL_MANDIR}/man8")


set(CPACK_PROJECT_NAME ${PROJECT_NAME})
set(CPACK_PROJECT_VERSION ${PROJECT_VERSION})
SET(CPACK_GENERATOR "DEB")
SET(CPACK_DEBIAN_PACKAGE_MAINTAINER "dpg") #required
include(CPack)
