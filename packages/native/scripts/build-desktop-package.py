#!/usr/bin/env python3
"""Build and archive the native application on a Windows or macOS host."""
import argparse
import hashlib
import os
from pathlib import Path
import platform
import re
import shutil
import struct
import subprocess
import sys

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--archive-only", action="store_true",
                    help="build the desktop archives without running the visual shell acceptance")
options = parser.parse_args()
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

def pe_machine(image):
    with image.open("rb") as stream:
        stream.seek(0x3C)
        stream.seek(struct.unpack("<I", stream.read(4))[0])
        if stream.read(4) != b"PE\0\0":
            raise ValueError(f"Invalid PE header in {image}")
        return struct.unpack("<H", stream.read(2))[0]

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
        "-DWORKSPACE_NATIVE_PLATFORM_SHELL_ACCEPTANCE=ON",
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
    if windows_arm64:
        # The Microsoft ARM64 redist also carries an x64 companion for emulated apps.
        companion = stage / "bin/vcruntime140_1.dll"
        if companion.is_file() and pe_machine(companion) == 0x8664:
            companion.unlink()
        for image in stage.rglob("*"):
            if image.suffix.lower() in {".exe", ".dll"} and pe_machine(image) != 0xAA64:
                sys.exit(f"The Windows ARM64 archive contains a non-native image: {image}")
    shutil.make_archive(str(archive.with_suffix("")), "zip", stage)
else:
    app = stage / "workspace-native.app"
    if not (app / "Contents/MacOS/workspace-native").is_file():
        sys.exit("The macOS package is missing its application bundle.")
    run("codesign", "--force", "--deep", "--sign", "-", app)
    run("codesign", "--verify", "--deep", "--strict", app)
    run("ditto", "-c", "-k", "--sequesterRsrc", stage, archive)
packages = [archive]
if windows:
    cache = (build / "application/CMakeCache.txt").read_text(encoding="utf-8")
    version = re.search(r"^CMAKE_PROJECT_VERSION:STATIC=([0-9]+\.[0-9]+\.[0-9]+)$", cache, re.M)
    if not version:
        sys.exit("Missing configured application version for the Windows installer.")
    arch = name.removeprefix("workspace-native-windows-")
    installer = artifacts / f"{name}-setup.msi"
    msi_build = build / "msi"
    environment = dict(os.environ, WORKSPACE_MSI_VERSION=version[1], WORKSPACE_MSI_STAGE=str(stage),
                       WORKSPACE_MSI_ARCH=arch)
    subprocess.run([os.environ.get("WORKSPACE_CPACK", "cpack"), "--config",
                    root / "packages/native/packaging/windows-msi.cmake", "-B", msi_build],
                   env=environment, check=True)
    shutil.copyfile(msi_build / installer.name, installer)
    packages.append(installer)
checksums = []
for package in packages:
    with package.open("rb") as stream:
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
    checksums.append(f"{digest}  {package.name}\n")
    print(f"Created {package.name} ({package.stat().st_size} bytes).")
(artifacts / "SHA256SUMS").write_text("".join(checksums), encoding="ascii")

if options.archive_only:
    print("Archive checks complete. Visual shell acceptance was not requested.")
    sys.exit(0)

shell_test = build / "application" / ("Release/native-platform-shell-test.exe" if windows
                                     else "native-platform-shell-test")
shell_log = build / "platform-shell.txt"
result = subprocess.run([str(shell_test), "-o", f"{shell_log},txt"])
if shell_log.is_file():
    sys.stdout.buffer.write(shell_log.read_bytes())
    sys.stdout.buffer.flush()
result.check_returncode()
