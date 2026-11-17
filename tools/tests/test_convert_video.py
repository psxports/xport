import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest import mock

import convert_video


def movie(frames=2):
    output = bytearray()
    for number in range(1, frames+1):
        block = bytearray(2352)
        block[:12] = convert_video.SYNC
        block[15] = 2
        block[16:24] = bytes([1, 1, 0x48, 0])*2
        block[24:28] = b'\x60\x01\x01\x80'
        struct.pack_into('<HHIIHH', block, 28, 0, 1, number, 64, 16, 16)
        output.extend(block)
    return output


def audio_sector(file, channel, coding=1):
    block = bytearray(2352)
    block[:12] = convert_video.SYNC
    block[15] = 2
    block[16:24] = bytes([file, channel, 0x64, coding])*2
    block[24:2328] = bytes([channel + 1])*2304
    return block


def multichannel_movie():
    video = movie(2)
    return (video[:2352] + audio_sector(7, 1) + audio_sector(8, 1)
            + audio_sector(7, 2) + video[2352:])


class ConvertVideoTests(unittest.TestCase):
    def test_reviewed_audio_identity_keeps_video_and_selected_audio_in_place(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/'multi.str'
            original = multichannel_movie()
            path.write_bytes(original)
            with self.assertRaisesRegex(ValueError, 'Multiple XA'):
                convert_video.inspect_str(path)
            selection = dict(file=7, channel=1, source_sha256=convert_video.digest(path))
            info = convert_video.inspect_str(path, include_completions=True, audio_selection=selection)
            self.assertEqual(info['audio_identity'], dict(file=7, channel=1))
            self.assertEqual(info['omitted_audio_sectors'], [2, 3])
            self.assertEqual(info['audio'], [dict(channel=1, first_sector=1, coding=1, sectors=1)])
            self.assertEqual([(r['frame'], r['sector'], r['end_sector']) for r in info['frames']],
                             [(1, 0, 0), (2, 4, 4)])
            prepared = Path(directory)/'prepared.str'
            convert_video.prepare_str(path, prepared, info, selection)
            copy = prepared.read_bytes()
            self.assertEqual(len(copy), len(original))
            self.assertEqual([i for i, (a, b) in enumerate(zip(copy, original)) if a != b],
                             [2*2352+18, 2*2352+22, 3*2352+18, 3*2352+22])
            observed = convert_video.inspect_str(prepared, include_completions=True)
            self.assertEqual(observed['frames'], info['frames'])
            self.assertEqual(observed['audio'], info['audio'])
            self.assertEqual(path.read_bytes(), original)

    def test_audio_selection_rejects_stale_invalid_and_absent_identities(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/'multi.str'
            path.write_bytes(multichannel_movie())
            valid = dict(file=7, channel=1, source_sha256=convert_video.digest(path))
            for change in (dict(file=True), dict(file=256), dict(channel=True), dict(channel=-1),
                           dict(channel=32), dict(source_sha256='0'*64), dict(extra=1)):
                with self.subTest(change=change), self.assertRaisesRegex(ValueError, 'Invalid or stale'):
                    convert_video.inspect_str(path, audio_selection={**valid, **change})
            with self.assertRaisesRegex(ValueError, 'absent'):
                convert_video.inspect_str(path, audio_selection={**valid, 'file': 9})
            with self.assertRaisesRegex(ValueError, 'absent'):
                convert_video.inspect_str(path, sector_count=1, audio_selection=valid)
            path.write_bytes(path.read_bytes()+audio_sector(7, 1))
            with self.assertRaisesRegex(ValueError, 'stale'):
                convert_video.inspect_str(path, audio_selection=valid)

    def test_audio_selection_conversion_binds_policy_and_prepares_temporary_source(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root/'bin/DATA').mkdir(parents=True)
            source = root/'bin/DATA/MULTI.STR'
            original = multichannel_movie()
            source.write_bytes(original)
            selection = dict(file=7, channel=1, source_sha256=convert_video.digest(source))
            config = dict(video=dict(frame_rate=30,
                          audio_selections={'bin/DATA/MULTI.STR': selection}))
            def encode(command, log):
                if '-c:v' in command:
                    temporary = Path(command[command.index('-i')+1])
                    self.assertNotEqual(temporary, source)
                    observed = convert_video.inspect_str(temporary)
                    self.assertEqual(observed['audio'], [dict(channel=1, first_sector=1, coding=1, sectors=1)])
                    self.assertEqual(observed['frames'][-1]['sector'], 4)
                    Path(command[-1]).write_bytes(b'Encoded selected audio fixture')
                return ''
            with mock.patch.object(convert_video, 'run_hidden_desktop', return_value=mock.Mock(stdout='FFmpeg test\n')), \
                    mock.patch.object(convert_video, 'run', side_effect=encode) as encoding, \
                    mock.patch.object(convert_video, 'decoded_frames', return_value=2):
                result = convert_video.convert(root, config, 'test-ffmpeg')
                self.assertEqual(result['clips'][0]['audio_selection'], selection)
                self.assertEqual(result['clips'][0]['original']['omitted_audio_sectors'], [2, 3])
                self.assertEqual(convert_video.convert(root, config, 'test-ffmpeg'), result)
                self.assertEqual(encoding.call_count, 2)
            self.assertEqual(source.read_bytes(), original)

    def test_reviewed_prefix_excludes_rewind_without_rewriting_original(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'rewind.str'
            original = movie(2) + movie(1)
            path.write_bytes(original)
            source_range = dict(sector_count=2, source_sha256=convert_video.digest(path))
            count = convert_video.reviewed_sector_count(path, source_range)
            info = convert_video.inspect_str(path, include_completions=True, sector_count=count)
            self.assertEqual([(r['frame'], r['sector'], r['end_sector']) for r in info['frames']],
                             [(1, 0, 0), (2, 1, 1)])
            self.assertEqual(path.read_bytes(), original)
            with self.assertRaises(ValueError):
                convert_video.inspect_str(path)
            path.write_bytes(original + movie(1))
            with self.assertRaisesRegex(ValueError, 'stale'):
                convert_video.reviewed_sector_count(path, source_range)

    def test_prefix_rejects_partial_frame_and_invalid_extent(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'partial.str'
            first = movie(1)
            struct.pack_into('<H', first, 30, 2)
            last = bytearray(first)
            struct.pack_into('<H', last, 28, 1)
            path.write_bytes(first + last)
            with self.assertRaisesRegex(ValueError, 'complete'):
                convert_video.inspect_str(path, sector_count=1)
            self.assertEqual(len(convert_video.inspect_str(path, sector_count=2)['frames']), 1)
            for count in (True, 0, -1, 3, '2'):
                with self.subTest(count=count), self.assertRaises(ValueError):
                    convert_video.inspect_str(path, sector_count=count)

    def test_raw2048_preserves_payload_frames_and_sector_timing(self):
        sectors = movie()
        raw = b''.join(sectors[i+24:i+2072] for i in range(0, len(sectors), 2352))
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/'silent.str'
            path.write_bytes(raw)
            info = convert_video.inspect_str(path, include_completions=True)
            self.assertEqual(info['layout'], 'raw2048')
            self.assertEqual(info['sector_bytes'], 2048)
            self.assertEqual(info['audio'], [])
            self.assertEqual([(f['frame'], f['end_sector']) for f in info['frames']], [(1, 0), (2, 1)])
            self.assertEqual(convert_video.wrap_video_sector(raw[:2048])[24:2072], raw[:2048])
            path.write_bytes(raw[:-1])
            with self.assertRaises(ValueError):
                convert_video.inspect_str(path)

    def test_scan_preserves_original_frame_and_sector_identities(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/'movie.str'
            path.write_bytes(movie())
            info = convert_video.inspect_str(path)
            self.assertEqual(info['channel'], 1)
            self.assertEqual(info['width'], 16)
            self.assertEqual([(row['frame'],row['sector']) for row in info['frames']],[(1,0),(2,1)])
            path.write_bytes(b'RIFF'+bytes(4)+b'CDXA'+bytes(32)+movie())
            self.assertEqual(convert_video.inspect_str(path)['header_bytes'],44)

    def test_incomplete_or_discontinuous_movie_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/'movie.str'
            for bad in (movie()[:-1], movie()[:2352]+movie()[:2352]):
                path.write_bytes(bad)
                with self.assertRaises(ValueError):
                    convert_video.inspect_str(path)

    def test_completion_sector_includes_interleaved_audio(self):
        first = movie(1)
        struct.pack_into('<H', first, 30, 2)
        last = bytearray(first)
        struct.pack_into('<H', last, 28, 1)
        audio = bytearray(2352)
        audio[:12] = convert_video.SYNC
        audio[15] = 2
        audio[16:24] = bytes([1,1,0x64,1])*2
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/'movie.str'
            path.write_bytes(first+audio+last)
            self.assertNotIn('end_sector',convert_video.inspect_str(path)['frames'][0])
            self.assertEqual(convert_video.inspect_str(path,include_completions=True)['frames'][0]['end_sector'],2)

    def test_data_padding_preserves_video_audio_and_physical_sector_positions(self):
        first = movie(1)
        first[18] = first[22] = 0x42
        struct.pack_into('<H', first, 30, 2)
        last = bytearray(first)
        struct.pack_into('<H', last, 28, 1)
        padding = bytearray(last)
        padding[18] = padding[22] = 0x48
        audio = bytearray(2352)
        audio[:12] = convert_video.SYNC
        audio[15] = 2
        audio[16:24] = bytes([1, 1, 0x64, 1])*2
        second = bytearray(first)
        struct.pack_into('<I', second, 32, 2)
        second_last = bytearray(second)
        struct.pack_into('<H', second_last, 28, 1)
        first_padding = bytearray(first)
        first_padding[18] = first_padding[22] = 0x48
        original = first + first_padding + audio + last + padding + second + second_last
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/'padding.xa'
            path.write_bytes(original)
            info = convert_video.inspect_str(path, include_completions=True)
            self.assertEqual([(f['frame'], f['sector'], f['end_sector']) for f in info['frames']],
                             [(1, 0, 3), (2, 5, 6)])
            self.assertEqual(info['sectors'], 7)
            self.assertEqual(info['padding_sectors'], [1, 4])
            self.assertEqual(info['audio'][0]['first_sector'], 2)
            self.assertEqual(info['audio'][0]['sectors'], 1)
            self.assertEqual(path.read_bytes(), original)
            for offset, value in [(24+32, 1), (17, 2), (18, 0x42)]:
                bad = bytearray(padding)
                bad[offset] = value
                if offset == 17:
                    bad[21] = value
                if offset == 18:
                    bad[22] = value
                path.write_bytes(first + last + bad)
                with self.subTest(offset=offset), self.assertRaises(ValueError):
                    convert_video.inspect_str(path)

    def test_padding_inspection_does_not_authorize_unprepared_ffmpeg_conversion(self):
        first = movie(1)
        first[18] = first[22] = 0x42
        struct.pack_into('<H', first, 30, 2)
        last = bytearray(first)
        struct.pack_into('<H', last, 28, 1)
        padding = bytearray(last)
        padding[18] = padding[22] = 0x48
        original = first + last + padding
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root/'bin/DATA').mkdir(parents=True)
            path = root/'bin/DATA/PADDING.STR'
            path.write_bytes(original)
            with mock.patch.object(convert_video, 'run_hidden_desktop') as decoder:
                result = convert_video.convert(root, {'video': {'frame_rate': 15}})
                decoder.assert_not_called()
            self.assertEqual(result['status'], 'unsupported')
            self.assertIn('preparation adapter', result['unsupported'][0]['reason'])
            self.assertEqual(path.read_bytes(), original)
            self.assertFalse(list((root/'bin/VIDEO').glob('*.mp4')))

    def test_rate_requires_review_and_accepts_rational_override(self):
        with self.assertRaisesRegex(ValueError,'does not declare FPS'):
            convert_video.frame_rate({},'bin/DATA/INTRO.STR')
        self.assertEqual(convert_video.frame_rate({'video':{'frame_rate':15,'frame_rates':{'intro':'30000/1001'}}},'intro'),'30000/1001')
        for value in (True, 0, -1, '1/0', 'NaN', 121):
            with self.subTest(value=value), self.assertRaises(ValueError):
                convert_video.frame_rate({'video':{'frame_rate':value}},'intro')

    def test_no_video_needs_no_encoder(self):
        with tempfile.TemporaryDirectory() as directory:
            self.assertEqual(convert_video.convert(directory,{})['status'],'not_applicable')

    def test_source_allowlist_does_not_prepare_other_movies(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root/'bin/DATA').mkdir(parents=True)
            for name in ('LOGO.STR','DUMMY.STR'):
                (root/'bin/DATA'/name).write_bytes(b'Unknown STR variant')
            result = convert_video.convert(root,{'video':{'sources':['bin\\DATA\\logo.str']}})
            self.assertEqual([row['source'] for row in result['unsupported']],['bin/DATA/LOGO.STR'])
            for sources in (['bin/DATA/MISSING.STR'], ['../LOGO.STR'], ['bin/DATA/LOGO.STR']*2):
                with self.subTest(sources=sources), self.assertRaises(ValueError):
                    convert_video.convert(root,{'video':{'sources':sources}})

    def test_unknown_str_is_reported_and_original_is_retained(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root/'bin/DATA').mkdir(parents=True)
            path = root/'bin/DATA/UNKNOWN.STR'
            path.write_bytes(b'Unknown format')
            result = convert_video.convert(root,{})
            self.assertEqual(result['status'],'unsupported')
            self.assertEqual(len(result['unsupported']),1)
            self.assertEqual(path.read_bytes(),b'Unknown format')
            self.assertFalse(list((root/'bin/VIDEO').glob('*.mp4')))

    def test_verified_publish_retry_and_modified_output_protection(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root/'bin/DATA').mkdir(parents=True)
            source = root/'bin/DATA/INTRO.STR'
            source.write_bytes(movie())
            config = {'video':{'frame_rate':15,'audio_sample_rates':{'bin/DATA/INTRO.STR':44100},
                               'crfs':{'bin/DATA/INTRO.STR':23}}}
            def encode(command, log):
                self.assertEqual(command[command.index('-ar')+1],'44100')
                self.assertEqual(command[command.index('-crf')+1],'23')
                Path(command[-1]).write_bytes(b'Encoded fixture')
                return ''
            version = mock.Mock(stdout='FFmpeg test\n')
            with mock.patch.object(convert_video,'run_hidden_desktop',return_value=version), \
                    mock.patch.object(convert_video,'run',side_effect=encode) as encoding, \
                    mock.patch.object(convert_video,'decoded_frames',return_value=2):
                first = convert_video.convert(root,config,'test-ffmpeg')
                self.assertEqual(first['clips'][0]['encoding']['audio_sample_rate'],44100)
                self.assertEqual(first['clips'][0]['encoding']['crf'],23)
                self.assertEqual(convert_video.convert(root,config,'test-ffmpeg'),first)
                with mock.patch.object(convert_video, 'run_hidden_desktop',
                                       return_value=mock.Mock(stdout='FFmpeg newer host version\n')):
                    reused = convert_video.convert(root, config, 'newer-ffmpeg')
                self.assertEqual(reused, first)
                self.assertEqual(reused['clips'][0]['encoder']['version'], 'FFmpeg test')
                self.assertEqual(encoding.call_count,1)
                self.assertEqual(source.read_bytes(),movie())
                target = root/first['clips'][0]['output']
                target.write_bytes(b'User modification')
                with self.assertRaisesRegex(ValueError,'modified MP4'):
                    convert_video.convert(root,config,'test-ffmpeg')
                self.assertEqual(target.read_bytes(),b'User modification')
            self.assertEqual(json.loads((root/'bin/VIDEO/manifest.json').read_text()),first)

    def test_invalid_crf_never_starts_encoder(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root/'bin/DATA').mkdir(parents=True)
            (root/'bin/DATA/INTRO.STR').write_bytes(movie())
            for crf in (True, -1, 52, '23', 23.5):
                with self.subTest(crf=crf), self.assertRaisesRegex(ValueError, 'video.crfs'):
                    convert_video.convert(root, {'video': {'frame_rate': 15,
                        'crfs': {'bin/DATA/INTRO.STR': crf}}}, 'test-ffmpeg')
            self.assertFalse(list((root/'bin/VIDEO').glob('*.mp4')))

    def test_wrong_decoded_frame_count_never_publishes_mp4(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root/'bin/DATA').mkdir(parents=True)
            (root/'bin/DATA/INTRO.STR').write_bytes(movie())
            with mock.patch.object(convert_video,'run_hidden_desktop',return_value=mock.Mock(stdout='FFmpeg test\n')), \
                    mock.patch.object(convert_video,'run',side_effect=lambda command, log: Path(command[-1]).write_bytes(b'Bad encode')), \
                    mock.patch.object(convert_video,'decoded_frames',return_value=1):
                with self.assertRaisesRegex(ValueError,'frame count differs'):
                    convert_video.convert(root,{'video':{'frame_rate':15}},'test-ffmpeg')
            self.assertFalse(list((root/'bin/VIDEO').glob('*.mp4')))
