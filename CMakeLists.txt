cmake_minimum_required ( VERSION 3.10 )

project(clone_pseudo_fs VERSION 0.89.9)
# Use version number: x.y.9 to indicate pre-release of x.(y+1) 
# Example: VERSION 0.89.9 is pre-release of 0.90

option ( BUILD_SHARED_LIBS "Build using shared libraries" ON)

set (CMAKE_CXX_STANDARD 20 )
set (CMAKE_CXX_STANDARD_REQUIRED ON)
set (CMAKE_CXX_EXTENSIONS OFF)

set ( sourcefiles src/clone_pseudo_fs.cpp )
set ( headerfiles src/bwprint.hpp )

add_executable ( clone_pseudo_fs ${sourcefiles} ${headerfiles} )

if ( BUILD_SHARED_LIBS )
    MESSAGE( ">> Build using shared libraries (default)" )
else ( BUILD_SHARED_LIBS )
    MESSAGE( ">> Trying to build a static executable" )
    add_definitions ( -static )
    target_link_libraries(clone_pseudo_fs -static)
endif ( BUILD_SHARED_LIBS )

include(GNUInstallDirs)

install(TARGETS clone_pseudo_fs RUNTIME DESTINATION bin)
file(ARCHIVE_CREATE OUTPUT clone_pseudo_fs.8.gz PATHS doc/clone_pseudo_fs.8 FORMAT raw COMPRESSION GZip)
install(FILES clone_pseudo_fs.8.gz DESTINATION "${CMAKE_INSTALL_MANDIR}/man8")
install(FILES ChangeLog DESTINATION "${CMAKE_INSTALL_DOCDIR}")
install(FILES AUTHORS DESTINATION "${CMAKE_INSTALL_DOCDIR}")
install(FILES CREDITS DESTINATION "${CMAKE_INSTALL_DOCDIR}")
install(FILES README DESTINATION "${CMAKE_INSTALL_DOCDIR}")
install(FILES INSTALL DESTINATION "${CMAKE_INSTALL_DOCDIR}")

if ( ${CMAKE_SYSTEM_NAME} STREQUAL "FreeBSD" )
    MESSAGE( ">> Detected FreeBSD" )
    target_include_directories( clone_pseudo_fs PRIVATE /usr/local/include )
endif ( )


set(CPACK_PROJECT_NAME ${PROJECT_NAME})
set(CPACK_PROJECT_VERSION ${PROJECT_VERSION})
SET(CPACK_GENERATOR "DEB")
SET(CPACK_DEBIAN_PACKAGE_MAINTAINER "Douglas Gilbert") #required
SET(CPACK_DEBIAN_PACKAGE_SECTION "admin")
include(CPack)
