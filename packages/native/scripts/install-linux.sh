#!/usr/bin/env bash
set -euo pipefail

# Per-user installation: version directories are immutable. Settings and the
# message cache live separately and are never copied, reset, or removed here.
source_dir=$(CDPATH='' cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
version=$(cat -- "$source_dir/VERSION")
[[ $version =~ ^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$ ]] || exit 2
[[ $# == 0 || ( $# == 1 && $1 == --no-launch ) ]] || exit 2
data_home=${XDG_DATA_HOME:-$HOME/.local/share}
[[ $data_home == /* && $HOME == /* ]] || exit 2
root=$data_home/workspace-native-install
bin_dir=$HOME/.local/bin
desktop_dir=$data_home/applications
[[ ! -L $root ]] || { echo 'The installation directory must not be a symbolic link.' >&2; exit 1; }
mkdir -p -- "$root/versions" "$bin_dir" "$desktop_dir"
[[ -O $root && ! -L $root/versions && -O $root/versions ]] || exit 1
[[ -f $source_dir/bin/workspace-native && -x $source_dir/bin/workspace-native ]] || exit 1
launcher=$bin_dir/workspace-native
desktop=$desktop_dir/com.exordos.workspace.native.preview.desktop
for existing in "$launcher" "$desktop"; do
    if [[ -e $existing || -L $existing ]]; then
        [[ ! -L $existing && -f $existing ]] && grep -q '^# Workspace Native managed installation$' "$existing" || {
            echo 'An existing launcher belongs to another installation.' >&2; exit 1;
        }
    fi
done
for temporary in "$root/.current-next" "$launcher.new" "$desktop.new"; do
    [[ ! -e $temporary && ! -L $temporary ]] || { echo 'An installation temporary path is already occupied.' >&2; exit 1; }
done
mkdir -- "$root/.install-lock" || { echo 'Another installation is running.' >&2; exit 1; }
staging=''
target_created=false
committed=false
cleanup() {
    [[ -z $staging ]] || rm -rf -- "$staging"
    if $target_created && ! $committed; then rm -rf -- "$target"; fi
    rm -f -- "$root/.current-next" "$launcher.new" "$desktop.new"
    rmdir -- "$root/.install-lock"
}
trap cleanup EXIT
trap 'exit 143' TERM
trap 'exit 130' INT
target=$root/versions/$version
[[ ! -e $target && ! -L $target ]] || { echo 'This version is already installed.' >&2; exit 1; }
staging=$(mktemp -d -- "$root/versions/.install-XXXXXXXX")
cp -a -- "$source_dir/." "$staging/"
{
    printf '#!/usr/bin/env bash\n# Workspace Native managed installation\nexec '
    printf '%q' "$root/current/bin/workspace-native"
    printf ' "$@"\n'
} > "$launcher.new"
chmod 755 -- "$launcher.new"
# Desktop Entry strings use their own escaping, rather than shell quoting.
desktop_exec=${launcher//\\/\\\\}
desktop_exec=${desktop_exec//\"/\\\"}
desktop_exec=${desktop_exec//\`/\\\`}
desktop_exec=${desktop_exec//\$/\\\$}
desktop_exec=${desktop_exec//%/%%}
# KeyFile decoding runs before Exec argument decoding, so preserve that
# argument's backslashes through the first stage as well.
desktop_exec=${desktop_exec//\\/\\\\}
icon=$root/current/share/icons/hicolor/512x512/apps/com.exordos.workspace.native.preview.png
icon=${icon//\\/\\\\}
cat > "$desktop.new" <<EOF
[Desktop Entry]
# Workspace Native managed installation
Type=Application
Name=Workspace Native Preview
Comment=Native Exordos Workspace development preview
Exec="$desktop_exec" %U
Icon=$icon
Terminal=false
Categories=Network;InstantMessaging;
StartupNotify=true
StartupWMClass=com.exordos.workspace.native.preview
EOF
chmod 644 -- "$desktop.new"
mv -- "$staging" "$target"
staging=''
target_created=true
mv -f -- "$launcher.new" "$launcher"
mv -f -- "$desktop.new" "$desktop"
ln -s -- "versions/$version" "$root/.current-next"
mv -Tf -- "$root/.current-next" "$root/current"
committed=true
printf 'Workspace Native %s installed. Launch: %s\n' "$version" "$launcher" || true
