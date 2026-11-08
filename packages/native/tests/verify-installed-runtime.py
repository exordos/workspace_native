"""Check the deployed runtime, not the developer Qt installation.

Run after cmake --install, passing the staging directory. This check does not
claim runtime or operating-system acceptance; the visible installed app must
also be exercised on each supported operating system.
"""

import argparse
import configparser
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile


MODULES = (
    "QtQuick", "QtQuick/Controls", "QtQuick/Controls/Basic", "QtQuick/Templates",
    "QtQuick/Layouts", "QtQuick/Dialogs", "QtQuick/Dialogs/quickimpl", "QtQuick/Effects",
    "Qt/labs/folderlistmodel", "QtQml/WorkerScript",
)


def verify(prefix, pid=None):
    prefix = prefix.resolve(strict=True)
    bundles = list(prefix.glob("*.app/Contents"))
    if bundles:
        assert len(bundles) == 1, "Expected a single application bundle"
        contents = bundles[0]
        assert (contents/"MacOS/workspace-native").is_file(), "Missing app executable"
        qml = contents/"Resources/qml"
        plugins = contents/"PlugIns"
    else:
        binary = prefix/"bin/workspace-native"
        if not binary.is_file():
            binary = prefix/"bin/workspace-native.exe"
        assert binary.is_file(), "Missing installed executable"
        conf = binary.parent/"qt.conf"
        assert conf.is_file(), "Missing installed qt.conf"
        config = configparser.ConfigParser()
        config.read(conf, encoding="utf-8")
        assert config.has_section("Paths"), "Missing qt.conf Paths section"
        paths = config["Paths"]
        runtime_prefix = (binary.parent/paths.get("Prefix", "..")).resolve()
        assert runtime_prefix.is_relative_to(prefix), "Runtime prefix escapes package"
        qml = runtime_prefix/paths.get("QmlImports", "qml")
        plugins = runtime_prefix/paths.get("Plugins", "plugins")
    for location in (qml, plugins):
        assert location.resolve().is_relative_to(prefix), "Runtime path escapes package"
    for module in MODULES:
        directory = qml/module
        qmldir = directory/"qmldir"
        assert qmldir.is_file(), f"Missing deployed QML module: {module}"
        text = qmldir.read_text(encoding="utf-8")
        declared = re.search(r"^module\s+(\S+)", text, re.MULTILINE)
        assert declared and declared[1] == module.replace("/", "."), f"Wrong module: {module}"
        for plugin in re.findall(r"^(?:optional\s+)?plugin\s+(\S+)", text, re.MULTILINE):
            files = list(directory.glob(f"*{plugin}*.so*")) + list(directory.glob(f"*{plugin}*.dll"))
            files += list(directory.glob(f"*{plugin}*.dylib")) + list(plugins.glob(f"*{plugin}*.dylib"))
            assert files, f"Missing deployed QML plugin: {plugin}"
            assert all(file.resolve().is_relative_to(prefix) for file in files), "Plugin symlink escapes package"
    platform_plugins = plugins/"platforms"
    assert platform_plugins.is_dir() and any(platform_plugins.iterdir()), "Missing platform plugin"
    sqlite_plugins = list((plugins/"sqldrivers").glob("*qsqlite*.so*"))
    sqlite_plugins += list((plugins/"sqldrivers").glob("*qsqlite*.dll"))
    sqlite_plugins += list((plugins/"sqldrivers").glob("*qsqlite*.dylib"))
    assert sqlite_plugins, "Missing deployed SQLite cache driver"
    assert all(file.resolve().is_relative_to(prefix) for file in sqlite_plugins), "SQLite driver escapes package"
    executable_magic = b""
    if not bundles:
        with binary.open("rb") as executable:
            executable_magic = executable.read(4)
    if executable_magic == b"\x7fELF":
        translations = runtime_prefix/paths.get("Translations", "translations")
        assert translations.resolve().is_relative_to(prefix), "Translations path escapes package"
        assert (translations/"qt_ru.qm").is_file(), "Missing deployed Qt Russian catalog at configured runtime path"
        for relative in ("platforms/*qxcb*.so*", "platforms/*qwayland*.so*",
                         "wayland-shell-integration/*xdg-shell*.so*",
                         "wayland-graphics-integration-client/*wayland-egl*.so*"):
            files = list(plugins.glob(relative))
            assert files, f"Missing deployed Linux display plugin: {relative}"
            assert all(file.resolve().is_relative_to(prefix) for file in files), "Display plugin symlink escapes package"
        for relative in ("imageformats/libqsvg.so*", "iconengines/libqsvgicon.so*"):
            files = [file for file in plugins.glob(relative) if file.is_file()]
            assert files, f"Missing deployed SVG plugin: {relative}"
            assert all(file.resolve().is_relative_to(prefix) for file in files), "SVG plugin symlink escapes package"
        for name in ("libmd4c", "libmd4c-html", "libcrypto", "libssl"):
            libraries = list(prefix.glob(f"lib*/{name}.so*"))
            libraries += list(prefix.glob(f"lib*/*/{name}.so*"))
            assert libraries, f"Missing deployed native dependency: {name}"
            assert all(file.resolve().is_relative_to(prefix) for file in libraries), "Native library symlink escapes package"
        needed = subprocess.run(["patchelf", "--print-needed", str(binary)], check=True,
                                capture_output=True, text=True).stdout.splitlines()
        assert any(name.startswith("libssl.so.") for name in needed), "Application must directly bind its bundled SSL runtime"
    assert list(prefix.glob("**/doc/workspace-native-preview/md4c.LICENSE")), "Missing Markdown parser license"
    assert list(prefix.glob("**/doc/workspace-native-preview/openssl.LICENSE")), "Missing OpenSSL license"
    print(f"Installed runtime check passed: {len(MODULES)} QML modules, their plugins, qt.conf, platform, SQLite and Markdown dependencies")
    if pid is not None:
        assert sys.platform.startswith("linux"), "Process mapping check requires Linux"
        process = Path(f"/proc/{pid}")
        assert (process/"exe").resolve().is_relative_to(prefix), "Process is not the installed application"
        maps = (process/"maps").read_text(encoding="utf-8")
        libraries = {line.split()[-1] for line in maps.splitlines()
                     if "/libQt6" in line or "/qt6/qml/" in line or "/qt6/plugins/" in line
                     or "/libqt6keychain" in line or "/libmd4c" in line
                     or "/libcrypto.so" in line or "/libssl.so" in line}
        assert len(libraries) >= 10, "Expected the visible application's Qt runtime to be loaded"
        for name in ("libmd4c.so", "libmd4c-html.so"):
            assert any(Path(path).name.startswith(name) for path in libraries), f"Markdown parser was not loaded: {name}"
        external = [path for path in libraries if not Path(path).resolve().is_relative_to(prefix)]
        assert not external, "Application loaded runtime libraries outside the package: " + ", ".join(sorted(external))
        print(f"Process mapping check passed: {len(libraries)} Qt/keychain/QML/plugin/Markdown libraries inside package")


def probe_runtime(prefix, executable):
    """Run synthetic crypto/TLS operations with the installed runtime; no network."""
    assert sys.platform.startswith("linux"), "The runtime probe currently checks Linux mappings"
    binary_directory = prefix.resolve(strict=True)/"bin"
    descriptor, temporary = tempfile.mkstemp(prefix=".cassi-runtime-probe-", dir=binary_directory)
    os.close(descriptor)
    temporary = Path(temporary)
    try:
        shutil.copy2(executable, temporary)
        subprocess.run(["patchelf", "--set-rpath", "$ORIGIN/../lib", str(temporary)], check=True,
                       capture_output=True, text=True)
        result = subprocess.run([str(temporary)], capture_output=True, text=True, timeout=15)
        print(result.stdout.strip())
        assert result.returncode == 0, f"Installed crypto/TLS runtime probe failed ({result.returncode})"
    finally:
        temporary.unlink(missing_ok=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("prefix", type=Path)
    parser.add_argument("--pid", type=int, help="Also verify a running installed Linux app uses packaged Qt")
    parser.add_argument("--probe", type=Path, help="Temporarily run the built native-runtime-probe inside Linux staging")
    args = parser.parse_args()
    try:
        verify(args.prefix, args.pid)
        if args.probe is not None:
            probe_runtime(args.prefix, args.probe)
    except (AssertionError, OSError, configparser.Error, subprocess.SubprocessError) as error:
        print(f"Installed runtime check failed: {error}", file=sys.stderr)
        sys.exit(1)
