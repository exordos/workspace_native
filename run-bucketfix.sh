#!/bin/bash
# #818 buckets-sync fix chain: rebuild window-test on the rebased tree,
# measure the lane's new cases solo, regenerate bucket files.
set -x
cd "$HOME/dev/lanes/784" || exit 1
cmake -G Ninja -S packages/native -B build/784 -DCMAKE_BUILD_TYPE=Debug > cfg.log 2>&1 || { echo CFG_FAIL; tail -20 cfg.log; exit 1; }
cmake --build build/784 --target native-window-test --parallel 6 > build.log 2>&1 || { echo BUILD_FAIL; tail -30 build.log; exit 1; }
echo TARGET_BUILD_RC=0
DISPLAY=:31 XAUTHORITY= QT_QPA_PLATFORM=xcb ./build/784/native-window-test mediaViewerControlsStayVisibleWhileBarControlFocused mediaViewerTabReachesVideoControlsAfterAutoHide -o run.xml,xml > run-solo.log 2>&1
echo SOLO_RC=$?
grep -c 'status="fail' run.xml
python3 packages/native/scripts/gen-test-buckets.py --binary build/784/native-window-test --out packages/native/tests/window-test-buckets.cmake --fragments packages/native/tests/bucket-timings/window --timings run.xml > gen.log 2>&1
echo GEN_RC=$?
tail -2 gen.log
git status --short -- packages/native/tests/ | head -6
echo CHAIN_818_COMPLETE
