import argparse
import fnmatch
import gzip
import hashlib
import html
import json
import re
import shutil
from pathlib import Path
from urllib.parse import quote
from web_output_permissions import inherit_output_permissions


PACKAGE_LIMIT = 25 * 1024 * 1024


def exclusion_patterns(values):
    patterns = []
    for value in values:
        parts = [part for part in value.replace('\\', '/').strip('/').split('/') if part and part != '.']
        if not parts or '..' in parts or any(':' in part for part in parts):
            raise ValueError(f'Invalid exclusion path: {value}')
        patterns.append('/' + '/'.join(parts).lower())
    return patterns


def is_excluded(target, patterns):
    return any(fnmatch.fnmatchcase(target.as_posix().lower(), pattern) for pattern in patterns)


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


def github_cover_url(project):
    config_path = project / 'xport-project.json'
    config = json.loads(config_path.read_text(encoding='utf-8-sig')) if config_path.is_file() else {}
    github = config.get('github', {})
    repository = github.get('repository')
    branch = github.get('branch')
    if not repository or not branch:
        return ''
    if not isinstance(repository, str) or not re.fullmatch(r'[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+', repository):
        raise ValueError('Invalid github.repository for social preview')
    if not isinstance(branch, str):
        raise ValueError('Invalid github.branch for social preview')
    return 'https://raw.githubusercontent.com/' + repository + '/' + quote(branch, safe='') + '/cover.png'


def social_preview(args):
    title = html.escape(args.full_name + ' (xport)', quote=True)
    page_url = 'https://psxports.to/' + quote(args.short_name, safe='') + '/'
    cover_url = html.escape(github_cover_url(args.project.resolve()), quote=True)
    tags = [f'<meta property="og:title" content="{title}">',
            '<meta property="og:type" content="website">',
            f'<meta property="og:url" content="{page_url}">',
            f'<meta name="twitter:title" content="{title}">']
    if cover_url:
        tags += [f'<meta property="og:image" content="{cover_url}">',
                 f'<meta property="og:image:alt" content="{title} cover">',
                 '<meta name="twitter:card" content="summary">',
                 f'<meta name="twitter:image" content="{cover_url}">',
                 f'<meta name="twitter:image:alt" content="{title} cover">']
    else:
        tags.append('<meta name="twitter:card" content="summary">')
    return '\n  '.join(tags)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--project", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--short-name", required=True)
    parser.add_argument("--full-name", required=True)
    parser.add_argument("--index-template", required=True, type=Path)
    parser.add_argument("--extra-file", nargs=2, action="append", default=[], metavar=("SOURCE", "TARGET"))
    parser.add_argument("--exclude", action="append", default=[], metavar="VIRTUAL_PATH_GLOB")
    args = parser.parse_args()

    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    build_lazy(args, output)


def build_lazy(args, output):
    project = args.project.resolve()
    excluded = exclusion_patterns(getattr(args, 'exclude', []))
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
    files = [item for item in files if not is_excluded(item[1], excluded)]
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
            if is_excluded(Path('/') / name, excluded):
                continue
            item = known.get('/' + name.lower())
            if item is None:
                suffix = '/' + name.lower()
                matches = [candidate for path, candidate in known.items() if path.endswith(suffix)]
                if len(matches) != 1:
                    raise ValueError(f'Unknown or ambiguous collected file: {line}')
                item = matches[0]
            if is_excluded(item[1], excluded):
                continue
            if item not in startup:
                startup.append(item)
    limit = PACKAGE_LIMIT - 65536
    groups = []

    def pack(items):
        first = len(groups)
        group, used = [], 0
        for source, target, size in items:
            offset = 0
            while offset < max(size, 1):
                if group and used == limit:
                    groups.append(group)
                    group, used = [], 0
                length = min(size - offset, limit - used)
                group.append((source, target, offset, length))
                used += length
                offset += max(length, 1)
        if group or first == 0:
            groups.append(group)

    pack(startup)
    startup_packages = list(range(len(groups)))
    pack(item for item in sorted(files, key=lambda item: item[1].as_posix()) if item not in startup)
    manifest = {'name': args.short_name, 'packages': [], 'files': {}, 'music': {},
                'startupPackages': startup_packages,
                'startupFiles': [target.as_posix() for _, target, _ in startup]}
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
        if is_excluded(Path('/MUSIC') / path.name, excluded):
            continue
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
    icon_source = project / 'src/platform/win/icon.ico'
    icon_name = None
    favicon = ''
    if icon_source.is_file():
        icon_data = icon_source.read_bytes()
        icon_name = 'xport-icon-' + hashlib.sha256(icon_data).hexdigest()[:16] + '.ico'
        (output / icon_name).write_bytes(icon_data)
        favicon = '<link rel="icon" type="image/x-icon" href="' + icon_name + '">'
    index = index.replace('@@FAVICON@@', favicon).replace('@@SOCIAL_PREVIEW@@', social_preview(args))
    index = index.replace('@@PACKAGES@@', manifest_name).replace('@@ASSETS@@', runtime_name)
    (output / 'index.html').write_text(index, encoding='utf-8')
    keep = {item['url'] for item in manifest['packages']} | {manifest_name, runtime_name, 'xport-runtime.js'}
    for pattern in ('xport-*.data', 'xport-*.js'):
        for path in output.glob(pattern):
            if path.name not in keep:
                path.unlink()
    for path in output.glob('xport-icon-*.ico'):
        if path.name != icon_name:
            path.unlink()
    keep_music = {item['url'].split('/')[1] for item in manifest['music'].values()}
    for path in music_output.glob('*.WAV'):
        if path.name not in keep_music:
            path.unlink()
    published = [output / name for name in keep if (output / name).is_file()]
    published += [output / 'index.html']
    published += [music_output / name for name in keep_music]
    published += [path for path in (output / 'xport-runtime.wasm', output / 'xport-runtime.js.map',
                                    output / 'xport-runtime.wasm.map') if path.is_file()]
    if icon_name:
        published.append(output / icon_name)
    corrected = inherit_output_permissions(published)
    if corrected:
        print(f'Restored destination ACL inheritance for {len(corrected)} published web files')
    print(f'Startup: {len(startup)} collected files; {len(groups)} total packages')
    if not log.exists():
        print('No collect.log: package zero is empty; DATA will load on first access')


if __name__ == "__main__":
    main()
