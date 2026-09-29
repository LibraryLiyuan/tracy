"""Regression: cancellation before handshake must leave machine-readable state."""
import json
import pathlib
import subprocess
import sys
import tempfile
import time
import unittest

EXE = pathlib.Path(sys.argv.pop(1)).resolve()

class CaptureCliTests(unittest.TestCase):
    def test_status_channel_never_overwrites_existing_trace(self):
        with tempfile.TemporaryDirectory(prefix='capture-status-collision-') as temp:
            trace = pathlib.Path(temp) / 'existing.tracy-stream'
            trace.write_bytes(b'original evidence')
            result = subprocess.run([str(EXE), '--headless', '--status-json', str(trace),
                '-j', str(trace)], capture_output=True, timeout=10)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(trace.read_bytes(), b'original evidence')

    def test_cancel_before_handshake_with_unicode_paths(self):
        with tempfile.TemporaryDirectory(prefix='capture-gui-中文-') as temp:
            root = pathlib.Path(temp)
            status, stop = root / 'status.json', root / '停止.stop'
            with (root / 'capture.log').open('wb') as log:
                process = subprocess.Popen([str(EXE), '--headless', '--status-json', str(status),
                    '-a', '127.0.0.1', '-p', '65530', '-j', str(root / '录制.tracy-stream'),
                    '-x', str(stop)], stdout=log, stderr=log)
                try:
                    deadline = time.monotonic() + 8
                    while process.poll() is None and time.monotonic() < deadline:
                        if status.exists() and json.loads(status.read_text())['state'] != 'starting':
                            break
                        time.sleep(.05)
                    self.assertTrue(status.exists(), 'Capture must expose connection state before handshake')
                    self.assertEqual(json.loads(status.read_text())['state'], 'connecting')
                    stop.write_text('user stop', encoding='utf-8')
                    process.wait(timeout=8)
                    value = json.loads(status.read_text())
                    self.assertEqual(value['state'], 'cancelled')
                    self.assertFalse(value['first_data_received'])
                    self.assertEqual(process.returncode, 0)
                finally:
                    if process.poll() is None:
                        process.kill()
                        process.wait()

if __name__ == '__main__':
    unittest.main()
