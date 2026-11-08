#!/usr/bin/env python3
"""Package the actual deployed Qt prefix; never include build/source folders."""
import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import tarfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--version', required=True)
    args = parser.parse_args()
    if not re.fullmatch(r'(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)', args.version):
        parser.error('version must be a stable X.Y.Z')
    if platform.system() != 'Linux' or platform.machine() != 'x86_64':
        parser.error('this package currently accepts Linux x86_64 only')
    build = args.build_dir.resolve(strict=True)
    cache = (build/'CMakeCache.txt').read_text()
    source_match = re.search(r'^WorkspaceNative_SOURCE_DIR:STATIC=(.+)$', cache, re.M)
    assert source_match, 'Expected a WorkspaceNative CMake build'
    source = Path(source_match[1])
    cmake = (source/'CMakeLists.txt').read_text()
    assert re.search(r'project\(WorkspaceNative VERSION ' + re.escape(args.version) + r'\s', cmake), 'Version/build mismatch'
    assert re.search(r'^CMAKE_PROJECT_VERSION:STATIC=' + re.escape(args.version) + r'$', cache, re.M), 'Configured build version mismatch'
    assert 'CMAKE_BUILD_TYPE:STRING=Release' in cache, 'Release build required'
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    name = f'workspace-native-{args.version}-linux-x86_64'
    stage = output/name
    assert not stage.exists(), 'Refusing to overwrite a package stage'
    subprocess.run(['cmake', '--install', str(build), '--prefix', str(stage), '--strip'], check=True)
    subprocess.run(['python3', str(source/'tests/verify-installed-runtime.py'), str(stage)], check=True)
    subprocess.run(['python3', str(source/'scripts/collect-linux-licenses.py'),
                    '--prefix', str(stage), '--output-dir', str(stage/'share/doc/workspace-native-preview/third-party'),
                    '--application-elf', 'bin/workspace-native'], check=True)
    shutil.copy2(source/'scripts/install-linux.sh', stage/'install.sh')
    (stage/'install.sh').chmod(0o755)
    (stage/'VERSION').write_text(args.version + '\n')
    metadata = {'version': args.version, 'system': 'linux', 'architecture': 'x86_64',
                'buildDistribution': dict(line.split('=', 1) for line in Path('/etc/os-release').read_text().splitlines() if '=' in line).get('PRETTY_NAME', '').strip('"'),
                'buildLibc': platform.libc_ver(), 'developmentPreview': True}
    (stage/'release.json').write_text(json.dumps(metadata, indent=2) + '\n')
    for path in stage.rglob('*'):
        if path.is_symlink():
            assert path.resolve(strict=True).is_relative_to(stage), f'External symlink: {path}'
    asset = output/f'Exordos-Workspace-Native-{args.version}-linux-x86_64.tar.gz'
    assert not asset.exists(), 'Refusing to overwrite a release archive'
    epoch = int(os.environ.get('SOURCE_DATE_EPOCH', '0'))
    def normalize(info):
        info.uid = info.gid = 0
        info.uname = info.gname = ''
        info.mtime = epoch
        return info
    with asset.open('xb') as raw, gzip.GzipFile(filename='', mode='wb', fileobj=raw, mtime=epoch) as compressed:
        with tarfile.open(fileobj=compressed, mode='w', format=tarfile.PAX_FORMAT) as archive:
            archive.add(stage, arcname=name, filter=normalize)
    digest = hashlib.file_digest(asset.open('rb'), 'sha256').hexdigest()
    (output/'SHA256SUMS').write_text(f'{digest}  {asset.name}\n')
    (output/'package-result.json').write_text(json.dumps({**metadata, 'asset': asset.name,
        'bytes': asset.stat().st_size, 'sha256': digest}, indent=2) + '\n')
    print(json.dumps({'asset': str(asset), 'bytes': asset.stat().st_size, 'sha256': digest}))


if __name__ == '__main__':
    main()
