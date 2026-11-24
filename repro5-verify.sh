#!/bin/bash
# #770 verification: RED (fix stripped) -> GREEN (fix restored) -> seam loops.
# Lane display :31 (self-hosted; restart if killed). Never kill :31 from other runs.
set -u
D=$HOME/dev/lanes/770
CPP=$D/packages/native/tests/native-window-test.cpp
SRC=/tmp/full770.cpp
BIN=$D/build/lane/native-window-test
XDSP=:31
NEWFUNC=lateTeardownRequestDoesNotReenterDeadCaseLocals
x_alive(){ pgrep -x Xvnc >/dev/null; }
XVPID=""; OBPID=""
ensure_x() {
  if [ -e /tmp/.X11-unix/X31 ] && [ -n "$XVPID" ] && kill -0 $XVPID 2>/dev/null; then return; fi
  rm -f /tmp/.X31-lock 2>/dev/null
  Xvnc $XDSP -geometry 1440x900 -depth 24 -securitytypes none -localhost >/dev/null 2>&1 &
  XVPID=$!
  for i in $(seq 1 20); do [ -e /tmp/.X11-unix/X31 ] && break; sleep 0.5; done
  DISPLAY=$XDSP XAUTHORITY= openbox >/dev/null 2>&1 & OBPID=$!
  sleep 1; echo "[$(date +%H:%M:%S)] X :31 up Xvnc=$XVPID openbox=$OBPID"
}
runf() { # $1 func; rc = process rc; prints Totals line if any
  DISPLAY=$XDSP XAUTHORITY= QT_QPA_PLATFORM=xcb timeout 180 "$BIN" "$1" 2>&1
}
log() { echo "[$(date +%H:%M:%S)] $*"; }

cp "$CPP" "$SRC"
log "STAGE1-RED: strip quiesce lines"
n1=$(grep -c "handler = {}; // #770" "$CPP"); n2=$(grep -c "^        fixture.server.handler = {};$" "$CPP")
log "fix-line census: commented=$n1 bare=$n2 (expect 5 and 1)"
sed -i -e "/handler = {}; \/\/ #770/d" -e "/^        fixture.server.handler = {};$/d" "$CPP"
log "after strip: $(grep -c "handler = {}; // #770" "$CPP") + $(grep -c '^        fixture.server.handler = {};$' "$CPP") (expect 0 0)"
log "RED build"
cmake --build "$D/build/lane" --target native-window-test --parallel 6 > "$D/repro5-build-red.log" 2>&1
log "RED build rc=$?"
ensure_x
RED_GREEN=0; RED_RED=0
for i in $(seq 1 8); do
  ensure_x
  out=$(runf "$NEWFUNC"); rc=$?
  if [ $rc -eq 0 ] && echo "$out" | grep -q "Totals: .* passed"; then
    RED_GREEN=$((RED_GREEN+1)); log "red-iter $i: survived (rc=0)"
  else
    RED_RED=$((RED_RED+1)); log "red-iter $i: CRASHED rc=$rc :: $(echo "$out" | tail -2 | tr '\n' ' ')"
  fi
done
log "RED_SUMMARY green=$RED_GREEN red=$RED_RED"

log "STAGE2-GREEN: restore fix"
cp "$SRC" "$CPP"
log "GREEN build"
cmake --build "$D/build/lane" --target native-window-test --parallel 6 > "$D/repro5-build-green.log" 2>&1
log "GREEN build rc=$?"
G_GREEN=0
for i in $(seq 1 8); do
  ensure_x
  out=$(runf "$NEWFUNC"); rc=$?
  if [ $rc -eq 0 ] && echo "$out" | grep -q "Totals: .* passed"; then G_GREEN=$((G_GREEN+1)); else
    log "green-iter $i: FAILED rc=$rc :: $(echo "$out" | tail -3 | tr '\n' ' ')"; fi
done
log "GREEN_SUMMARY green=$G_GREEN/8"

log "STAGE3: seam loops on fixed build"
PAIR1="channelTopicSummaryDialogInvalidEndpointNumberReportsFieldError"
for spec in "P1" "P2"; do :; done
S3=0; S3F=0
for i in $(seq 1 16); do
  ensure_x
  out=$(runf "channelTopicSummaryDialogInvalidEndpointNumberReportsFieldError"); rc=$?
  [ $rc -eq 0 ] && echo "$out" | grep -q "Totals: .* passed" && S3=$((S3+1)) || { S3F=$((S3F+1)); log "seam-loop $i rc=$rc"; }
done
log "loop InvalidEndpoint: green=$S3 red=$S3F"
S4=0; S4F=0
for i in $(seq 1 16); do
  ensure_x
  out=$(runf "channelTopicSummaryDialogSendsOnlyUserEditedPromptAfterRemoteEffortUpdate"); rc=$?
  [ $rc -eq 0 ] && echo "$out" | grep -q "Totals: .* passed" && S4=$((S4+1)) || { S4F=$((S4F+1)); log "loop2 $i rc=$rc"; }
done
log "loop SendsOnlyPrompt: green=$S4 red=$S4F"
S5=0; S5F=0
for i in $(seq 1 12); do
  ensure_x
  out=$(runf "profileAndTopbarUseMeasuredThreeLineGeometry"); rc=$?
  [ $rc -eq 0 ] && echo "$out" | grep -q "Totals: .* passed" && S5=$((S5+1)) || { S5F=$((S5F+1)); log "loop3 $i rc=$rc"; }
done
log "loop profile-exit: green=$S5 red=$S5F"
log "ALL-STAGES-DONE"
