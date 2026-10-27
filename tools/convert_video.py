"""Prepare standard STR movies as H.264 MP4 assets without replacing MDEC inputs"""
import argparse
from fractions import Fraction
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import tempfile

from xport_project import atomic_json, load_project
from windows_desktop import run_hidden_desktop


SYNC = b'\x00'+b'\xff'*10+b'\x00'
ENCODING = dict(video='libx264', pixel_format='yuv420p', crf=18, preset='slow', b_frames=0,
                faststart=True, audio='aac', audio_bitrate='128k')


def digest(path):
    result = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024*1024), b''):
            result.update(block)
    return result.hexdigest()


def inspect_str(path, include_completions=False):
    """Accept one complete standard STR video channel in raw or RIFF/CDXA sectors"""
    frames = []
    audio = {}
    pending = None
    video_channel = None
    dimensions = None
    with Path(path).open('rb') as stream:
        header = stream.read(44)
        offset = 44 if header[:4] == b'RIFF' and header[8:12] == b'CDXA' else 0
        if (Path(path).stat().st_size-offset) % 2352:
            raise ValueError('Expected complete 2352-byte sectors or RIFF/CDXA')
        stream.seek(offset)
        sector = 0
        while True:
            block = stream.read(2352)
            if not block:
                break
            if block[:12] != SYNC or block[15] != 2 or block[16:20] != block[20:24]:
                raise ValueError('Invalid Mode 2 sector/subheader at sector '+str(sector))
            channel, submode = block[17], block[18]
            kind = submode & 0x0e
            if kind in (2, 8):
                if channel >= 32:
                    raise ValueError('Unsupported STR video channel')
                if block[24:28] != b'\x60\x01\x01\x80':
                    raise ValueError('Nonstandard STR video header at sector '+str(sector))
                part, parts, number, size, width, height = struct.unpack_from('<HHIIHH', block, 28)
                if not parts or part >= parts or not 0 < size <= parts*2016 or not width or not height or width % 2 or height % 2:
                    raise ValueError('Invalid frame geometry or sector count')
                if video_channel is None:
                    video_channel = channel
                    dimensions = (width, height)
                if channel != video_channel or (width, height) != dimensions:
                    raise ValueError('Multiple video channels or changing dimensions require a reviewed adapter')
                if part == 0:
                    if pending is not None or frames and number != frames[-1]['frame']+1:
                        raise ValueError('Incomplete, repeated or discontinuous STR frames')
                    pending = dict(frame=number, sector=sector, sector_count=parts, compressed_bytes=size)
                    next_part = 0
                if pending is None or part != next_part or (number, parts, size) != (
                        pending['frame'], pending['sector_count'], pending['compressed_bytes']):
                    raise ValueError('Out-of-order STR frame chunks')
                next_part += 1
                if next_part == parts:
                    if include_completions:
                        pending['end_sector'] = sector
                    frames.append(pending)
                    pending = None
            elif kind == 4:
                if channel >= 32:
                    raise ValueError('Unsupported XA audio channel')
                entry = audio.setdefault(channel, dict(channel=channel, first_sector=sector, coding=block[19], sectors=0))
                if entry['coding'] != block[19]:
                    raise ValueError('Changing XA audio format requires a reviewed adapter')
                entry['sectors'] += 1
            elif kind:
                raise ValueError('Unsupported sector kind')
            sector += 1
    if pending is not None or not frames:
        raise ValueError('No complete standard STR movie')
    if len(audio) > 1:
        raise ValueError('Multiple XA audio channels require a reviewed selection')
    return dict(layout='riff_cdxa' if offset else 'raw2352', header_bytes=offset,
                sector_bytes=2352, sectors=sector, channel=video_channel,
                width=dimensions[0], height=dimensions[1], frames=frames,
                audio=list(audio.values()))


def frame_rate(config, source):
    video = config.get('video', {})
    value = video.get('frame_rates', {}).get(source, video.get('frame_rate'))
    if value is None or isinstance(value, bool):
        raise ValueError('Review video.frame_rate or video.frame_rates['+source+']; STR does not declare FPS')
    try:
        rate = Fraction(str(value))
    except (ValueError, ZeroDivisionError) as error:
        raise ValueError('Invalid video frame rate') from error
    if not 0 < rate <= 120:
        raise ValueError('Video frame rate must be in (0, 120]')
    return str(rate)


def run(command, log):
    result = run_hidden_desktop(command, capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=3600)
    Path(log).write_text(result.stdout+result.stderr, encoding='utf-8')
    if result.returncode:
        raise ValueError('Video command failed; inspect '+str(log))
    return result.stdout


def decoded_frames(ffmpeg, path, log):
    progress = run([ffmpeg, '-nostdin', '-v', 'error', '-xerror', '-i', str(path),
                    '-map', '0:v:0', '-an', '-vsync', '0', '-progress', 'pipe:1', '-f', 'null', '-'], log)
    counts = [int(line.split('=', 1)[1]) for line in progress.splitlines() if line.startswith('frame=')]
    if not counts:
        raise ValueError('Decoder did not report any frames')
    return counts[-1]


def convert(root, config, ffmpeg=None):
    root = Path(root).resolve()
    sources = sorted(path for path in (root/'bin/DATA').rglob('*') if path.is_file() and path.suffix.lower() == '.str')
    selected = config.get('video', {}).get('sources')
    if selected is not None:
        if not isinstance(selected, list) or any(not isinstance(name, str) for name in selected):
            raise ValueError('video.sources must be a list of project-relative STR paths')
        available = {path.relative_to(root).as_posix().casefold(): path for path in sources}
        wanted = []
        for name in selected:
            parts = Path(name.replace('\\', '/'))
            if parts.is_absolute() or '..' in parts.parts or ':' in name or parts.as_posix().casefold() not in available:
                raise ValueError('Unknown or unsafe video.sources entry: '+name)
            path = available[parts.as_posix().casefold()]
            if path in wanted:
                raise ValueError('Duplicate video.sources entry: '+name)
            wanted.append(path)
        sources = wanted
    if not sources:
        return dict(status='not_applicable', clips=[], unsupported=[])
    output = root/'bin/VIDEO'
    if not output.resolve().is_relative_to(root):
        raise ValueError('VIDEO directory escapes project')
    status = root/'status/video'
    status.mkdir(parents=True, exist_ok=True)
    manifest_path = output/'manifest.json'
    previous = json.loads(manifest_path.read_text(encoding='utf-8')) if manifest_path.exists() else {}
    clips = []
    unsupported = []
    executable = None
    for path in sources:
        if not path.resolve().is_relative_to(root):
            raise ValueError('Video input escapes project')
        source = path.relative_to(root).as_posix()
        try:
            info = inspect_str(path)
        except ValueError as error:
            unsupported.append(dict(source=source, reason=str(error)))
            continue
        fps = frame_rate(config, source)
        audio_rate = config.get('video', {}).get('audio_sample_rates', {}).get(source)
        if audio_rate is not None and audio_rate not in (32000, 44100, 48000):
            raise ValueError('Unsupported video.audio_sample_rates entry: '+source)
        encoding = dict(ENCODING)
        crf = config.get('video', {}).get('crfs', {}).get(source, ENCODING['crf'])
        if type(crf) is not int or not 0 <= crf <= 51:
            raise ValueError('Unsupported video.crfs entry: '+source)
        encoding['crf'] = crf
        if audio_rate is not None:
            encoding['audio_sample_rate'] = audio_rate
        if executable is None:
            executable = ffmpeg or config.get('video', {}).get('ffmpeg') or os.environ.get('XPORT_FFMPEG') or shutil.which('ffmpeg')
            if not executable:
                raise ValueError('FFmpeg with psxstr, MDEC, libx264 and AAC support is required; set XPORT_FFMPEG')
            executable = shutil.which(str(executable)) or str(executable)
            version = run_hidden_desktop([executable, '-version'], check=True, capture_output=True, text=True, timeout=30).stdout.splitlines()[0]
            encoder = dict(path=executable, sha256=digest(executable) if Path(executable).is_file() else None, version=version)
        source_hash = digest(path)
        name = source_hash[:20]+'-'+hashlib.sha256((source+fps+json.dumps(encoding,sort_keys=True)).encode()).hexdigest()[:12]+'.mp4'
        destination = output/name
        identity = dict(source=source, source_sha256=source_hash, frame_rate=fps, encoding=encoding,
                        encoder=encoder, original=info)
        old = next((clip for clip in previous.get('clips', []) if clip.get('source') == source), None)
        if destination.exists():
            if not old or any(old.get(key) != value for key, value in identity.items()) or old.get('sha256') != digest(destination):
                raise ValueError('Unverified or modified MP4 already exists: '+str(destination))
            clips.append(old)
            continue
        output.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='encode-', dir=status) as temporary:
            encoded = Path(temporary)/'movie.mp4'
            command = [executable, '-nostdin', '-hide_banner', '-v', 'error', '-err_detect', 'explode',
                       '-r', fps, '-f', 'psxstr', '-i', str(path), '-map', '0:v:0', '-map', '0:a?',
                       '-c:v', 'libx264', '-preset', 'slow', '-crf', str(crf), '-pix_fmt', 'yuv420p', '-bf', '0',
                       '-vsync', '0', '-c:a', 'aac', '-b:a', '128k']
            if audio_rate is not None:
                command += ['-ar', str(audio_rate)]
            command += ['-movflags', '+faststart', str(encoded)]
            run(command, status/(name+'.encode.log'))
            if decoded_frames(executable, encoded, status/(name+'.verify.log')) != len(info['frames']):
                raise ValueError('MP4 decoded frame count differs from STR: '+source)
            if info['audio']:
                run([executable, '-nostdin', '-v', 'error', '-xerror', '-i', str(encoded),
                     '-map', '0:a:0', '-vn', '-f', 'null', '-'], status/(name+'.audio-verify.log'))
            os.link(encoded, destination)
        clips.append(dict(**identity, output=destination.relative_to(root).as_posix(), bytes=destination.stat().st_size,
                          sha256=digest(destination), runtime_binding='pending_webcodecs_integration'))
        atomic_json(manifest_path, dict(schema=1, clips=clips, unsupported=unsupported))
    result = dict(schema=1, status='converted' if clips else 'unsupported', clips=clips, unsupported=unsupported)
    output.mkdir(parents=True, exist_ok=True)
    atomic_json(manifest_path, result)
    atomic_json(root/'status/startup-video.json', result)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ffmpeg', help='FFmpeg executable with psxstr, MDEC, libx264 and AAC')
    args = parser.parse_args()
    root, config = load_project()
    result = convert(root, config, args.ffmpeg)
    print(json.dumps(dict(status=result['status'], converted=len(result['clips']), unsupported=result['unsupported']), indent=2))


if __name__ == '__main__':
    main()
