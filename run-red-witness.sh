#!/bin/bash
# RED witness for #831 finding 4687773818: injected rogue DELETE must redden the case.
# Reuses lane display :31 (started by the orchestrator this tick).
set -x
W=/home/ubuntu/dev/lanes/796-sum
B=$W/packages/native/build/796sum
XV=31
CTL=$W/packages/native/src/native-controller.cpp
cp $CTL /tmp/ctl-796sum.bak
python3 - <<'PY'
p="/home/ubuntu/dev/lanes/796-sum/packages/native/src/native-controller.cpp"
lines=open(p).read().split("\n")
for i,l in enumerate(lines):
    if "topic_summary_endpoints/" in l and 'request("DELETE"' in l:
        fake='    request("DELETE", messenger + "/topic_summary_endpoints/" + uuid + "-rogue", {}, {}, [](const ApiResult &){});'
        lines.insert(i, fake)
        print("INJECTED before line", i+1)
        break
else:
    print("INJECT-MISS"); raise SystemExit(1)
open(p,"w").write("\n".join(lines))
PY
if [ $? -ne 0 ]; then echo INJECT-FAIL; exit 1; fi
cmake --build $B --target native-window-test --parallel 6 || { echo BUILD-HACK-FAIL; cp /tmp/ctl-796sum.bak $CTL; exit 1; }
echo BUILD-HACK-OK
CASE=channelTopicSummaryDialogEndpointDeleteConfirmDispatchesAndCancelRestores
cd $B
DISPLAY=:$XV XAUTHORITY= QT_QPA_PLATFORM=xcb ./native-window-test $CASE > /tmp/g796sum-red.log 2>&1
echo "RED-RC=$?"
grep -E 'FAIL!|Totals:' /tmp/g796sum-red.log | head -5
cp /tmp/ctl-796sum.bak $CTL
cmake --build $B --target native-window-test --parallel 6; echo "RESTORE-BUILD-RC=$?"
git -C $W diff --stat -- packages/native/src/native-controller.cpp
echo ALL-DONE
