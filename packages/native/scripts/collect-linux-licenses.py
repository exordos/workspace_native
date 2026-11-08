#!/usr/bin/env python3
"""Collect installed Ubuntu package notices for a deployed ELF prefix, offline.

No downloading, installation, source extraction or license conclusions. Exact
file hash is preferred; GNU build ID + ELF class/machine/SONAME identifies an
SDK file after deployment changed its RPATH. Ambiguous/unmapped third-party
ELFs fail rather than guessing from the basename.
"""
import argparse
import functools
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
from urllib.parse import quote


def command(*args):
    return subprocess.check_output(args, text=True, env={**os.environ, "LC_ALL": "C"})


def digest(path):
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


@functools.cache
def identity(path):
    header = command("readelf", "-h", path)
    notes = command("readelf", "-n", path)
    dynamic = command("readelf", "-d", path)
    def field(pattern, text):
        match = re.search(pattern, text)
        return match.group(1).strip() if match else ""
    return {
        "class": field(r"Class:\s*(.+)", header),
        "machine": field(r"Machine:\s*(.+)", header),
        "buildId": field(r"Build ID:\s*([0-9a-fA-F]+)", notes).lower(),
        "soname": field(r"\(SONAME\).*?\[(.*?)\]", dynamic),
    }


@functools.cache
def candidates(basename):
    found = []
    query = subprocess.run(["dpkg-query", "-S", "*/" + basename], text=True,
                           capture_output=True, env={**os.environ, "LC_ALL": "C"})
    if query.returncode not in (0, 1):
        raise RuntimeError(query.stderr)
    for line in query.stdout.splitlines():
        packages, separator, filename = line.partition(": ")
        candidate = Path(filename)
        if not separator or not candidate.is_file() or candidate.name != basename:
            continue
        with candidate.open("rb") as stream:
            if stream.read(4) != b"\x7fELF":
                continue
        for package in packages.split(", "):
            if package and not package.startswith("diversion "):
                found.append((package, candidate.resolve()))
    return found


@functools.cache
def package_record(package):
    fields = command("dpkg-query", "-W", "-f=${binary:Package}\t${Version}\t${source:Package}\t${source:Version}\t${Architecture}", package).split("\t")
    binary, version, source, source_version, arch = fields
    source = source or binary.split(":")[0]
    source_version = source_version or version
    source_root = "https://launchpad.net/ubuntu/+archive/primary/+sourcefiles/" + quote(source, safe="") + "/" + quote(source_version, safe="") + "/"
    filename_version = source_version.split(":", 1)[-1]
    return {
        "binaryPackage": binary, "binaryVersion": version, "architecture": arch,
        "sourcePackage": source, "sourceVersion": source_version,
        "ubuntuSourcePage": "https://launchpad.net/ubuntu/+source/" + quote(source, safe="") + "/" + quote(source_version, safe=""),
        "ubuntuDscUrl": source_root + quote(source + "_" + filename_version + ".dsc", safe=""),
        "sourceLocatorVerification": "derived from exact installed source name/version; not fetched by collector",
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--prefix", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--application-elf", action="append", default=[],
                        help="Explicit prefix-relative application ELF; existing app notice covers it")
    args = parser.parse_args()
    prefix = args.prefix.resolve(strict=True)
    output = args.output_dir.resolve()
    if output.exists() and any(output.iterdir()):
        raise RuntimeError("Use an empty notices output directory")
    os_release = dict(line.split("=", 1) for line in Path("/etc/os-release").read_text().splitlines()
                      if "=" in line and not line.startswith("#"))
    if os_release.get("ID", "").strip('"') != "ubuntu":
        raise RuntimeError("Ubuntu source locators require an actual Ubuntu SDK/container")
    files = {}
    for path in sorted(prefix.rglob("*")):
        if not path.is_file():
            continue
        target = path.resolve(strict=True)
        if not target.is_relative_to(prefix):
            raise RuntimeError("Deployed file escapes prefix: " + str(path))
        with target.open("rb") as stream:
            if stream.read(4) != b"\x7fELF":
                continue
        files.setdefault(target, []).append(str(path.relative_to(prefix)))
    allowed = set(args.application_elf)
    seen_allowed = set()
    mapped, local, packages = [], [], {}
    for deployed, aliases in files.items():
        bundled_hash = digest(deployed)
        if allowed.intersection(aliases):
            seen_allowed.update(allowed.intersection(aliases))
            local.append({"paths": aliases, "sha256": bundled_hash, "notice": "existing application license, not an Ubuntu library"})
            continue
        bundled_identity = identity(str(deployed))
        exact, rebuilt_rpath = [], []
        for package, original in candidates(deployed.name):
            original_hash = digest(original)
            record = (package, original, original_hash)
            if original_hash == bundled_hash:
                exact.append(record)
            elif bundled_identity["buildId"] and identity(str(original)) == bundled_identity:
                rebuilt_rpath.append(record)
        matches = exact or rebuilt_rpath
        if not matches:
            raise RuntimeError("No installed package identity for bundled ELF: " + str(deployed))
        sources = {(package_record(pkg)["sourcePackage"], package_record(pkg)["sourceVersion"])
                   for pkg, _, _ in matches}
        if len(sources) != 1:
            raise RuntimeError("Ambiguous source package for " + str(deployed))
        origins = []
        for package, original, original_hash in matches:
            record = package_record(package)
            packages[record["binaryPackage"]] = record
            origins.append({"binaryPackage": record["binaryPackage"], "sdkFile": str(original), "sdkSha256": original_hash})
        mapped.append({"paths": aliases, "bundledSha256": bundled_hash,
                       "match": "exact-sha256" if exact else "build-id-class-machine-soname-after-deployment",
                       "elf": bundled_identity, "origins": origins})
    if seen_allowed != allowed:
        raise RuntimeError("Application ELF exclusion did not identify an actual ELF: " + repr(allowed - seen_allowed))
    output.mkdir(parents=True, exist_ok=True)
    for package, record in packages.items():
        copyright_file = Path("/usr/share/doc") / package.split(":")[0] / "copyright"
        if not copyright_file.is_file():
            raise RuntimeError("Missing installed copyright file: " + str(copyright_file))
        name = re.sub(r"[^A-Za-z0-9.+_-]", "_", package)
        destination = output / "packages" / name / "copyright"
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(copyright_file, destination)
        record["copyright"] = {"path": str(destination.relative_to(output)), "sha256": digest(destination)}
        common = []
        for filename in sorted(set(re.findall(r"/usr/share/common-licenses/([A-Za-z0-9][A-Za-z0-9.+-]*)", copyright_file.read_text()))):
            filename = filename.rstrip(".")
            source = Path("/usr/share/common-licenses") / filename
            if not source.is_file():
                raise RuntimeError("Referenced common license missing: " + str(source))
            copied = output / "common" / filename
            copied.parent.mkdir(exist_ok=True)
            shutil.copyfile(source, copied)
            common.append({"path": str(copied.relative_to(output)), "sha256": digest(copied)})
        record["commonLicenses"] = common
    manifest = {"schema": "native-linux-bundled-licenses-v1", "osRelease": os_release,
                "libraries": mapped, "applicationElf": local, "packages": packages,
                "scope": "ELF origin/notices only; existing app/MD4C/OpenSSL/emoji/font notices remain; source URLs not downloaded or availability-verified"}
    (output / "bundled-libraries.json").write_text(json.dumps(manifest, indent=2) + "\n")


if __name__ == "__main__":
    main()
