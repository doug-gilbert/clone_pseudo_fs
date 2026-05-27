
file(ARCHIVE_CREATE OUTPUT ${CMAKE_BINARY_DIR}/doc/clone_pseudo_fs.8.gz PATHS doc/clone_pseudo_fs.8 FORMAT raw COMPRESSION GZip)
install(FILES ${CMAKE_BINARY_DIR}/doc/clone_pseudo_fs.8.gz DESTINATION "${CMAKE_INSTALL_MANDIR}/man8")
