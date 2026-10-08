import argparse
import gzip
import hashlib
import html
import json
import shutil
from pathlib import Path


PACKAGE_LIMIT = 25 * 1024 * 1024


def collect_files(project_root):
    files = []
    root = project_root / "bin" / "DATA"
    for path in root.rglob("*"):
        if not path.is_file():
            continue
        relative = path.relative_to(root)
        if path.suffix.lower() == ".str":
            continue
        files.append((path, Path("/") / "DATA" / relative, path.stat().st_size))
    return sorted(files, key=lambda item: (-item[2], item[1].as_posix()))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--project", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--short-name", required=True)
    parser.add_argument("--full-name", required=True)
    parser.add_argument("--index-template", required=True, type=Path)
    parser.add_argument("--extra-file", nargs=2, action="append", default=[], metavar=("SOURCE", "TARGET"))
    args = parser.parse_args()

    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    build_lazy(args, output)


def build_lazy(args, output):
    project = args.project.resolve()
    files = collect_files(project)
    for source_text, target_text in getattr(args, 'extra_file', []):
        source = Path(source_text).resolve()
        target_name = target_text.replace('\\', '/').strip('/')
        target_parts = tuple(part for part in target_name.split('/') if part and part != '.')
        if not source.is_file():
            raise ValueError(f'Missing extra file: {source}')
        if not target_parts or '..' in target_parts or any(':' in part for part in target_parts):
            raise ValueError(f'Invalid extra target: {target_text}')
        target = Path('/') / Path(*target_parts)
        files.append((source, target, source.stat().st_size))
    known = {item[1].as_posix().lower(): item for item in files}
    startup = []
    log = project / 'src/platform/web/collect.log'
    if log.exists():
        for line in log.read_text(encoding='utf-8-sig').splitlines():
            name = line.strip().replace('\\', '/').split(';')[0].lstrip('/')
            name = '/'.join(part for part in name.split('/') if part and part != '.')
            if not name.lower().startswith('data/'):
                continue
            if Path(name).suffix.lower() == '.str':
                continue
            item = known.get('/' + name.lower())
            if item is None:
                suffix = '/' + name.lower()
                matches = [candidate for path, candidate in known.items() if path.endswith(suffix)]
                if len(matches) != 1:
                    raise ValueError(f'Unknown or ambiguous collected file: {line}')
                item = matches[0]
            if item not in startup:
                startup.append(item)
    limit = PACKAGE_LIMIT - 65536
    if sum(item[2] for item in startup) > limit:
        raise ValueError('Collected startup files exceed one 25 MiB package')
    groups = [[(source, target, 0, size) for source, target, size in startup]]
    group, used = [], 0
    for source, target, size in sorted(files, key=lambda item: item[1].as_posix()):
        if (source, target, size) in startup:
            continue
        offset = 0
        while offset < max(size, 1):
            if group and used == limit:
                groups.append(group)
                group, used = [], 0
            length = min(size - offset, limit - used)
            group.append((source, target, offset, length))
            used += length
            offset += max(length, 1)
    if group:
        groups.append(group)
    manifest = {'name': args.short_name, 'packages': [], 'files': {}, 'music': {}}
    for index, group in enumerate(groups):
        payload = bytearray()
        for source, target, offset, length in group:
            entry = manifest['files'].setdefault(target.as_posix(), {'size': source.stat().st_size, 'parts': []})
            entry['parts'].append([index, len(payload), offset, length])
            with source.open('rb') as stream:
                stream.seek(offset)
                payload.extend(stream.read(length))
        data = gzip.compress(payload, mtime=0)
        if len(data) > PACKAGE_LIMIT:
            raise ValueError('Compressed package exceeds hosting limit')
        digest = hashlib.sha256(data).hexdigest()
        name = f'xport-{index:02d}-{digest[:16]}.data'
        (output / name).write_bytes(data)
        manifest['packages'].append({'url': name, 'hash': digest, 'size': len(data)})
    music_output = output / 'MUSIC'
    music_output.mkdir(exist_ok=True)
    for path in sorted((project / 'bin/MUSIC').glob('*.WAV')):
        data = path.read_bytes()
        if len(data) > PACKAGE_LIMIT:
            raise ValueError(f'Music exceeds hosting limit: {path}')
        digest = hashlib.sha256(data).hexdigest()
        name = f'{path.stem}-{digest[:16]}.WAV'
        shutil.copyfile(path, music_output / name)
        manifest['music'][path.stem] = {'url': 'MUSIC/' + name, 'hash': digest, 'size': len(data)}
    encoded = 'Module.xportManifest = ' + json.dumps(manifest, separators=(',', ':')) + ';\n'
    manifest_name = 'xport-packages-' + hashlib.sha256(encoded.encode()).hexdigest()[:16] + '.js'
    (output / manifest_name).write_text(encoded, encoding='utf-8')
    runtime = args.index_template.with_name('assets.js').read_bytes()
    runtime_name = 'xport-assets-' + hashlib.sha256(runtime).hexdigest()[:16] + '.js'
    (output / runtime_name).write_bytes(runtime)
    index = args.index_template.read_text(encoding='utf-8').replace('@@FULL_NAME@@', html.escape(args.full_name))
    index = index.replace('@@PACKAGES@@', manifest_name).replace('@@ASSETS@@', runtime_name)
    (output / 'index.html').write_text(index, encoding='utf-8')
    keep = {item['url'] for item in manifest['packages']} | {manifest_name, runtime_name, 'xport-runtime.js'}
    for pattern in ('xport-*.data', 'xport-*.js'):
        for path in output.glob(pattern):
            if path.name not in keep:
                path.unlink()
    keep_music = {item['url'].split('/')[1] for item in manifest['music'].values()}
    for path in music_output.glob('*.WAV'):
        if path.name not in keep_music:
            path.unlink()
    print(f'Startup: {len(startup)} collected files; {len(groups)} total packages')
    if not log.exists():
        print('No collect.log: package zero is empty; DATA will load on first access')


if __name__ == "__main__":
    main()
