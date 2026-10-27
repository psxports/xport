import argparse
import fnmatch
import hashlib
import html
import json
import re
import shutil
import zlib
from pathlib import Path
from urllib.parse import quote
from web_output_permissions import inherit_output_permissions
from video_mp4 import index_mp4
from convert_video import inspect_str


PACKAGE_LIMIT = 25 * 1024 * 1024
PACKAGE_UNPACKED_LIMIT = 128 * 1024 * 1024
PACKAGE_READ_BYTES = 256 * 1024
VIDEO_CHUNK_BYTES = 256 * 1024


def compressed_packages(startup, remaining):
    """Keep package zero collected-only and bound final gzip and decoded sizes"""
    compressor = zlib.compressobj(9, zlib.DEFLATED, 31)
    compressed = bytearray()
    group, used, index = [], 0, 0

    def candidate(block):
        trial = compressor.copy()
        encoded = trial.compress(block)
        final_size = len(compressed) + len(encoded) + len(trial.copy().flush())
        return trial, encoded, final_size

    def finish():
        data = bytes(compressed) + compressor.flush()
        if len(data) > PACKAGE_LIMIT:
            raise ValueError('Compressed package exceeds hosting limit')
        return data

    for phase, items in enumerate((startup, remaining)):
        if phase == 1 and index == 0:
            yield group, finish()
            compressor = zlib.compressobj(9, zlib.DEFLATED, 31)
            compressed, group, used, index = bytearray(), [], 0, 1
        for source, target, size in items:
            if size == 0:
                group.append((source, target, 0, 0))
                continue
            with source.open('rb') as stream:
                offset = 0
                while offset < size:
                    capacity = min(PACKAGE_READ_BYTES, PACKAGE_UNPACKED_LIMIT - used, size - offset)
                    if capacity == 0:
                        yield group, finish()
                        compressor = zlib.compressobj(9, zlib.DEFLATED, 31)
                        compressed, group, used = bytearray(), [], 0
                        index += 1
                        continue
                    stream.seek(offset)
                    block = stream.read(capacity)
                    if len(block) != capacity:
                        raise ValueError('Short package source read: '+str(source))
                    trial, encoded, final_size = candidate(block)
                    length = len(block)
                    full = final_size > PACKAGE_LIMIT
                    if full:
                        low, high = 0, length
                        while low < high:
                            middle = (low + high + 1) // 2
                            if candidate(block[:middle])[2] <= PACKAGE_LIMIT:
                                low = middle
                            else:
                                high = middle - 1
                        length = low
                        if length:
                            trial, encoded, final_size = candidate(block[:length])
                    if length:
                        compressor = trial
                        compressed.extend(encoded)
                        if group and group[-1][0:2] == (source, target) and group[-1][2] + group[-1][3] == offset:
                            previous = group[-1]
                            group[-1] = (source, target, previous[2], previous[3] + length)
                        else:
                            group.append((source, target, offset, length))
                        offset += length
                        used += length
                    if full:
                        if not used:
                            raise ValueError('Hosting limit cannot fit a gzip package')
                        yield group, finish()
                        compressor = zlib.compressobj(9, zlib.DEFLATED, 31)
                        compressed, group, used = bytearray(), [], 0
                        index += 1
    if group:
        yield group, finish()


def file_digest(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024*1024), b''):
            digest.update(block)
    return digest.hexdigest()


def prune_video_outputs(output, keep):
    directory = output / 'VIDEO'
    if not directory.exists():
        return
    if directory.is_symlink() or directory.resolve().parent != output.resolve():
        raise ValueError('Video output directory must stay inside publication directory')
    for path in directory.iterdir():
        if (path.name not in keep and not path.is_symlink() and path.is_file() and
                re.fullmatch(r'[0-9a-f]{32}\.mp4', path.name) and
                file_digest(path)[:32] == path.stem):
            path.unlink()


def prepared_videos(project):
    config_path = project/'xport-project.json'
    config = json.loads(config_path.read_text(encoding='utf-8-sig')) if config_path.is_file() else {}
    video = config.get('video', {})
    if video.get('web_enabled') is not True:
        return {}, []
    sector_rate = video.get('sector_rate')
    if type(sector_rate) is not int or sector_rate not in (75, 150):
        raise ValueError('Registered movies require reviewed video.sector_rate (75 or 150)')
    receipt = project/'bin/VIDEO/manifest.json'
    if not receipt.is_file():
        raise ValueError('video.web_enabled requires a prepared VIDEO manifest')
    document = json.loads(receipt.read_text(encoding='utf-8'))
    clips = document.get('clips', [])
    requested = video.get('sources')
    if requested is not None:
        if not isinstance(requested, list) or any(not isinstance(item, str) for item in requested):
            raise ValueError('Invalid video.sources')
        keys = [item.replace('\\', '/').casefold() for item in requested]
        if len(keys) != len(set(keys)):
            raise ValueError('Duplicate video.sources')
        clips = [clip for clip in clips if str(clip.get('source', '')).casefold() in keys]
        if set(keys) != {clip.get('source', '').casefold() for clip in clips}:
            raise ValueError('Prepared MP4 missing for a registered video source')
    result = {}
    outputs = []
    for clip in clips:
        source = clip.get('source', '')
        output_name = clip.get('output', '')
        for name, prefix in ((source, 'bin/DATA/'), (output_name, 'bin/VIDEO/')):
            if (not isinstance(name, str) or not name.startswith(prefix) or '\\' in name or
                    '..' in Path(name).parts or ':' in name or not (project/name).resolve().is_relative_to(project.resolve())):
                raise ValueError('Invalid registered movie path')
        original = project/source
        movie = project/output_name
        if (not original.is_file() or file_digest(original) != clip.get('source_sha256') or not movie.is_file()
                or file_digest(movie) != clip.get('sha256') or movie.stat().st_size != clip.get('bytes')):
            raise ValueError('Prepared movie source/output hash mismatch: '+source)
        if movie.stat().st_size > PACKAGE_LIMIT:
            raise ValueError('MP4 exceeds hosting limit; progressive parts are not implemented: '+source)
        tracks = index_mp4(movie)
        frame_list = clip.get('original', {}).get('frames', [])
        track = next(track for track in tracks if track['type'] == 'video')
        original_info = clip.get('original', {})
        if (len(track['samples']) != len(frame_list) or not frame_list or
                (track['width'], track['height']) != (original_info.get('width'), original_info.get('height'))):
            raise ValueError('Prepared movie frames/geometry differ from source: '+source)
        observed = inspect_str(original, include_completions=True)
        if ([(row['frame'], row['sector']) for row in observed['frames']] !=
                [(row['frame'], row['sector']) for row in frame_list]):
            raise ValueError('Original movie frame locations differ from receipt: '+source)
        virtual = '/'+source[4:]
        if any(key.casefold() == virtual.casefold() for key in result):
            raise ValueError('Duplicate registered movie source')
        filename = clip['sha256'][:32]+'.mp4'
        chunks = []
        with movie.open('rb') as stream:
            position = 0
            for block in iter(lambda: stream.read(VIDEO_CHUNK_BYTES), b''):
                chunks.append(dict(offset=position, size=len(block), hash=hashlib.sha256(block).hexdigest()))
                position += len(block)
        result[virtual] = dict(url='VIDEO/'+filename, hash=clip['sha256'], size=clip['bytes'],
                              chunks=chunks,
                              width=track['width'], height=track['height'], frameRate=clip['frame_rate'],
                              frames=[dict(number=frame['frame'], sector=frame['sector'], endSector=frame['end_sector'])
                                      for frame in observed['frames']], sectorRate=sector_rate,
                              sourceSectorBytes=original_info.get('sector_bytes', 2352),
                              sourceAudio=original_info.get('audio', []), tracks=tracks)
        outputs.append((movie, filename))
    return result, outputs


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


def collect_files(project_root, include_str=()):
    files = []
    root = project_root / "bin" / "DATA"
    for path in root.rglob("*"):
        if not path.is_file():
            continue
        relative = path.relative_to(root)
        target = Path("/") / "DATA" / relative
        if path.suffix.lower() == ".str" and not is_excluded(target, include_str):
            continue
        files.append((path, target, path.stat().st_size))
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
    parser.add_argument("--include-str", action="append", default=[], metavar="VIRTUAL_PATH_GLOB")
    args = parser.parse_args()

    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    build_lazy(args, output)


def build_lazy(args, output):
    project = args.project.resolve()
    excluded = exclusion_patterns(getattr(args, 'exclude', []))
    videos, video_outputs = prepared_videos(project)
    excluded += [key.lower() for key in videos]
    included_str = exclusion_patterns(getattr(args, 'include_str', []))
    files = collect_files(project, included_str)
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
            if Path(name).suffix.lower() == '.str' and not is_excluded(Path('/') / name, included_str):
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
    remaining = [item for item in sorted(files, key=lambda item: item[1].as_posix()) if item not in startup]
    startup_targets = {target.as_posix() for _, target, _ in startup}
    manifest = {'name': args.short_name, 'packages': [], 'files': {}, 'music': {},
                'startupPackages': [0],
                'startupFiles': [target.as_posix() for _, target, _ in startup]}
    for index, (group, data) in enumerate(compressed_packages(startup, remaining)):
        payload_offset = 0
        for source, target, offset, length in group:
            entry = manifest['files'].setdefault(target.as_posix(), {'size': source.stat().st_size, 'parts': []})
            entry['parts'].append([index, payload_offset, offset, length])
            payload_offset += length
        if index and any(target.as_posix() in startup_targets for _, target, _, _ in group):
            manifest['startupPackages'].append(index)
        digest = hashlib.sha256(data).hexdigest()
        name = f'xport-{index:02d}-{digest[:16]}.data'
        (output / name).write_bytes(data)
        manifest['packages'].append({'url': name, 'hash': digest, 'size': len(data)})
    music_output = output / 'MUSIC'
    music_output.mkdir(exist_ok=True)
    for path in sorted((project / 'bin/MUSIC').glob('*')):
        if path.suffix.upper() not in ('.WAV', '.XA') or not path.is_file():
            continue
        if is_excluded(Path('/MUSIC') / path.name, excluded):
            continue
        data = path.read_bytes()
        if len(data) > PACKAGE_LIMIT:
            raise ValueError(f'Music exceeds hosting limit: {path}')
        digest = hashlib.sha256(data).hexdigest()
        name = f'{path.stem}-{digest[:16]}{path.suffix.upper()}'
        shutil.copyfile(path, music_output / name)
        if path.stem in manifest['music']:
            raise ValueError(f'Duplicate music key: {path.stem}')
        manifest['music'][path.stem] = {'url': 'MUSIC/' + name, 'hash': digest, 'size': len(data)}
    manifest['video'] = videos
    video_directory = output/'VIDEO'
    if video_outputs:
        video_directory.mkdir(exist_ok=True)
        for source, name in video_outputs:
            shutil.copyfile(source, video_directory/name)
    encoded = 'Module.xportManifest = ' + json.dumps(manifest, separators=(',', ':')) + ';\n'
    manifest_name = 'xport-packages-' + hashlib.sha256(encoded.encode()).hexdigest()[:16] + '.js'
    (output / manifest_name).write_text(encoded, encoding='utf-8')
    runtime = args.index_template.with_name('assets.js').read_bytes()
    video_runtime = args.index_template.with_name('video.js')
    if videos:
        if not video_runtime.is_file():
            raise ValueError('Registered movies require the WebCodecs video.js runtime')
        runtime += b'\n'+video_runtime.read_bytes()
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
    prune_video_outputs(output, {name for _, name in video_outputs})
    keep = {item['url'] for item in manifest['packages']} | {manifest_name, runtime_name, 'xport-runtime.js'}
    for pattern in ('xport-*.data', 'xport-*.js'):
        for path in output.glob(pattern):
            if path.name not in keep:
                path.unlink()
    for path in output.glob('xport-icon-*.ico'):
        if path.name != icon_name:
            path.unlink()
    keep_music = {item['url'].split('/')[1] for item in manifest['music'].values()}
    for path in music_output.iterdir():
        if path.suffix.upper() not in ('.WAV', '.XA') or not path.is_file():
            continue
        if path.name not in keep_music:
            path.unlink()
    published = [output / name for name in keep if (output / name).is_file()]
    published += [output / 'index.html']
    published += [music_output / name for name in keep_music]
    published += [video_directory/name for _, name in video_outputs]
    published += [path for path in (output / 'xport-runtime.wasm', output / 'xport-runtime.js.map',
                                    output / 'xport-runtime.wasm.map') if path.is_file()]
    if icon_name:
        published.append(output / icon_name)
    corrected = inherit_output_permissions(published)
    if corrected:
        print(f'Restored destination ACL inheritance for {len(corrected)} published web files')
    print(f"Startup: {len(startup)} collected files; {len(manifest['packages'])} total packages")
    if not log.exists():
        print('No collect.log: package zero is empty; DATA will load on first access')


if __name__ == "__main__":
    main()
