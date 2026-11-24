#!/bin/bash
# Perf stash-cycle (base 3fe5aea6 vs lane head) + scoped live verify for issue #784 / PR #818.
set -x
cd "$HOME/dev/lanes/784" || exit 1
LANE_BRANCH=cassi/video-controls-focus-784
BASE_SHA=3fe5aea6
BUILD=$PWD/packages/native/build/lane
BIN=$BUILD/workspace-native
EV=$HOME/dev/qa/evidence/784
EVL=$HOME/dev/qa/evidence/784-live
mkdir -p "$EV" "$EVL"

pgrep -f 'workspace-native$' >/dev/null && { echo PRE_APP_RUNNING; exit 1; }

# --- stage 1: base (merge-base) build + perf probe ---
git checkout --detach "$BASE_SHA" || { echo BASE_CHECKOUT_FAIL; exit 1; }
cmake --build "$BUILD" --target workspace-native --parallel 6 || { echo BASE_BUILD_FAIL; git checkout "$LANE_BRANCH"; exit 1; }
DISPLAY=:31 XAUTHORITY= QT_QPA_PLATFORM=xcb python3 "$HOME/dev/qa/perf_probe.py" --display :31 --bin "$BIN" --idle 20 --no-compare --out "$EV/perf-base.json" --label "base-$BASE_SHA"
echo BASE_PERF_RC=$?

# --- stage 2: lane head build + perf probe vs base ---
git checkout "$LANE_BRANCH" || { echo LANE_CHECKOUT_FAIL; exit 1; }
cmake --build "$BUILD" --target workspace-native --parallel 6 || { echo LANE_BUILD_FAIL; exit 1; }
DISPLAY=:31 XAUTHORITY= QT_QPA_PLATFORM=xcb python3 "$HOME/dev/qa/perf_probe.py" --display :31 --bin "$BIN" --idle 20 --ref "$EV/perf-base.json" --out "$EV/perf-lane.json"
echo LANE_PERF_RC=$?

# --- stage 3: scoped live verify (login + health + send; QA stream has no video attachment) ---
DISPLAY=:31 XAUTHORITY= QT_QPA_PLATFORM=xcb python3 "$HOME/dev/qa/live_qa.py" native-login --display :31 --bin "$BIN" --evidence "$EVL"
echo LOGIN_RC=$?
sleep 8
DISPLAY=:31 XAUTHORITY= python3 "$HOME/dev/qa/live_qa.py" shot --display :31 --out "$EVL/post_login.png"
python3 "$HOME/dev/qa/live_qa.py" health --repeat 10
echo HEALTH_RC=$?
python3 "$HOME/dev/qa/live_qa.py" send --stream-uuid 59033cd5-64ba-4a00-a932-5dddb5c17b65 --text "CASSI QA $(date -u +%Y%m%dT%H%M) lane784-video-focus"
echo SEND_RC=$?
pkill -f 'build/lane/workspace-native' 2>/dev/null
git checkout "$LANE_BRANCH" 2>/dev/null
echo PERF_LIVE_784_COMPLETE
