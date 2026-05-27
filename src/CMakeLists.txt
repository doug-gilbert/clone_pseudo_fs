
set ( sourcefiles clone_pseudo_fs.cpp )

add_executable ( clone_pseudo_fs ${sourcefiles} )

target_include_directories(clone_pseudo_fs PRIVATE ${CMAKE_BINARY_DIR})

if ( BUILD_SHARED_LIBS )
    MESSAGE( ">> Build using shared libraries (default)" )
else ( BUILD_SHARED_LIBS )
    MESSAGE( ">> Trying to build a static executable" )
    add_definitions ( -static )
    target_link_libraries(clone_pseudo_fs -static)
endif ( BUILD_SHARED_LIBS )

if ( ENABLE_DEBUG ) 
  MESSAGE( STATUS ">> Compile with DEBUG set" )
  target_compile_definitions(clone_pseudo_fs PRIVATE DEBUG)
  target_compile_options(clone_pseudo_fs PRIVATE
    -Wextra -Wmisleading-indentation -Wduplicated-cond -Wlogical-op
    -Wnull-dereference -Wshadow -Wunused -Wsizeof-array-argument
    -Wduplicated-branches -Wparentheses)
endif ( ENABLE_DEBUG )

install(TARGETS clone_pseudo_fs RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

