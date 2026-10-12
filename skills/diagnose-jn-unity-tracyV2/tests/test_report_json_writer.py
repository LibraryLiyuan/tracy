"""Canonical report JSON must publish atomically without whole-output copies."""
import json
from pathlib import Path
import sys
import tempfile
import tracemalloc
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from report_common import write_json


class ReportJsonWriterTests(unittest.TestCase):
    def test_canonical_unicode_escaping_sorting_and_final_lf(self):
        value = {'z': '中文🙂\n', 'a': [True, None, 1.5], 'nested': {'b': '\\', 'a': '"'}}
        expected = r'''{
  "a": [
    true,
    null,
    1.5
  ],
  "nested": {
    "a": "\"",
    "b": "\\"
  },
  "z": "中文🙂\n"
}''' + '\n'
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'value.json'
            write_json(path, value)
            self.assertEqual(path.read_bytes(), expected.encode('utf-8'))
            self.assertEqual({p.name for p in path.parent.iterdir()}, {'value.json'})

    def test_many_small_records_do_not_retain_output_sized_copies(self):
        # A whole dumps/UTF-8 materialization fails this allocation bound.
        # This is a synthetic orchestration fixture, never Trace statistics.
        value = {'label': '记录', 'rows': [
            {'index': i, 'ref': f'candidate:{i:064x}', 'text': 'short 中文 field'}
            for i in range(15000)
        ]}
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'value.json'
            tracemalloc.start()
            try:
                write_json(path, value)
                _, peak = tracemalloc.get_traced_memory()
            finally:
                tracemalloc.stop()
            self.assertGreater(path.stat().st_size, 2 * 1024 * 1024)
            self.assertLess(peak, 1024 * 1024, f'writer temporary allocation {peak} bytes')
            self.assertEqual(json.loads(path.read_text(encoding='utf-8')), value)

    def test_serialization_failure_leaves_existing_file_and_no_staging_file(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'value.json'
            path.write_bytes(b'old verified artifact\n')
            with self.assertRaises(TypeError):
                write_json(path, {'first': 1, 'late_invalid': object()})
            self.assertEqual(path.read_bytes(), b'old verified artifact\n')
            self.assertEqual({p.name for p in path.parent.iterdir()}, {'value.json'})

    def test_publication_failure_preserves_existing_file_and_cleans_staging(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'value.json'
            path.write_bytes(b'old verified artifact\n')
            # Real serialization and staging remain enabled. Only the final
            # filesystem publication is made to fail, as on a locked target.
            with mock.patch('os.replace', side_effect=OSError('publication denied')):
                with self.assertRaisesRegex(OSError, 'publication denied'):
                    write_json(path, {'new': 'content'})
            self.assertEqual(path.read_bytes(), b'old verified artifact\n')
            self.assertEqual({p.name for p in path.parent.iterdir()}, {'value.json'})


if __name__ == '__main__':
    unittest.main()
