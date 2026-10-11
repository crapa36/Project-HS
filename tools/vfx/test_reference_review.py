"""Synthetic-only tests: no reference footage or external downloads."""
import copy
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

import reference_review as review


class ReviewContracts(unittest.TestCase):
    def setUp(self):
        self.inventory = {'files': [{'id': 'source_001', 'sha256': 'a' * 64,
            'status': 'decoded_and_sampled_not_reviewed', 'kind': 'motion', 'duration_seconds': 2.0}]}
        self.review = {'files': [{'id': 'source_001', 'sha256': 'a' * 64, 'reviewed': True,
            'observed': ['Synthetic shape changes position.'], 'implementation_hypotheses': [],
            'coverage': [{'start': 0.0, 'end': 2.0, 'kind': 'detail', 'notes': 'Synthetic complete interval.'}]}]}

    def test_valid_declaration(self):
        self.assertEqual(review.validate_review(self.inventory, self.review), [])

    def test_unreviewed_cannot_pass(self):
        self.review['files'][0]['reviewed'] = False
        self.assertTrue(review.validate_review(self.inventory, self.review))

    def test_missing_source_cannot_pass(self):
        self.assertTrue(review.validate_review(self.inventory, {'files': []}))

    def test_duplicate_source_cannot_pass(self):
        self.review['files'].append(copy.deepcopy(self.review['files'][0]))
        self.assertTrue(review.validate_review(self.inventory, self.review))

    def test_changed_source_cannot_pass(self):
        self.review['files'][0]['sha256'] = 'b' * 64
        self.assertTrue(review.validate_review(self.inventory, self.review))

    def test_decode_error_cannot_pass(self):
        self.inventory['files'][0]['status'] = 'error'
        self.assertTrue(review.validate_review(self.inventory, self.review))

    def test_missing_visual_observations_cannot_pass(self):
        self.review['files'][0]['observed'] = [' ']
        self.assertTrue(review.validate_review(self.inventory, self.review))

    def test_gap_cannot_pass(self):
        self.review['files'][0]['coverage'][0]['start'] = 0.2
        self.assertTrue(review.validate_review(self.inventory, self.review))

    def test_uncovered_tail_cannot_pass(self):
        self.review['files'][0]['coverage'][0]['end'] = 1.9
        self.assertTrue(review.validate_review(self.inventory, self.review))

    def test_still_does_not_need_fake_motion_coverage(self):
        self.inventory['files'][0]['kind'] = 'still'
        self.review['files'][0]['coverage'] = []
        self.assertEqual(review.validate_review(self.inventory, self.review), [])

    def test_output_never_overwrites(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / 'source'
            source.mkdir()
            with self.assertRaises(ValueError):
                review.make_output(source / 'nested', source)
            output = root / 'output'
            output.mkdir()
            with self.assertRaises(FileExistsError):
                review.make_output(output, source)

    def test_parser_preserves_vfr_spacing(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'showinfo.log'
            path.write_text('[Parsed_showinfo_2 @ 0] n: 0 pts: 0 pts_time:0 pos: 0\n'
                '[Parsed_showinfo_2 @ 0] n: 1 pts: 1 pts_time:0.04 pos: 0\n'
                '[Parsed_showinfo_2 @ 0] n: 2 pts: 4 pts_time:0.16 pos: 0\n')
            self.assertEqual(review.parse_timestamps(path), [0.0, 0.04, 0.16])


@unittest.skipUnless(shutil.which('ffmpeg') and shutil.which('ffprobe'), 'FFmpeg/ffprobe missing')
class MediaIntegration(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.sources = self.root / 'sources'
        self.sources.mkdir()
        self.video = self.sources / 'synthetic clip.mp4'
        self.ffmpeg('-f', 'lavfi', '-i', 'testsrc2=size=160x90:rate=30',
                    '-t', '2', '-c:v', 'mpeg4', '-q:v', '3', str(self.video))

    def tearDown(self):
        self.temporary.cleanup()

    def ffmpeg(self, *args):
        result = subprocess.run(['ffmpeg', '-nostdin', '-hide_banner', '-v', 'error',
            '-threads', '1', '-filter_threads', '1', *args], capture_output=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stderr.decode('utf-8', errors='replace'))

    def test_inventory_all_media_and_native_detail(self):
        self.ffmpeg('-i', str(self.video), '-t', '0.8', '-vf', 'fps=10', str(self.sources / 'animated.gif'))
        self.ffmpeg('-i', str(self.video), '-frames:v', '1', str(self.sources / 'still.png'))
        data = review.inventory(self.sources, self.root / 'overview', 0.5, 60, 3)
        self.assertEqual(len(data['files']), 3)
        self.assertTrue(all(x['status'] == 'decoded_and_sampled_not_reviewed' for x in data['files']), data)
        self.assertFalse(data['visual_review_complete'])
        template = json.loads((self.root / 'overview/review.template.json').read_text())
        self.assertTrue(review.validate_review(data, template))
        detail = review.detail(self.video, self.root / 'detail', 0.5, 1.0, 60)
        self.assertEqual(len(detail['frames']), 15)
        self.assertAlmostEqual(detail['frames'][0]['source_seconds'], 0.5, places=5)
        self.assertAlmostEqual(detail['frames'][-1]['source_seconds'], 29 / 30, places=5)
        self.assertFalse(detail['resampled'])

    def test_missing_inventory_file_count(self):
        with self.assertRaises(ValueError):
            review.inventory(self.sources, self.root / 'wrong-count', 1.0, 60, 32)
        self.assertFalse((self.root / 'wrong-count').exists())

    def test_corrupt_source_is_reported(self):
        (self.sources / 'broken.mp4').write_bytes(b'not a media container')
        data = review.inventory(self.sources, self.root / 'corrupt', 1.0, 60, 2)
        self.assertEqual(sum(x['status'] == 'error' for x in data['files']), 1)

    def test_native_vfr_detail_has_no_synthetic_frames(self):
        variable = self.sources / 'variable.mkv'
        self.ffmpeg('-f', 'lavfi', '-i', 'testsrc2=size=160x90:rate=10', '-t', '1',
                    '-vf', "select='eq(mod(n,3),0)'", '-fps_mode', 'passthrough',
                    '-c:v', 'ffv1', str(variable))
        data = review.detail(variable, self.root / 'vfr', 0, 0.9, 60)
        self.assertEqual(len(data['frames']), 3)
        for observed, expected in zip(data['frames'], (0, 0.3, 0.6)):
            self.assertAlmostEqual(observed['source_seconds'], expected, places=5)


if __name__ == '__main__':
    unittest.main()
