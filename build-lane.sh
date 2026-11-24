#!/usr/bin/env bash
set -x
cd /home/ubuntu/dev/lanes/791/packages/native || exit 1
cmake -S . -B build/lane > build-configure.log 2>&1
rc=$?
echo "CONFIGURE_RC=$rc"
[ $rc -ne 0 ] && tail -30 build-configure.log && exit $rc
cmake --build build/lane --target native-window-test workspace-native --parallel 5
echo "BUILD_RC=$?"
