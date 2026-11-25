#!/bin/bash
# Chain v3 (#818 bucket regen + #820 build & live smoke).
# v1/v2 died: gateway SIGTERM-kills non-persisted jobs at session end; :31 openbox died with them.
set -x
OB_PID_FILE=/tmp/openbox31-v3.pid

# --- ensure a WM on :31 (own pid recorded, never name-kill others) ---
OB_OK=0
for p in $(pgrep -x openbox); do
  if tr '\0' '\n' < /proc/$p/environ 2>/dev/null | grep -qx DISPLAY=:31; then OB_OK=1; fi
done
if [ "$OB_OK" = "0" ]; then
  DISPLAY=:31 XAUTHORITY= openbox &
  echo $! > "$OB_PID_FILE"
  sleep 2
  echo OB31_RESTARTED=$(cat "$OB_PID_FILE")
else
  echo OB31_ALREADY_RUNNING
fi

# ================= stage A: #818 window-test buckets =================
cd "$HOME/dev/lanes/784" || { echo A_CD_FAIL; exit 1; }
cmake --build build/784 --target native-window-test --parallel 6 > build3.log 2>&1
B_RC=$?; echo B_BUILD_RC=$B_RC
[ "$B_RC" != "0" ] && { tail -20 build3.log; exit 1; }
timeout 900 env DISPLAY=:31 XAUTHORITY= QT_QPA_PLATFORM=xcb \
  ./build/784/native-window-test \
  mediaViewerControlsStayVisibleWhileBarControlFocused \
  mediaViewerTabReachesVideoControlsAfterAutoHide \
  -o run-bucketfix.xml,xml > run-solo3.log 2>&1
echo B_SOLO_RC=$?
echo B_FAILS=$(grep -c 'status="fail' run-bucketfix.xml)
grep -o 'time="[0-9.]*"' run-bucketfix.xml
python3 packages/native/scripts/gen-test-buckets.py \
  --binary build/784/native-window-test \
  --out packages/native/tests/window-test-buckets.cmake \
  --fragments packages/native/tests/bucket-timings/window \
  --timings run-bucketfix.xml > gen3.log 2>&1
echo B_GEN_RC=$?
tail -3 gen3.log
git status --porcelain -- packages/native/tests/ | head -8
echo CHAIN818_BUCKETS_DONE

# ================= stage B: #820 app build + scoped live smoke =================
cd "$HOME/dev/lanes/786" || { echo B_CD_FAIL; exit 1; }
B786="$PWD/build/786"
cmake --build "$B786" --target workspace-native --parallel 6 > build-app3.log 2>&1
A_RC=$?; echo A_BUILD_RC=$A_RC
[ "$A_RC" != "0" ] && { tail -30 build-app3.log; exit 1; }
BIN="$B786/workspace-native"
ls -la "$BIN" || { echo BIN_MISSING; exit 1; }
EVL="$HOME/dev/qa/evidence/786-live"
mkdir -p "$EVL"
DISPLAY=:31 XAUTHORITY= QT_QPA_PLATFORM=xcb \
  python3 "$HOME/dev/qa/live_qa.py" native-login --display :31 --bin "$BIN" --evidence "$EVL" > live3.log 2>&1
echo LIVE_RC=$?
tail -6 live3.log
sleep 8
DISPLAY=:31 XAUTHORITY= python3 "$HOME/dev/qa/live_qa.py" health --repeat 3 >> live3.log 2>&1
echo HEALTH_RC=$?
pgrep -f 'dev/lanes/786/build/786/workspace-native' | tail -1 > /tmp/app786.pid || true
echo APP786_PID=$(cat /tmp/app786.pid)
ls -la "$EVL"
echo CHAIN_78_82_V3_COMPLETE
