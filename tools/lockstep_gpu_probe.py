"""Read bounded GPU pixels without copying guest RAM into Python"""
import mmap
import shutil
import struct
from pathlib import Path

from lockstep_contract import mapping_name


def pixel_coordinates(rows):
    if not isinstance(rows, list) or not 1 <= len(rows) <= 128:
        raise ValueError("GPU probe requires 1..128 pixels")
    for row in rows:
        if (not isinstance(row, list) or len(row) != 2 or
                any(type(value) is not int for value in row) or
                not 0 <= row[0] < 1024 or not 0 <= row[1] < 512):
            raise ValueError("GPU probe pixel is outside VRAM")
    return rows


def original_pixels(path, coordinates):
    coordinates = pixel_coordinates(coordinates)
    marker = struct.pack('<I', 8) + b'GPU-VRAM'
    with Path(path).open('rb') as stream, mmap.mmap(
            stream.fileno(), 0, access=mmap.ACCESS_READ) as view:
        begin = view.find(marker)
        if begin < 0 or view.find(marker, begin + 1) >= 0:
            raise ValueError("GPU VRAM marker is missing or ambiguous")
        begin += len(marker)
        if begin + 1024 * 512 * 2 > len(view):
            raise ValueError("GPU VRAM section is truncated")
        return [struct.unpack_from('<H', view, begin + (y * 1024 + x) * 2)[0]
                for x, y in coordinates]


def native_pixels(run_id, coordinates):
    coordinates = pixel_coordinates(coordinates)
    with mmap.mmap(-1, 1024 * 512 * 2, tagname=mapping_name(run_id, 'vram'),
                   access=mmap.ACCESS_READ) as view:
        return [struct.unpack_from('<H', view, (y * 1024 + x) * 2)[0]
                for x, y in coordinates]


def original_metadata(path):
    marker = struct.pack('<I', 3) + b'GPU'
    with Path(path).open('rb') as stream, mmap.mmap(
            stream.fileno(), 0, access=mmap.ACCESS_READ) as view:
        begin = view.find(marker)
        if begin < 0 or view.find(marker, begin + 1) >= 0:
            raise ValueError("GPU state marker is missing or ambiguous")
        begin += len(marker)
        if begin + 649 > len(view):
            raise ValueError("GPU palette state is truncated")
        status, draw_mode, palette, window = struct.unpack_from('<IHHI', view, begin)
        cache_tag, wide = struct.unpack_from('<IB', view, begin + 132)
        if wide not in (0, 1):
            raise ValueError("GPU palette width is invalid")
        return dict(status=status, draw_mode=draw_mode, palette=palette,
                    texture_window=window, clut_tag=cache_tag, clut_8bit=bool(wide),
                    cached_clut=list(struct.unpack_from('<256H' if wide else '<16H',
                                                       view, begin + 137)))


def checkpoint_pixels(adapter, run_id, raw_path, frame, coordinates):
    coordinates = pixel_coordinates(coordinates)
    adapter.export_checkpoint(frame)
    original = original_pixels(raw_path, coordinates)
    native = native_pixels(run_id, coordinates)
    return [dict(x=x, y=y, original=left, native=right)
            for (x, y), left, right in zip(coordinates, original, native)]


def capture_pixels(adapter, run_id, raw_path, frame, coordinates, metadata=None):
    coordinates = pixel_coordinates(coordinates)
    native = native_pixels(run_id, coordinates)
    raw_path = Path(raw_path)
    backup = raw_path.with_suffix('.gpu-probe-origin')
    adapter.export_checkpoint(frame)
    shutil.copyfile(raw_path, backup)
    try:
        adapter.capture_checkpoint(frame)
        adapter.export_checkpoint(frame)
        original = original_pixels(raw_path, coordinates)
        if metadata is not None:
            metadata.update(original_metadata(raw_path))
    finally:
        shutil.copyfile(backup, raw_path)
        reply = adapter.remote.packet(f'qXportLockstepCheckpointImport:{frame:x}')
        if reply != f'OK:{frame:x}':
            raise RuntimeError(f'GPU probe checkpoint restore failed: {reply}')
        backup.unlink()
    return [dict(x=x, y=y, original=left, native=right)
            for (x, y), left, right in zip(coordinates, original, native)]
