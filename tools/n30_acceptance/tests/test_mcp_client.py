import json
from pathlib import Path
import sys
import tempfile
import textwrap
import unittest

from tools.n30_acceptance.mcp_client import McpClient, McpContractError


@unittest.skipUnless(sys.platform == "win32", "Windows MCP subprocess contract")
class McpClientTests(unittest.TestCase):
    def _server(self, root: Path) -> Path:
        path = root / "fake_mcp.py"
        path.write_text(
            textwrap.dedent(
                """
                import json, sys, time
                for line in sys.stdin:
                    request = json.loads(line)
                    if "id" not in request:
                        continue
                    method = request.get("method")
                    if method == "slow":
                        time.sleep(0.25)
                    print(json.dumps({
                        "jsonrpc": "2.0",
                        "id": request["id"],
                        "result": {"echo": method},
                    }), flush=True)
                """
            ),
            encoding="utf-8",
        )
        return path

    def test_round_trip_writes_auditable_ndjson_and_tracks_memory(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            raw = root / "raw.ndjson"
            with McpClient(
                (sys.executable, str(self._server(root))),
                cwd=root,
                raw_log_path=raw,
                stderr_path=root / "stderr.log",
                metrics_path=root / "metrics.ndjson",
                sample_interval_seconds=0.02,
            ) as client:
                response = client.request("initialize", {"value": 1}, timeout_seconds=2)
                self.assertEqual(response["result"]["echo"], "initialize")
                self.assertGreater(client.peak_private_bytes, 0)

            self.assertTrue(client.process.stdin.closed)
            self.assertTrue(client.process.stdout.closed)
            self.assertTrue(client.process.stderr.closed)

            records = [json.loads(line) for line in raw.read_text(encoding="utf-8").splitlines()]
            self.assertEqual([item["direction"] for item in records], ["request", "response"])
            self.assertEqual(records[0]["message"]["id"], records[1]["message"]["id"])

    def test_timeout_is_explicit_and_does_not_return_a_late_response(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with McpClient(
                (sys.executable, str(self._server(root))),
                cwd=root,
                raw_log_path=root / "raw.ndjson",
                stderr_path=root / "stderr.log",
                metrics_path=root / "metrics.ndjson",
                sample_interval_seconds=0.02,
            ) as client:
                with self.assertRaisesRegex(McpContractError, "timed out"):
                    client.request("slow", {}, timeout_seconds=0.05)


if __name__ == "__main__":
    unittest.main()
