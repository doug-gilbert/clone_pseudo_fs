#!/bin/sh

# Designed to remove 'cmake . ; cmake --build . ' artifacts from an in-tree
# build. For an out-of-tree build (e.g. 'cmake -S . -B build ; cd build ;
# cmake --build . ; cpack . ') simply do 'cd .. ; rm -rf build ' .

cd src || exit
./cmake_del_artifacts.sh
cd ..

cd doc || exit
./cmake_del_artifacts.sh
cd ..

cd examples || exit
./cmake_del_artifacts.sh
cd ..

rm -rf \
	build \
	CMakeCache.txt \
	CMakeFiles \
	CPackConfig.cmake \
	CPackSourceConfig.cmake \
	_CPack_Packages \
	CMakeFiles \
	cmake_install.cmake \
	config.h \
	CTestTestfile.cmake \
	DartConfiguration.tcl \
	install_manifest.txt \
	Makefile \
	Testing

