"""Candidate bookkeeping only: no Trace parsing or performance statistics."""
import copy
import gc
import json
import tempfile
import tracemalloc
import weakref
import unittest
from pathlib import Path

from tests.test_skill_v2_contract import load_state_tool, candidate, STATE_TEMPLATE


class Page(dict):
    pass


class CandidatePageIngestTests(unittest.TestCase):
    def test_state_publish_streams_encoding_with_identical_bytes(self):
        tool = load_state_tool()
        value = {"queue": [{"id": str(i), "note": "候选" + "x" * 128} for i in range(15000)]}
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "state.json"
            tracemalloc.start()
            try:
                tool._atomic_write(path, value)
                _, peak = tracemalloc.get_traced_memory()
            finally:
                tracemalloc.stop()
            self.assertLess(peak, 1024 * 1024, "state encoding retained a whole large JSON/string copy")
            self.assertEqual(path.read_bytes(), tool._stable_json(value))
            self.assertEqual(json.loads(path.read_bytes()), value)

    def test_consumes_pages_without_retaining_full_candidate_payloads(self):
        tool = load_state_tool()
        state = tool.new_state(STATE_TEMPLATE)
        state["scan"]["policy_algorithm"] = "candidate-policy-v3"
        refs = []
        page_count = 24

        def pages():
            for index in range(page_count):
                gc.collect()
                # At most the previous page is allowed to remain in the caller.
                self.assertLessEqual(sum(ref() is not None for ref in refs), 1)
                row = candidate(f"candidate-{index:04}", "P2", ["top"], selected=index % 3 == 0)
                row["wide_unused_evidence"] = "x" * 131072
                page = Page(items=[row], page={"cursor": str(index), "total": str(page_count),
                    "done": index == page_count - 1,
                    "next_cursor": None if index == page_count - 1 else str(index + 1)})
                refs.append(weakref.ref(page))
                yield page
                del page, row

        tool.ingest_candidate_pages(state, pages())
        self.assertEqual(state["investigations"]["coverage"]["candidate_total"], page_count)
        self.assertEqual(state["investigations"]["coverage"]["required_total"], 8)
        self.assertEqual(len(state["investigations"]["backlog"]), page_count)
        self.assertEqual(len(state["investigations"]["queue"]), 8)
        self.assertNotIn("wide_unused_evidence", str(state))

    def test_cursor_gap_and_early_done_do_not_publish_state(self):
        for mutation in ("gap", "early_done", "nonadvancing", "partial"):
            with self.subTest(mutation=mutation):
                tool = load_state_tool()
                state = tool.new_state(STATE_TEMPLATE)
                state["scan"]["policy_algorithm"] = "candidate-policy-v3"
                before = copy.deepcopy(state)
                pages = [
                    {"items": [candidate("a", "P2", ["top"])],
                     "page": {"cursor": "0", "next_cursor": "1", "done": False, "total": "2"}},
                    {"items": [candidate("b", "P2", ["top"])],
                     "page": {"cursor": "1", "next_cursor": None, "done": True, "total": "2"}},
                ]
                if mutation == "gap":
                    pages[1]["page"]["cursor"] = "2"
                elif mutation == "early_done":
                    pages[0]["page"].update(done=True, next_cursor=None)
                elif mutation == "nonadvancing":
                    pages[0]["page"]["next_cursor"] = "0"
                else:
                    pages[0]["page"]["partial"] = True
                with self.assertRaises(tool.AnalysisStateError):
                    tool.ingest_candidate_pages(state, iter(pages))
                self.assertEqual(state, before)

    def test_late_duplicate_does_not_partially_replace_investigation_queue(self):
        tool = load_state_tool()
        state = tool.new_state(STATE_TEMPLATE)
        state["scan"]["policy_algorithm"] = "candidate-policy-v3"
        before = copy.deepcopy(state)
        pages = ({"items": [candidate("same", "P2", ["top"])],
                  "page": {"total": "2", "cursor": str(i), "done": i == 1,
                           "next_cursor": None if i == 1 else "1"}} for i in range(2))
        with self.assertRaisesRegex(tool.AnalysisStateError, "duplicate"):
            tool.ingest_candidate_pages(state, pages)
        self.assertEqual(state, before)


if __name__ == "__main__":
    unittest.main()
