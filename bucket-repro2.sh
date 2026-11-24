#!/bin/bash
# #770 co-runner v2: self-hosted Xvnc :34 (OUTSIDE lane display set :30-:33 —
# run-end cleanup killed :31 mid-iter on 2026-11-24 and invalidated v1).
# Replay exact CI bucket shape under gdb; crash yields bt.
cd ~/dev/lanes/770
export DISPLAY=:34 QT_QPA_PLATFORM=xcb XAUTHORITY=
MY_XVNC=""; MY_WM=""
cleanup() { [ -n "$MY_XVNC" ] && kill "$MY_XVNC" 2>/dev/null; [ -n "$MY_WM" ] && kill "$MY_WM" 2>/dev/null; }
trap cleanup EXIT
Xvnc :34 -geometry 1440x900 -depth 24 -securitytypes none -localhost >/dev/null 2>&1 &
MY_XVNC=$!
sleep 2
openbox & MY_WM=$!
BIN=build/lane/native-window-test
OUT=~/dev/lanes/770/bucket2-repro
B8=()
B15=()
for f in $(sed -n '/set(WORKSPACE_NATIVE_WINDOW_BUCKET_15/,/^)/p' packages/native/tests/window-test-buckets.cmake | grep -vE 'set\(|^\)'); do B15+=("$f"); done
for f in $(sed -n '/set(WORKSPACE_NATIVE_WINDOW_BUCKET_8/,/^)/p' packages/native/tests/window-test-buckets.cmake | grep -vE 'set\(|^\)'); do B8+=("$f"); done
echo "counts: part8=${#B8[@]} part15=${#B15[@]}"
for i in 1 2 3 4; do
  echo "=== part15 iter $i $(date +%T) ==="
  gdb -batch -ex run -ex 'thread apply all bt 14' --args "$BIN" "${B15[@]}" > "$OUT-p15-$i.log" 2>&1
  if grep -qE 'SIGABRT|bad_function_call' "$OUT-p15-$i.log"; then echo "VERDICT: CRASH part15 iter $i (bt in $OUT-p15-$i.log)"; break; else echo "iter $i green: $(grep -c '^PASS' $OUT-p15-$i.log) passes"; fi
done
for i in 1 2 3; do
  echo "=== part8 iter $i $(date +%T) ==="
  gdb -batch -ex run -ex 'thread apply all bt 14' --args "$BIN" "${B8[@]}" > "$OUT-p8-$i.log" 2>&1
  if grep -qE 'SIGABRT|bad_function_call' "$OUT-p8-$i.log"; then echo "VERDICT: CRASH part8 iter $i (bt in $OUT-p8-$i.log)"; break; else echo "iter $i green: $(grep -c '^PASS' $OUT-p8-$i.log) passes"; fi
done
echo BURP_DONE
