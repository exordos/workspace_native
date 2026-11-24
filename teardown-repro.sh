#!/bin/bash
# #770 surgical teardown-seam repro v3 (self-healing X: restarts its own Xvnc if killed)
# Seams from CI evidence:
#   A: after channelTopicSummaryDialogInvalidEndpointNumberReportsFieldError PASS,
#      before channelTopicSummaryDialogSendsOnlyUserEditedEffortAfterRemotePromptUpdate banner (bucket15)
#   B: after profileAndTopbarUseMeasuredThreeLineGeometry PASS, exit teardown (bucket8 last case)
BIN=$HOME/dev/gate/current/build/gate/native-window-test
D=$HOME/dev/lanes/770
XVPORT=34
XDSP=:34
echo $$ > "$D/repro3.pid"
XVPID=""; OBPID=""

log() { echo "[$(date +%H:%M:%S)] $*"; }

x_alive() { [ -n "$XVPID" ] && kill -0 "$XVPID" 2>/dev/null; }

ensure_x() {
  if x_alive; then return 0; fi
  log "X $XDSP not alive - (re)starting own Xvnc+openbox"
  # clear stale socket only if our pid is gone
  if [ ! -e /tmp/.X11-unix/X$XVPORT ] || ! x_alive; then
    rm -f /tmp/.X${XVPORT}-lock 2>/dev/null
  fi
  Xvnc $XDSP -geometry 1440x900 -depth 24 -securitytypes none -localhost >/dev/null 2>&1 &
  XVPID=$!
  for i in $(seq 1 20); do [ -e /tmp/.X11-unix/X$XVPORT ] && break; sleep 0.5; done
  DISPLAY=$XDSP XAUTHORITY= openbox >/dev/null 2>&1 &
  OBPID=$!
  sleep 1
  log "X $XDSP up: Xvnc=$XVPID openbox=$OBPID"
}

xdeath() { grep -qE "X11 connection broke|XIO: +fatal IO error" "$1" 2>/dev/null; }

BURP=0
run_iter() { # $1 tag $2 rc-expected-log path... ; args after 2 = case list
  local tag="$1"; local out="$2"; shift 2
  ensure_x
  DISPLAY=$XDSP XAUTHORITY= QT_QPA_PLATFORM=xcb timeout 300 gdb -batch -ex run -ex bt -ex "thread apply all bt" --args "$BIN" "$@" > "$out" 2>&1
  local rc=$?
  if xdeath "$out"; then log "$tag: X died mid-iter (external kill) - restarting X, not a burp"; kill $XVPID $OBPID 2>/dev/null; XVPID=""; OBPID=""; ensure_x; return 200; fi
  if [ $rc -eq 134 ] || grep -qE "SIGABRT|bad_function_call" "$out"; then log "BURP in $tag (rc=$rc) - bt in $out"; return 1; fi
  if [ $rc -eq 139 ] || grep -qE "SIGSEGV" "$out"; then log "SEGV in $tag (rc=$rc) - bt in $out"; return 1; fi
  if [ $rc -eq 124 ]; then log "$tag: timeout 300s (hang) - see $out"; return 1; fi
  return 0
}

cleanup() { kill $XVPID $OBPID 2>/dev/null; }
trap cleanup EXIT

PAIR1=(channelTopicSummaryDialogInvalidEndpointNumberReportsFieldError channelTopicSummaryDialogSendsOnlyUserEditedEffortAfterRemotePromptUpdate)
PAIR2=(channelTopicSummaryDialogInvalidEndpointNumberReportsFieldError channelTopicSummaryDialogSendsOnlyUserEditedPromptAfterRemoteEffortUpdate)

log "=== PHASE A: seam pair x24 under gdb ==="
for i in $(seq 1 24); do
  run_iter "A$i" "$D/repro3-A$i.log" "${PAIR1[@]}"
  rc=$?
  if [ $rc -eq 1 ]; then BURP=1; cp "$D/repro3-A$i.log" "$D/repro3-BURP.log"; log "BURP_DONE phase A iter $i"; exit 0; fi
  if [ $rc -ne 0 ]; then log "A$i retrying after anomaly"; run_iter "A${i}r" "$D/repro3-A${i}r.log" "${PAIR1[@]}"; [ $? -eq 1 ] && { cp "$D/repro3-A${i}r.log" "$D/repro3-BURP.log"; log "BURP_DONE"; exit 0; }; fi
  [ $((i % 6)) -eq 0 ] && log "A progress $i/24"
done

log "=== PHASE A2: second dialog pair x12 ==="
for i in $(seq 1 12); do
  run_iter "A2_$i" "$D/repro3-A2_$i.log" "${PAIR2[@]}"
  [ $? -eq 1 ] && { cp "$D/repro3-A2_$i.log" "$D/repro3-BURP.log"; log "BURP_DONE phase A2 iter $i"; exit 0; }
done

log "=== PHASE B: part8 last-case exit-teardown x12 ==="
for i in $(seq 1 12); do
  run_iter "B$i" "$D/repro3-B$i.log" profileAndTopbarUseMeasuredThreeLineGeometry
  [ $? -eq 1 ] && { cp "$D/repro3-B$i.log" "$D/repro3-BURP.log"; log "BURP_DONE phase B iter $i"; exit 0; }
done

log "=== PHASE C: load - 5x parallel pair loops x8 (no gdb), then gdb re-hit on burp ==="
for i in $(seq 1 8); do
  for w in 1 2 3 4 5; do
    ( DISPLAY=$XDSP XAUTHORITY= QT_QPA_PLATFORM=xcb timeout 300 "$BIN" "${PAIR1[@]}" > "$D/repro3-C-${i}-${w}.log" 2>&1; echo "w$w rc=$?" >> "$D/repro3-C-rc.log" ) &
  done
  wait
  grep -q "rc=134" "$D/repro3-C-rc.log" 2>/dev/null && { log "load burp at round $i - gdb re-hit"; run_iter "Cgdb" "$D/repro3-Cgdb.log" "${PAIR1[@]}"; [ $? -eq 1 ] && { cp "$D/repro3-Cgdb.log" "$D/repro3-BURP.log"; log "BURP_DONE phase C"; exit 0; }; }
  log "C round $i/8 done"
done

log "VERDICT: all phases green (A x24 + A2 x12 + B x12 + C load) - not locally reproducible; named CI-load flake"
echo "VERDICT_ALL_GREEN"
