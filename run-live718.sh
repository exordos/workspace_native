#!/bin/bash
# Live smoke for PR #732 head d4082c1 (issue #718 slice 2). Runs detached.
set -x
cd "$HOME/dev/lanes/718" || exit 1
Xvnc :32 -geometry 1440x900 -depth 24 -securitytypes none -localhost >/dev/null 2>&1 &
XV=$!; sleep 2
DISPLAY=:32 openbox >/dev/null 2>&1 &
OB=$!
echo "PIDS XV=$XV OB=$OB" | tee /tmp/live718.pids
BIN=$HOME/dev/lanes/718/build/lane/workspace-native
test -x "$BIN" && echo BIN_OK || { echo BIN_MISSING; exit 1; }
EV=$HOME/dev/qa/evidence/718-live
DISPLAY=:32 XAUTHORITY= QT_QPA_PLATFORM=xcb python3 $HOME/dev/qa/live_qa.py native-login --display :32 --bin "$BIN" --evidence "$EV"
echo LOGIN_RC=$?
DISPLAY=:32 XAUTHORITY= python3 $HOME/dev/qa/live_qa.py shot --display :32 --out "$EV/post_login.png"
sleep 45
DISPLAY=:32 XAUTHORITY= python3 $HOME/dev/qa/live_qa.py shot --display :32 --out "$EV/post_preload.png"
python3 $HOME/dev/qa/live_qa.py health --repeat 10
python3 $HOME/dev/qa/live_qa.py send --stream-uuid 59033cd5-64ba-4a00-a932-5dddb5c17b65 --text "CASSI QA 20261123T0935 lane718-preload"
echo LIVE718_DONE
pkill -f "$BIN" 2>/dev/null
kill $OB $XV 2>/dev/null
echo LIVE718_COMPLETE
