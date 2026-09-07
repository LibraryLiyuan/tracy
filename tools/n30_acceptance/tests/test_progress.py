import json
from pathlib import Path
import tempfile
import unittest

from tools.n30_acceptance.progress import AcceptanceProgress


class ProgressTests(unittest.TestCase):
    def test_heartbeat_persists_phase_and_terminal_state(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with AcceptanceProgress(root / "run", root, interval_seconds=0.02) as progress:
                progress.update(phase="P4", command="convert", status="RUNNING")
                progress.write_now()
                current = json.loads((root / "acceptance-progress.json").read_text())
                self.assertEqual(current["phase"], "P4")
                self.assertEqual(current["command"], "convert")
                self.assertGreaterEqual(current["elapsed_ns"], 0)
                progress.update(status="INCOMPLETE", last_error="test interruption")
            current = json.loads((root / "acceptance-progress.json").read_text())
            self.assertEqual(current["status"], "INCOMPLETE")
            self.assertFalse(progress.is_alive)
            self.assertTrue((root / "run" / "progress.ndjson").is_file())
