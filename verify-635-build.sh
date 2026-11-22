#!/bin/bash
# Lane 635 v1: compile the touched controller target in the warm gate tree and
# run the channels group (headless, no display needed) for the new cases.
set -x
cd ~/dev/gate/current || exit 1
git fetch origin -q || { echo "FETCH_FAIL"; exit 1; }
git checkout -q -B lane635-check origin/cassi/topic-summary-settings || { echo "CHECKOUT_FAIL"; exit 1; }
cmake --build build/gate --target native-controller-channels-test --parallel 6 || { echo "BUILD_FAIL_635"; exit 1; }
QT_QPA_PLATFORM=offscreen ./build/gate/native-controller-channels-test -o /home/ubuntu/.hermes/cache/scratch/635_channels.xml,xml
echo "TESTS_RC=$? (expect 0)"
echo "DONE_635_BUILD"
git checkout -q --detach 1367394 2>/dev/null || true
