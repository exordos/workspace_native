#!/bin/bash
# #770 repro v4: PAIR2 seam under gdb 'catch syscall exit_group' - captures bt for ANY
# exit(1)/abort path (A2_6 died exit-01 mid-case with no signal stop: invisible to v3 greps).
# Self-hosts X :35; if X dies mid-iter restarts it and continues. NEVER kill :35 from other runs.
BIN=$HOME/dev/gate/current/build/gate/native-window-test
D=$HOME/dev/lanes/770
XVPORT=35
XDSP=:35
echo $$ > "$D/repro4.pid"
XVPID=""; OBPID=""
log() { echo "[$(date +%H:%M:%S)] $*"; }
x_alive() { [ -n "$XVPID" ] && kill -0 "$XVPID" 2>/dev/null; }
ensure_x() {
  if x_alive; then return 0; fi
  log "X $XDSP not alive - (re)starting own Xvnc+openbox"
  if [ ! -e /tmp/.X11-unix/X$XVPORT ]; then rm -f /tmp/.X${XVPORT}-lock 2>/dev/null; fi
  Xvnc $XDSP -geometry 1440x900 -depth 24 -securitytypes none -localhost >/dev/null 2>&1 &
  XVPID=$!
  for i in $(seq 1 20); do [ -e /tmp/.X11-unix/X$XVPORT ] && break; sleep 0.5; done
  DISPLAY=$XDSP XAUTHORITY= openbox >/dev/null 2>&1 &
  OBPID=$!
  sleep 1
  log "X $XDSP up: Xvnc=$XVPID openbox=$OBPID"
}
xdeath() { grep -qE "X11 connection broke|XIO: +fatal IO error" "$1" 2>/dev/null; }

# gdb: stop on SIGABRT/SIGSEGV (default) AND catch the exit_group syscall -> bt covers
# silent exit(1) / QtTest-handler _exit paths. A clean pass also hits the catch AFTER Totals.
GDBCMD=(gdb -batch -ex "set pagination off" -ex "set print thread-events off"
        -ex "catch syscall exit_group" -ex run -ex "bt 40" -ex "info threads")

run_iter() { # $1 tag $2 out ; rest = case list
  local tag="$1" out="$2"; shift 2
  ensure_x
  DISPLAY=$XDSP XAUTHORITY= QT_QPA_PLATFORM=xcb timeout 300 "${GDBCMD[@]}" --args "$BIN" "$@" > "$out" 2>&1
  if xdeath "$out"; then log "$tag: X died mid-iter - restarting X, not counted"; kill $XVPID $OBPID 2>/dev/null; XVPID=""; OBPID=""; ensure_x; return 200; fi
  if grep -qE "Totals: .* passed" "$out"; then return 0; fi
  # no Totals = died mid-run: bt is already in $out (signal stop OR exit_group catch)
  log "DIED mid-case in $tag - bt captured in $out"
  return 1
}
cleanup() { kill $XVPID $OBPID 2>/dev/null; }
trap cleanup EXIT

PAIR1=(channelTopicSummaryDialogInvalidEndpointNumberReportsFieldError channelTopicSummaryDialogSendsOnlyUserEditedEffortAfterRemotePromptUpdate)
PAIR2=(channelTopicSummaryDialogInvalidEndpointNumberReportsFieldError channelTopicSummaryDialogSendsOnlyUserEditedPromptAfterRemoteEffortUpdate)
PAIR2B=(channelTopicSummaryDialogSendsOnlyUserEditedPromptAfterRemoteEffortUpdate channelTopicSummaryDialogInvalidEndpointNumberReportsFieldError)

for spec in "P2:PAIR2:48" "P1:PAIR1:24" "P2B:PAIR2B:24"; do
  name=${spec%%:*}; rest=${spec#*:}; arr=${rest%%:*}; n=${rest##*:}
  eval "cases=(\"\${${arr}[@]}\")"
  log "=== PHASE $name (${arr}) x$n under catch-exit_group gdb ==="
  for i in $(seq 1 $n); do
    run_iter "${name}_$i" "$D/repro4-${name}_$i.log" "${cases[@]}"
    rc=$?
    if [ $rc -eq 1 ]; then cp "$D/repro4-${name}_$i.log" "$D/repro4-BURP.log"; log "BURP_DONE phase $name iter $i"; exit 0; fi
    if [ $((i % 8)) -eq 0 ]; then log "$name progress $i/$n (alive)"; fi
  done
done
log "VERDICT: all phases green - not locally reproducible; named CI-load flake"
echo "VERDICT_ALL_GREEN"
