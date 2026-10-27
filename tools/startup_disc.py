"""Extract ISO9660 data tracks without copying binaries into orig"""
import hashlib
import os
from pathlib import Path
import re
import tempfile

from convert_music import parse_cue


FORMATS = {'MODE1/2048': (2048, 0), 'MODE1/2352': (2352, 16),
           'MODE2/2352': (2352, 24), 'MODE2/2336': (2336, 8)}


def inside(base, path):
    path = path.resolve()
    if not path.is_relative_to(base.resolve()):
        raise ValueError('Disc path escapes its directory: '+str(path))
    return path


def discover(root):
    base = Path(root)/'iso'
    descriptors = sorted(base.rglob('*')) if base.exists() else []
    referenced = set()
    discs = []
    for cue in descriptors:
        if not cue.is_file() or cue.suffix.lower() != '.cue':
            continue
        tracks = parse_cue(cue)
        for track in tracks:
            source = inside(base, cue.parent/track['file'])
            if not source.is_file():
                raise ValueError('Missing CUE track: '+str(source))
            referenced.add(source)
        data = [track for track in tracks if track['mode'] != 'AUDIO']
        if len(data) != 1 or data[0]['mode'] not in FORMATS or 1 not in data[0]['indexes']:
            raise ValueError('CUE requires one supported data track with INDEX 01: '+str(cue))
        track = data[0]
        source = inside(base, cue.parent/track['file'])
        stride, offset = FORMATS[track['mode']]
        start = track['indexes'][1]
        end = source.stat().st_size//stride
        for following in tracks[tracks.index(track)+1:]:
            if inside(base, cue.parent/following['file']) == source:
                end = following['indexes'].get(0, following['indexes'].get(1, -1))
                break
        audio = []
        if stride == 2352:
            file_bases = {}
            position = 0
            for item in tracks:
                item_source = inside(base, cue.parent/item['file'])
                if item_source not in file_bases:
                    if item_source.stat().st_size % stride:
                        raise ValueError('CUE track is not sector aligned: '+str(item_source))
                    file_bases[item_source] = position
                    position += item_source.stat().st_size//stride
            origin = file_bases[source]+start
            for index, item in enumerate(tracks):
                if item['mode'] != 'AUDIO' or 1 not in item['indexes']:
                    continue
                item_source = inside(base, cue.parent/item['file'])
                begin = item['indexes'][1]
                finish = item_source.stat().st_size//stride
                for following in tracks[index+1:]:
                    if inside(base, cue.parent/following['file']) == item_source:
                        finish = following['indexes'].get(0, following['indexes'].get(1, -1))
                        break
                if re.search(rb'^\s*(?:PREGAP|POSTGAP)\b', cue.read_bytes(), re.MULTILINE | re.IGNORECASE):
                    continue
                audio.append(dict(source=item_source, offset=begin*stride, track=item['number'],
                                  start=file_bases[item_source]+begin-origin,
                                  end=file_bases[item_source]+finish-origin))
        discs.append(dict(descriptor=cue, source=source, stride=stride, offset=offset, start=start, end=end,
                          audio=audio))
    for source in descriptors:
        if not source.is_file() or source.resolve() in referenced or source.suffix.lower() not in ('.iso', '.bin', '.img'):
            continue
        inside(base, source)
        matches = []
        with source.open('rb') as stream:
            for stride, offset in sorted(set(FORMATS.values())):
                stream.seek(16*stride+offset)
                if stream.read(7) == b'\x01CD001\x01':
                    matches.append((stride, offset))
        if len(matches) != 1:
            raise ValueError('Cannot identify ISO9660 image: '+str(source))
        stride, offset = matches[0]
        discs.append(dict(descriptor=source, source=source, stride=stride, offset=offset,
                          start=0, end=source.stat().st_size//stride))
    return sorted(discs, key=lambda item: item['descriptor'].as_posix().lower())


class IsoReader:
    def __init__(self, disc, stream):
        self.disc = disc
        self.stream = stream
        self.audio_files = {}

    def sector(self, lba, raw=False):
        disc = self.disc
        physical = disc['start']+lba
        if lba < 0 or physical >= disc['end']:
            raise ValueError('ISO extent exceeds data track')
        self.stream.seek(physical*disc['stride'])
        data = self.stream.read(disc['stride'])
        if len(data) != disc['stride']:
            raise ValueError('Truncated ISO sector')
        return data if raw else data[disc['offset']:disc['offset']+2048]

    def entries(self):
        pvd = self.sector(16)
        if pvd[:7] != b'\x01CD001\x01' or int.from_bytes(pvd[128:130], 'little') != 2048:
            raise ValueError('Expected ISO9660 with 2048-byte logical blocks')
        declared_volume = int.from_bytes(pvd[80:84], 'little')
        # Some PlayStation volumes include following audio tracks in their size
        volume = min(declared_volume, self.disc['end']-self.disc['start'])
        visited = set()

        def walk(record, parent, depth):
            lba = int.from_bytes(record[2:6], 'little')
            size = int.from_bytes(record[10:14], 'little')
            if depth > 64 or (lba, size) in visited:
                raise ValueError('Cyclic or excessively deep ISO directory')
            visited.add((lba, size))
            if size > 64*1024*1024 or lba+(size+2047)//2048 > volume:
                raise ValueError('Invalid ISO directory extent')
            remaining = size
            for sector in range((size+2047)//2048):
                block = self.sector(lba+sector)[:remaining]
                remaining -= len(block)
                position = 0
                while position < len(block) and block[position]:
                    length = block[position]
                    row = block[position:position+length]
                    if length < 34 or len(row) != length or 33+row[32] > length:
                        raise ValueError('Invalid ISO directory record')
                    position += length
                    name = row[33:33+row[32]]
                    if name in (b'\x00', b'\x01'):
                        continue
                    name = re.sub(r';\d+$', '', name.decode('ascii')).rstrip('.')
                    if (not name or name in ('.', '..') or re.search(r'[<>:"/\\|?*\x00-\x1f]', name)
                            or name.endswith(' ') or re.match(r'(?i)^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\.|$)', name)):
                        raise ValueError('Unsafe ISO filename: '+name)
                    path = parent/name
                    if row[1] or row[25] & 0x80 or row[26] or row[27]:
                        raise ValueError('Unsupported extended, multi-extent or interleaved ISO file: '+str(path))
                    if row[25] & 2:
                        yield path, None, 0
                        yield from walk(row, path, depth+1)
                    else:
                        extent = int.from_bytes(row[2:6], 'little')
                        length = int.from_bytes(row[10:14], 'little')
                        system_use = 33+row[32]+(row[32] % 2 == 0)
                        xa = row[system_use:system_use+14]
                        cdda = len(xa) == 14 and xa[6:8] == b'XA' and int.from_bytes(xa[4:6], 'big') & 0x4000
                        if cdda:
                            sectors = (length+2047)//2048
                            matches = [item for item in self.disc.get('audio', [])
                                       if item['start'] <= extent and extent+sectors <= item['end']]
                            if length % 2048 or extent+sectors > declared_volume or len(matches) != 1:
                                raise ValueError('CDDA extent has no exact bounded CUE audio source: '+str(path))
                            self.audio_files[(extent, length)] = matches[0]
                        elif extent+(length+2047)//2048 > volume:
                            raise ValueError('File exceeds ISO volume: '+str(path))
                        yield path, extent, length
        yield from walk(pvd[156:190], Path(), 0)

    def file_is_raw(self, lba, size):
        if self.disc['stride'] not in (2352, 2336):
            return False
        for sector in range((size+2047)//2048):
            block = self.sector(lba+sector, raw=True)
            if self.disc['stride'] == 2352:
                if block[15] == 2 and block[18] & 0x60:
                    return True
            elif block[2] & 0x60:
                return True
        return False

    def chunks(self, lba, size):
        audio = self.audio_files.get((lba, size))
        if audio is not None:
            with audio['source'].open('rb') as stream:
                stream.seek(audio['offset']+(lba-audio['start'])*2352)
                for sector in range(size//2048):
                    block = stream.read(2352)
                    if len(block) != 2352:
                        raise ValueError('Truncated CUE audio extent')
                    yield block
            return
        # Preserve the entire physical stream if any sector requires raw storage
        raw = self.file_is_raw(lba, size)
        for sector in range((size+2047)//2048):
            block = self.sector(lba+sector, raw=raw)
            yield block if raw else block[:min(2048, size-sector*2048)]


def extract(root):
    root = Path(root).resolve()
    discs = discover(root)
    results = []
    destination = root/'bin/DATA'
    inside(root, destination)
    for index, disc in enumerate(discs, 1):
        output = destination/str(index) if len(discs) > 1 else destination
        files = []
        seen = set()
        with disc['source'].open('rb') as stream:
            reader = IsoReader(disc, stream)
            for name, lba, size in reader.entries():
                if name.as_posix().lower() in seen:
                    raise ValueError('Duplicate ISO path: '+str(name))
                seen.add(name.as_posix().lower())
                target = inside(destination, output/name)
                if lba is None:
                    target.mkdir(parents=True, exist_ok=True)
                    continue
                target.parent.mkdir(parents=True, exist_ok=True)
                temporary = None
                try:
                    with tempfile.NamedTemporaryFile(dir=target.parent, prefix='.startup-', delete=False) as staged:
                        temporary = Path(staged.name)
                        sha = hashlib.sha256()
                        count = 0
                        for block in reader.chunks(lba, size):
                            staged.write(block)
                            sha.update(block)
                            count += len(block)
                    if target.exists():
                        existing = hashlib.sha256()
                        with target.open('rb') as previous:
                            for block in iter(lambda: previous.read(1024*1024), b''):
                                existing.update(block)
                        if existing.digest() != sha.digest():
                            raise ValueError('Extraction would replace a different file: '+str(target))
                    else:
                        # Exclusive publication preserves files created during extraction
                        os.link(temporary, target)
                    receipt = dict(path=target.relative_to(root).as_posix(), lba=lba,
                                   iso_bytes=size, bytes=count, sha256=sha.hexdigest())
                    audio = reader.audio_files.get((lba, size))
                    if audio is not None:
                        receipt.update(storage='raw_cdda', track=audio['track'],
                                       source=audio['source'].relative_to(root).as_posix(),
                                       source_offset=audio['offset']+(lba-audio['start'])*2352)
                    files.append(receipt)
                finally:
                    if temporary is not None:
                        temporary.unlink(missing_ok=True)
        results.append(dict(image=disc['descriptor'].relative_to(root).as_posix(),
                            source=disc['source'].relative_to(root).as_posix(), files=files))
    return dict(discs=results, file_count=sum(len(disc['files']) for disc in results))
