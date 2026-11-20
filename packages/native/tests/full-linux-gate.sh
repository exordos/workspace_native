#!/bin/bash
# Run CTest on a private 1440x900 desktop in the Linux release container.
set -euo pipefail
build_dir=${1:?expected a CMake build directory}
if [[ -e /tmp/.X11-unix/X30 || -e /tmp/.X30-lock ]]; then
    printf '%s\n' 'The release gate needs its own display :30' >&2
    exit 1
fi
state=$(mktemp -d)
export DISPLAY=:30 QT_QPA_PLATFORM=xcb QT_QUICK_BACKEND=software QSG_RENDER_LOOP=basic
export XDG_CURRENT_DESKTOP=Openbox NO_AT_BRIDGE=1 QT_ACCESSIBILITY=0
export XAUTHORITY="$state/Xauthority" XDG_RUNTIME_DIR="$state/runtime"
export XDG_CONFIG_HOME="$state/config" XDG_CACHE_HOME="$state/cache" XDG_DATA_HOME="$state/data"
unset WAYLAND_DISPLAY
mkdir -m 700 "$XDG_RUNTIME_DIR" "$XDG_CONFIG_HOME" "$XDG_CACHE_HOME" "$XDG_DATA_HOME"
touch "$XAUTHORITY"
xauth -f "$XAUTHORITY" add "$DISPLAY" . "$(mcookie)"
display_pid=
wm_pid=
pulse_pid=
cleanup() {
    if [[ -n "$wm_pid" ]]; then kill "$wm_pid" 2>/dev/null || true; wait "$wm_pid" || true; fi
    if [[ -n "$display_pid" ]]; then kill "$display_pid" 2>/dev/null || true; wait "$display_pid" || true; fi
    if [[ -n "$pulse_pid" ]]; then kill "$pulse_pid" 2>/dev/null || true; wait "$pulse_pid" || true; fi
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
if [[ -z "${PULSE_SERVER:-}" ]]; then
    export PULSE_SERVER="unix:$state/pulse"
    pulseaudio -n --daemonize=no --exit-idle-time=-1 --load="module-native-protocol-unix socket=$state/pulse" --load=module-null-sink &
    pulse_pid=$!
    for _ in {1..100}; do [[ -S "$state/pulse" ]] && break; sleep 0.1; done
    test -S "$state/pulse"
fi
# A release stand has no portal desktop session. Prevent a local installed portal
# from auto-starting and emitting failure-level Qt warnings in synthetic tests.
cat > "$state/bus.conf" <<'EOF'
<busconfig>
  <type>session</type>
  <listen>unix:tmpdir=/tmp</listen>
  <policy context="default">
    <allow send_destination="*" eavesdrop="true"/>
    <allow own="*"/>
    <allow eavesdrop="true"/>
  </policy>
</busconfig>
EOF
dbus-run-session --config-file "$state/bus.conf" -- ctest --test-dir "$build_dir" --parallel "$(nproc)" --output-on-failure --no-tests=error
