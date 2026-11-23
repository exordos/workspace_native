#!/bin/bash
set -x
cd ~/dev/lanes/735
cmake -S packages/native -B build/lane -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
rc=$?
[ $rc -ne 0 ] && { echo "CONFIGURE FAILED rc=$rc"; exit $rc; }
cmake --build build/lane --target native-controller-ext-provider-test --parallel 6
rc=$?
[ $rc -ne 0 ] && { echo "BUILD FAILED rc=$rc"; exit $rc; }
echo BUILD_DONE
