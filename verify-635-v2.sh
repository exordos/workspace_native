#!/bin/bash
# Lane 635 verify v2: audit-field fix @76aed6d -> full channels group green.
set -x
cd ~/dev/gate/current || exit 1
git fetch origin -q
git checkout -q -B lane635-check origin/cassi/topic-summary-settings
cmake --build build/gate --target native-controller-channels-test --parallel 6
echo "BUILD_RC=$? (expect 0)"
QT_QPA_PLATFORM=offscreen ./build/gate/native-controller-channels-test
echo "GROUP_RC=$? (expect 0)"
git checkout -q --detach 1367394
echo DONE_635_V2
