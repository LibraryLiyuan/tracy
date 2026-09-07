import json
from pathlib import Path
import tempfile
import unittest


from tools.n30_acceptance.command_executor import CommandExecutor
from tools.n30_acceptance.process_runner import ConsoleBreakPlan


class _Result:
    pid = 1234
    timed_out = False
    no_progress_timed_out = False
    peak_working_set_bytes = 30
    read_bytes = 40
    write_bytes = 50
    sample_count = 1
    exit_code = 0
    elapsed_ns = 10
    peak_private_bytes = 20
    console_break_count = 1


class _Runner:
    def __init__(self):
        self.calls = []

    def run(self, command, **kwargs):
        self.calls.append((tuple(command), kwargs))
        return _Result()


class _TextRunner(_Runner):
    def run(self, command, **kwargs):
        result = super().run(command, **kwargs)
        kwargs["stdout_path"].write_text("stdout fixture\n", encoding="utf-8")
        kwargs["stderr_path"].write_text("stderr fixture\n", encoding="utf-8")
        return result


class CommandExecutorTests(unittest.TestCase):
    def test_console_break_plan_is_forwarded_and_result_is_audited(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            runner = _Runner()
            executor = CommandExecutor(root, runner=runner)
            plan = ConsoleBreakPlan("[Canonical]")
            executor.run("cancel", ["converter"], cwd=root, console_break_plan=plan)
            self.assertIs(runner.calls[0][1]["console_break_plan"], plan)
            record = json.loads((root / "commands.ndjson").read_text())
            self.assertEqual(record["console_break_count"], 1)

    def test_every_command_is_numbered_logged_and_gets_isolated_artifacts(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            runner = _Runner()
            executor = CommandExecutor(root, runner=runner)
            executor.run("P1-self-test", ["python", "-m", "unittest"], cwd=root)
            executor.run("P2-build", ["cmake", "--build", "x"], cwd=root)

            commands = [
                json.loads(line)
                for line in (root / "commands.ndjson").read_text().splitlines()
            ]
            self.assertEqual([item["sequence"] for item in commands], [1, 2])
            self.assertEqual(commands[0]["label"], "P1-self-test")
            first_call = runner.calls[0][1]
            self.assertEqual(
                first_call["stdout_path"], root / "raw" / "001-P1-self-test" / "stdout.log"
            )

    def test_run_text_returns_the_exact_audited_stdout_and_stderr(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            executor = CommandExecutor(root, runner=_TextRunner())
            result, stdout, stderr = executor.run_text(
                "identity", ["tool", "--identity"], cwd=root
            )
            self.assertEqual(result.exit_code, 0)
            self.assertEqual(stdout, "stdout fixture\n")
            self.assertEqual(stderr, "stderr fixture\n")
            self.assertEqual(
                (root / "raw" / "001-identity" / "stdout.log").read_text(encoding="utf-8"),
                stdout,
            )

    def test_reopening_command_log_continues_sequence(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            first = CommandExecutor(root, runner=_Runner())
            first.run("one", ["tool", "one"], cwd=root)
            second = CommandExecutor(root, runner=_Runner())
            second.run("two", ["tool", "two"], cwd=root)
            records = [
                json.loads(line)
                for line in (root / "commands.ndjson").read_text().splitlines()
            ]
            self.assertEqual([item["sequence"] for item in records], [1, 2])
            self.assertTrue((root / "raw" / "002-two").is_dir())


if __name__ == "__main__":
    unittest.main()
