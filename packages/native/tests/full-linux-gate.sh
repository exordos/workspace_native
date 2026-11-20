#!/bin/bash
# Run CTest on a private 1440x900 desktop in the Linux release container.
set -euo pipefail
build_dir=${1:?expected a CMake build directory}
state=$(mktemp -d)
export DISPLAY=:30 QT_QPA_PLATFORM=xcb QT_QUICK_BACKEND=software
export XAUTHORITY="$state/Xauthority" XDG_RUNTIME_DIR="$state/runtime"
export XDG_CONFIG_HOME="$state/config" XDG_CACHE_HOME="$state/cache" XDG_DATA_HOME="$state/data"
unset WAYLAND_DISPLAY
mkdir -m 700 "$XDG_RUNTIME_DIR" "$XDG_CONFIG_HOME" "$XDG_CACHE_HOME" "$XDG_DATA_HOME"
touch "$XAUTHORITY"
xauth -f "$XAUTHORITY" add "$DISPLAY" . "$(mcookie)"
display_pid=
wm_pid=
cleanup() {
    if [[ -n "$wm_pid" ]]; then kill "$wm_pid" 2>/dev/null || true; wait "$wm_pid" || true; fi
    if [[ -n "$display_pid" ]]; then kill "$display_pid" 2>/dev/null || true; wait "$display_pid" || true; fi
    pulseaudio --kill 2>/dev/null || true
    rm -rf -- "$state"
}
trap cleanup EXIT
Xtigervnc "$DISPLAY" -geometry 1440x900 -depth 24 -auth "$XAUTHORITY" \
    -nolisten tcp -noreset -rfbport -1 -rfbunixpath "$state/vnc" -rfbunixmode 0600 -localhost &
display_pid=$!
for _ in {1..100}; do xdpyinfo >/dev/null 2>&1 && break; sleep 0.1; done
xdpyinfo >/dev/null
openbox &
wm_pid=$!
for _ in {1..100}; do xprop -root _NET_SUPPORTING_WM_CHECK | grep -q 'window id' && break; sleep 0.1; done
xprop -root _NET_SUPPORTING_WM_CHECK | grep -q 'window id'
pulseaudio --start --exit-idle-time=-1
ctest --test-dir "$build_dir" --parallel "$(nproc)" --output-on-failure --no-tests=error
