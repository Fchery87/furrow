#!/usr/bin/env python3
"""Package locally built Furrow artifacts without modifying their source bundles."""
import argparse
import hashlib
import json
import platform
import shutil
import tarfile
import tempfile
import zipfile
from pathlib import Path


def package(build, destination, system):
    root = Path(__file__).resolve().parents[1]
    artifacts = build / 'FurrowSpike_artefacts' / 'Release'
    required = [artifacts / 'VST3' / 'Furrow Spike.vst3', artifacts / 'CLAP' / 'Furrow Spike.clap']
    standalone = artifacts / 'Standalone' / ('Furrow Spike.exe' if system == 'windows' else 'Furrow Spike.app' if system == 'darwin' else 'Furrow Spike')
    required.append(standalone)
    absent = [str(path) for path in required if not path.exists()]
    if absent:
        raise ValueError('Missing build artifacts: ' + ', '.join(absent))
    destination.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='furrow-package-', dir=destination) as temp:
        stage = Path(temp) / 'furrow'
        stage.mkdir()
        for source in required:
            target = stage / source.parent.name / source.name
            target.parent.mkdir(exist_ok=True)
            if source.is_dir():
                shutil.copytree(source, target, symlinks=True)
            else:
                shutil.copy2(source, target)
        shutil.copy2(root / 'LICENSE', stage / 'LICENSE')
        shutil.copy2(root / 'README.md', stage / 'README.md')
        shutil.copytree(root / 'docs', stage / 'docs')
        files = {}
        for path in sorted(stage.rglob('*')):
            if path.is_symlink():
                files[str(path.relative_to(stage))] = {'symlink': str(path.readlink())}
            elif path.is_file():
                files[str(path.relative_to(stage))] = {'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}
        manifest = {'platform': system, 'source': 'https://github.com/Fchery87/furrow', 'files': files}
        (stage / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
        if system == 'windows':
            archive = destination / 'furrow-windows.zip'
            with zipfile.ZipFile(archive, 'w', compression=zipfile.ZIP_DEFLATED) as out:
                for path in sorted(stage.rglob('*')):
                    if path.is_file():
                        out.write(path, path.relative_to(stage.parent))
        else:
            archive = destination / f'furrow-{system}.tar.gz'
            with tarfile.open(archive, 'w:gz', dereference=False) as out:
                out.add(stage, arcname='furrow')
        print(archive)
        return archive


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--platform', choices=['linux', 'darwin', 'windows'], default=platform.system().lower())
    args = parser.parse_args()
    try:
        package(args.build, args.output, args.platform)
    except (ValueError, OSError) as error:
        parser.exit(1, f'Packaging failed: {error}\n')


if __name__ == '__main__':
    main()
