#!/bin/sh

# Designed to remove 'cmake . ; cmake --build . ' artifacts from an in-tree
# build. For an out-of-tree build (e.g. 'cmake -S . -B build ; cd build ;
# cmake --build . ; cpack . ') simply do 'cd .. ; rm -rf build ' .

echo "Entering src/cmake_del_artifacts.sh"

rm -rf \
	clone_pseudo_fs \
	CMakeCache.txt \
	CMakeFiles \
	cmake_install.cmake \
	Makefile

