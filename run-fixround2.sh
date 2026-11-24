#!/bin/bash
# Lane 796-mv fix-round verification for review finding r4686911130:
# 1) RED witness: hack the countdown interval to 2000 -> the new interval pin
#    must fail (proves the pre-fix witness accepted a shortened countdown).
# 2) GREEN: committed tree (interval 2500) -> the case passes.
set -x
W=~/dev/lanes/796-mv
B=$W/build/796
XV=36
cleanup() {
  [ -n "$XP" ] && kill "$XP" 2>/dev/null
  [ -n "$OP" ] && kill "$OP" 2>/dev/null
}
trap cleanup EXIT
setsid nohup Xvnc :$XV -geometry 1440x900 -depth 24 -securitytypes none -localhost > /tmp/xvnc-36.log 2>&1 < /dev/null &
XP=$!
sleep 2
setsid nohup env DISPLAY=:$XV openbox > /tmp/openbox-36.log 2>&1 < /dev/null &
OP=$!
sleep 2
CASE=mediaViewerVideoControlsAutoHideReturnsOnMoveAndPause
QML=$W/packages/native/qml/NativeMediaViewer.qml
cmake --build $B --target native-window-test --parallel 6 > /tmp/build796g.log 2>&1 || { echo "BUILD-GREEN-FAILED"; grep -E 'error:' /tmp/build796g.log | head; exit 1; }
echo "BUILD-GREEN-OK"
cd $B
DISPLAY=:$XV XAUTHORITY= QT_QPA_PLATFORM=xcb ./native-window-test $CASE > /tmp/win796-green.log 2>&1
echo "GREEN-RC=$? $(grep -oE 'Totals: .{0,40}' /tmp/win796-green.log | tail -1)"
sed -i 's/interval: 2500/interval: 2000/' "$QML"
cmake --build $B --target native-window-test --parallel 6 > /tmp/build796r.log 2>&1 || { echo "BUILD-RED-FAILED"; git -C $W checkout -- packages/native/qml/NativeMediaViewer.qml; exit 1; }
echo "BUILD-RED-OK"
DISPLAY=:$XV XAUTHORITY= QT_QPA_PLATFORM=xcb ./native-window-test $CASE > /tmp/win796-red.log 2>&1
echo "RED-RC=$? $(grep -oE 'Totals: .{0,40}' /tmp/win796-red.log | tail -1)"
grep -E 'FAIL!' /tmp/win796-red.log | head -3
git -C $W checkout -- packages/native/qml/NativeMediaViewer.qml
cmake --build $B --target native-window-test --parallel 6 > /tmp/build796g2.log 2>&1
echo "RESTORE-RC=$?"
echo "ALL-DONE"
