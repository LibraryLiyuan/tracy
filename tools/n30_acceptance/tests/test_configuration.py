import json
from pathlib import Path
import tempfile
import unittest


from tools.n30_acceptance.configuration import ConfigurationError, load_local_inputs


class ConfigurationTests(unittest.TestCase):
    def test_loads_existing_inputs_and_keeps_outputs_distinct(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            worktree = root / "repo"
            worktree.mkdir()
            executable = root / "tool.exe"
            executable.write_bytes(b"tool")
            for name in ("short.stream", "short.tracy", "long.stream"):
                (root / name).write_bytes(name.encode())
            existing_session = root / "existing.session"
            existing_session.mkdir()
            config = {
                "worktree": str(worktree),
                "cmake_executable": str(executable),
                "ctest_executable": str(executable),
                "python_executable": str(executable),
                "dependency_cache": str(root / "deps"),
                "short_stream": str(root / "short.stream"),
                "short_trace": str(root / "short.tracy"),
                "source_30m": str(root / "long.stream"),
                "existing_session_30m": str(existing_session),
                "fresh_session_30m": str(root / "fresh.session"),
                "scratch_root": str(root / "scratch"),
                "build_capture": str(root / "build-capture"),
                "build_query": str(root / "build-query"),
            }
            path = root / "inputs.json"
            path.write_text(json.dumps(config), encoding="utf-8")
            loaded = load_local_inputs(path)
            self.assertEqual(loaded.worktree, worktree.resolve())
            self.assertEqual(loaded.fresh_session_30m, (root / "fresh.session").resolve())

    def test_rejects_output_aliasing_formal_source_or_existing_session(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            worktree = root / "repo"
            worktree.mkdir()
            tool = root / "tool.exe"
            tool.write_bytes(b"tool")
            short_stream = root / "short.stream"
            short_trace = root / "short.tracy"
            long_stream = root / "long.stream"
            for path in (short_stream, short_trace, long_stream):
                path.write_bytes(b"x")
            existing = root / "existing.session"
            existing.mkdir()
            config = {
                "worktree": str(worktree),
                "cmake_executable": str(tool),
                "ctest_executable": str(tool),
                "python_executable": str(tool),
                "dependency_cache": str(root / "deps"),
                "short_stream": str(short_stream),
                "short_trace": str(short_trace),
                "source_30m": str(long_stream),
                "existing_session_30m": str(existing),
                "fresh_session_30m": str(existing),
                "scratch_root": str(root / "scratch"),
                "build_capture": str(root / "build-capture"),
                "build_query": str(root / "build-query"),
            }
            path = root / "inputs.json"
            path.write_text(json.dumps(config), encoding="utf-8")
            with self.assertRaises(ConfigurationError):
                load_local_inputs(path)


if __name__ == "__main__":
    unittest.main()
