"""Convert PlayStation CD audio tracks to native IMA ADPCM WAV files"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import wave

from xport_project import load_project


SECTOR_BYTES = 2352
FRAMES_PER_SECOND = 75


def cue_time(value):
    match = re.fullmatch(r'(\d+):(\d{2}):(\d{2})', value)
    if not match or int(match.group(2)) >= 60 or int(match.group(3)) >= FRAMES_PER_SECOND:
        raise ValueError('Invalid CUE time: ' + value)
    return (int(match.group(1)) * 60 + int(match.group(2))) * FRAMES_PER_SECOND + int(match.group(3))


def parse_cue(path):
    try:
        text = path.read_text(encoding='utf-8-sig')
    except UnicodeDecodeError:
        text = path.read_text(encoding='cp1252')
    current_file = None
    current_track = None
    tracks = []
    for line_number, line in enumerate(text.splitlines(), 1):
        file_match = re.match(r'^\s*FILE\s+(?:"([^"]+)"|(\S+))\s+(\S+)\s*$', line, re.IGNORECASE)
        if file_match:
            current_file = file_match.group(1) or file_match.group(2)
            continue
        track_match = re.match(r'^\s*TRACK\s+(\d+)\s+(\S+)\s*$', line, re.IGNORECASE)
        if track_match:
            if current_file is None:
                raise ValueError(f'TRACK before FILE at {path}:{line_number}')
            current_track = dict(number=int(track_match.group(1)), mode=track_match.group(2).upper(),
                                 file=current_file, indexes={})
            tracks.append(current_track)
            continue
        index_match = re.match(r'^\s*INDEX\s+(\d+)\s+(\d+:\d{2}:\d{2})\s*$', line, re.IGNORECASE)
        if index_match:
            if current_track is None:
                raise ValueError(f'INDEX before TRACK at {path}:{line_number}')
            current_track['indexes'][int(index_match.group(1))] = cue_time(index_match.group(2))
    numbers = [track['number'] for track in tracks]
    if not tracks or len(numbers) != len(set(numbers)):
        raise ValueError('CUE has no tracks or duplicate track numbers: ' + str(path))
    return tracks


def audio_ranges(cue_path):
    tracks = parse_cue(cue_path)
    result = []
    for index, track in enumerate(tracks):
        if track['mode'] != 'AUDIO':
            continue
        if 1 not in track['indexes']:
            raise ValueError(f"Audio track {track['number']} has no INDEX 01")
        source = (cue_path.parent / track['file']).resolve()
        if not source.is_file():
            raise FileNotFoundError(source)
        start_sector = track['indexes'][1]
        end_sector = source.stat().st_size // SECTOR_BYTES
        if source.stat().st_size % SECTOR_BYTES:
            raise ValueError('CD audio file size is not sector aligned: ' + str(source))
        for following in tracks[index + 1:]:
            following_source = (cue_path.parent / following['file']).resolve()
            if following_source != source:
                break
            boundary = following['indexes'].get(0, following['indexes'].get(1))
            if boundary is None:
                raise ValueError(f"Track {following['number']} has no usable index")
            end_sector = boundary
            break
        if start_sector >= end_sector:
            raise ValueError(f"Audio track {track['number']} has an empty or reversed range")
        result.append(dict(number=track['number'], source=source,
                           offset=start_sector * SECTOR_BYTES,
                           size=(end_sector - start_sector) * SECTOR_BYTES))
    if not result:
        raise ValueError('CUE has no AUDIO tracks: ' + str(cue_path))
    return result


def encoder_path():
    executable = Path(__file__).resolve().parent / 'adpcm-xq' / 'adpcm-xq.exe'
    if not executable.is_file():
        raise RuntimeError('ADPCM-XQ is not prepared; run [XPORT_ROOT]\\tools\\prepare.bat')
    return executable


def write_pcm_wave(track, destination):
    with track['source'].open('rb') as source, wave.open(str(destination), 'wb') as output:
        source.seek(track['offset'])
        output.setparams((2, 2, 44100, 0, 'NONE', 'not compressed'))
        remaining = track['size']
        while remaining:
            block = source.read(min(1024 * 1024, remaining))
            if not block:
                raise EOFError('Unexpected end of CD audio file: ' + str(track['source']))
            output.writeframesraw(block)
            remaining -= len(block)


def validate_output(path):
    data = path.read_bytes()[:128]
    if len(data) < 36 or data[:4] != b'RIFF' or data[8:12] != b'WAVE':
        raise ValueError('Encoder did not produce a RIFF/WAVE file: ' + str(path))
    fmt = data.find(b'fmt ')
    if fmt < 0 or fmt + 24 > len(data):
        raise ValueError('Encoded WAV has no valid fmt chunk: ' + str(path))
    format_tag, channels, sample_rate = struct.unpack_from('<HHI', data, fmt + 8)
    bits = struct.unpack_from('<H', data, fmt + 22)[0]
    if (format_tag, channels, sample_rate, bits) != (0x11, 2, 44100, 4):
        raise ValueError(f'Unexpected encoded WAV format: {format_tag}, {channels}, {sample_rate}, {bits}')


def convert(root, config, input_dir=None, output_dir=None):
    paths = config.get('paths', {})
    source = Path(input_dir) if input_dir else root / paths.get('music_source', 'iso')
    output = Path(output_dir) if output_dir else root / paths.get('music_output', 'bin/MUSIC')
    source = source.resolve()
    output = output.resolve()
    cues = sorted(source.glob('*.cue'))
    if len(cues) != 1:
        raise ValueError(f'Expected exactly one CUE file in {source}, found {len(cues)}')
    capability = config.get('capabilities', {}).get('redbook_audio', {})
    if capability.get('enabled') is False:
        reason = capability.get('reason')
        if not isinstance(reason, str) or not reason.strip():
            raise ValueError('Disabled redbook_audio capability requires a nonempty reason')
        tracks = parse_cue(cues[0])
        if any(track['mode'] == 'AUDIO' for track in tracks):
            raise ValueError('redbook_audio is disabled but the CUE contains AUDIO tracks: ' + str(cues[0]))
        return dict(status='not_applicable', reason=reason, input=str(cues[0]), tracks=[])
    tracks = audio_ranges(cues[0])
    encoder = encoder_path()
    output.mkdir(parents=True, exist_ok=True)
    receipts = []
    with tempfile.TemporaryDirectory(prefix='xport-music-', dir=root / 'tools') as temporary_dir:
        temporary_dir = Path(temporary_dir)
        for track in tracks:
            pcm = temporary_dir / f"{track['number']}.pcm.wav"
            encoded = temporary_dir / f"{track['number']}.WAV"
            write_pcm_wave(track, pcm)
            subprocess.run([str(encoder), '-q', '-y', '-5', '-w4', str(pcm), str(encoded)], check=True)
            validate_output(encoded)
            destination = output / encoded.name
            encoded.replace(destination)
            receipts.append(dict(track=track['number'], output=str(destination), bytes=destination.stat().st_size,
                                 sha256=hashlib.sha256(destination.read_bytes()).hexdigest()))
    return dict(status='converted', codec='IMA ADPCM WAV', encoder='ADPCM-XQ 0.5', bits=4,
                lookahead=5, input=str(cues[0]), output=str(output), tracks=receipts)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', help='Override the configured ISO directory')
    parser.add_argument('--output', help='Override the configured music output directory')
    args = parser.parse_args()
    root, config = load_project()
    print(json.dumps(convert(root, config, args.input, args.output), indent=2))


if __name__ == '__main__':
    main()
