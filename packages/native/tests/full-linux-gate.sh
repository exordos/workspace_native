#!/bin/bash
# Run CTest on a private 1440x900 desktop in the Linux release container.
set -euo pipefail
build_dir=${1:?expected a CMake build directory}
if [[ -e /tmp/.X11-unix/X30 || -e /tmp/.X30-lock ]]; then
    printf '%s\n' 'The release gate needs its own display :30' >&2
    exit 1
fi
state=$(mktemp -d)
export DISPLAY=:30 QT_QPA_PLATFORM=xcb QSG_RHI_BACKEND=opengl QSG_RENDER_LOOP=basic
export LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=2
export XDG_CURRENT_DESKTOP=Openbox NO_AT_BRIDGE=1 QT_ACCESSIBILITY=0
export XAUTHORITY="$state/Xauthority" XDG_RUNTIME_DIR="$state/runtime"
export XDG_CONFIG_HOME="$state/config" XDG_CACHE_HOME="$state/cache" XDG_DATA_HOME="$state/data"
unset WAYLAND_DISPLAY QT_QUICK_BACKEND
display_pid=
wm_pid=
pulse_pid=
bus_pid=
cleanup() {
    if [[ -n "$wm_pid" ]]; then kill "$wm_pid" 2>/dev/null || true; wait "$wm_pid" || true; fi
    if [[ -n "$display_pid" ]]; then kill "$display_pid" 2>/dev/null || true; wait "$display_pid" || true; fi
    if [[ -n "$pulse_pid" ]]; then kill "$pulse_pid" 2>/dev/null || true; wait "$pulse_pid" || true; fi
    if [[ -n "$bus_pid" ]]; then kill "$bus_pid" 2>/dev/null || true; wait "$bus_pid" || true; fi
    rm -rf -- "$state"
}
trap cleanup EXIT
mkdir -m 700 "$XDG_RUNTIME_DIR" "$XDG_CONFIG_HOME" "$XDG_CACHE_HOME" "$XDG_DATA_HOME"
touch "$XAUTHORITY"
xauth -f "$XAUTHORITY" add "$DISPLAY" . "$(mcookie)"
Xtigervnc "$DISPLAY" -geometry 1440x900 -depth 24 -auth "$XAUTHORITY" \
    -nolisten tcp -noreset -rfbport -1 -rfbunixpath "$state/vnc" -rfbunixmode 0600 -localhost &
display_pid=$!
for _ in {1..100}; do xdpyinfo >/dev/null 2>&1 && break; sleep 0.1; done
kill -0 "$display_pid"
xdpyinfo >/dev/null
export DBUS_SESSION_BUS_ADDRESS="unix:path=$state/desktop-bus"
dbus-daemon --session --nofork --address="$DBUS_SESSION_BUS_ADDRESS" &
bus_pid=$!
for _ in {1..100}; do [[ -S "$state/desktop-bus" ]] && break; sleep 0.1; done
test -S "$state/desktop-bus"
openbox --sm-disable &
wm_pid=$!
for _ in {1..100}; do xprop -root _NET_SUPPORTING_WM_CHECK | grep -q 'window id' && break; sleep 0.1; done
xprop -root _NET_SUPPORTING_WM_CHECK | grep -q 'window id'
if [[ -z "${PULSE_SERVER:-}" ]]; then
    export PULSE_SERVER="unix:$state/pulse"
    pulseaudio -n --daemonize=no --exit-idle-time=-1 --load="module-native-protocol-unix socket=$state/pulse" --load=module-null-sink &
    pulse_pid=$!
    for _ in {1..100}; do [[ -S "$state/pulse" ]] && break; sleep 0.1; done
    test -S "$state/pulse"
    # Chromium excludes monitor sources from its microphone list.
    monitor=$(pactl get-default-source)
    [[ "$monitor" == *.monitor ]]
    pactl load-module module-remap-source master="$monitor" source_name=gate_microphone > /dev/null
    pactl set-default-source gate_microphone
fi
# Disable service activation for the fixture bus used by the controller tests.
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
# Window paint and keyboard tests exercise OpenGL and the desktop button policy.
# Start the actual GTK portal before Qt queries system appearance or registers a window.
mkdir -p "$XDG_CONFIG_HOME/xdg-desktop-portal"
cat > "$XDG_CONFIG_HOME/xdg-desktop-portal/portals.conf" <<'EOF'
[preferred]
default=gtk
EOF
ctest_args=(--test-dir "$build_dir" --parallel "$(nproc)" --output-on-failure --no-tests=error)
gdbus call --session --timeout 30 --dest org.freedesktop.portal.Desktop \
    --object-path /org/freedesktop/portal/desktop --method org.freedesktop.portal.Settings.Read \
    org.freedesktop.appearance color-scheme
gate_status=0
QT_QPA_PLATFORMTHEME=gtk3 ctest "${ctest_args[@]}" -L gui || gate_status=1

# The remaining suites execute on X11 as well. A separate empty session bus
# leaves the well-known desktop service names available to their test fixtures.
QT_QPA_PLATFORMTHEME=generic dbus-run-session --config-file "$state/bus.conf" -- \
    ctest "${ctest_args[@]}" -LE gui || gate_status=1
exit "$gate_status"
