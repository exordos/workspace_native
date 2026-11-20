#!/usr/bin/env python3
"""Build and archive the native application on a Windows or macOS host."""
import hashlib
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys

root = Path.cwd()
build = root / "build" / "desktop-package"
prefix = build / "dependencies"
stage = build / "stage"
artifacts = build / "archives"
windows = sys.platform == "win32"
if not windows and sys.platform != "darwin":
    sys.exit("Run the platform packaging job on Windows or macOS.")
windows_arm64 = windows and os.environ.get("WORKSPACE_PACKAGE_PLATFORM") == "windows-arm64"
generator = ["-G", "Ninja"]
if windows:
    generator = ["-A", "ARM64"] if windows_arm64 else ["-G", "Visual Studio 17 2022", "-A", "x64"]
qt = Path(os.environ["QT_ROOT_DIR"])
openssl = Path(os.environ["WORKSPACE_OPENSSL_PREFIX"])
if not qt.is_dir() or not openssl.is_dir():
    sys.exit("The Qt and OpenSSL prefixes must exist.")

def run(*args):
    subprocess.run([str(arg) for arg in args], check=True)

def install(name, source, destination, *options):
    tree = build / name
    run("cmake", "-S", source, "-B", tree, *generator, "-DCMAKE_BUILD_TYPE=Release",
        f"-DCMAKE_INSTALL_PREFIX={destination}", f"-DCMAKE_PREFIX_PATH={prefix};{qt}", *options)
    run("cmake", "--build", tree, "--config", "Release", "--parallel", "3")
    run("cmake", "--install", tree, "--config", "Release")

install("markdown", root / "deps/md4c", prefix, "-DBUILD_SHARED_LIBS=OFF",
        "-DBUILD_MD2HTML_EXECUTABLE=OFF")
install("keychain", root / "deps/qtkeychain", prefix, "-DBUILD_WITH_QT6=ON",
        "-DBUILD_SHARED_LIBS=OFF", "-DBUILD_TRANSLATIONS=OFF", "-DBUILD_TEST_APPLICATION=OFF")
install("application", root / "packages/native", stage, "-DBUILD_TESTING=OFF",
        f"-DOPENSSL_ROOT_DIR={openssl}", "-DOPENSSL_USE_STATIC_LIBS=TRUE",
        f"-DWORKSPACE_NATIVE_PYTHON3={sys.executable}")
artifacts.mkdir(parents=True, exist_ok=True)
if windows:
    name = "workspace-native-windows-arm64" if windows_arm64 else "workspace-native-windows-x64"
else:
    machine = platform.machine()
    mac_arch = {"arm64": "arm64", "x86_64": "x64"}.get(machine)
    if mac_arch is None:
        sys.exit(f"Unsupported macOS package architecture: {machine}")
    name = f"workspace-native-macos-{mac_arch}"
archive = artifacts / f"{name}.zip"
# Workspace uses SQLite. Optional Qt SQL plugins such as Mimer and
# PostgreSQL import external SDK libraries that are absent on clean hosts.
sql_drivers = stage / ("plugins/sqldrivers" if windows else "workspace-native.app/Contents/PlugIns/sqldrivers")
sqlite_name = "qsqlite.dll" if windows else "libqsqlite.dylib"
if not (sql_drivers / sqlite_name).is_file():
    sys.exit("The package is missing the Qt SQLite driver.")
for driver in sql_drivers.glob("qsql*.dll" if windows else "libqsql*.dylib"):
    if driver.name != sqlite_name:
        driver.unlink()
if windows:
    if not (stage / "bin/workspace-native.exe").is_file():
        sys.exit("The Windows package is missing its executable.")
    shutil.make_archive(str(archive.with_suffix("")), "zip", stage)
else:
    app = stage / "workspace-native.app"
    if not (app / "Contents/MacOS/workspace-native").is_file():
        sys.exit("The macOS package is missing its application bundle.")
    run("codesign", "--force", "--deep", "--sign", "-", app)
    run("codesign", "--verify", "--deep", "--strict", app)
    run("ditto", "-c", "-k", "--sequesterRsrc", stage, archive)
with archive.open("rb") as stream:
    digest = hashlib.file_digest(stream, "sha256").hexdigest()
(artifacts / "SHA256SUMS").write_text(f"{digest}  {archive.name}\n", encoding="ascii")
print(f"Created {archive.name} ({archive.stat().st_size} bytes).")
