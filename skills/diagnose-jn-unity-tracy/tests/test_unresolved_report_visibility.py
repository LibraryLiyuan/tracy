"""Readers must see the evidence boundary, not confuse completion with a root cause."""
import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path

from tests.test_query_native_report_contract import query_native_analysis, load_contract, SKILL_ROOT


class UnresolvedReportVisibilityTests(unittest.TestCase):
    def fixture(self, root):
        _, _, _, analysis, _ = query_native_analysis(root)
        finding = analysis['candidate_findings'][0]
        finding.update(conclusion_status='Unresolved', investigation_receipts=[{
            'evidence_id': 'SEARCH-EMPTY', 'purpose': 'zone_tree', 'manifestation': '',
            'outcome': 'verified_not_recorded', 'frame_anchor_evidence_id': 'FRAME-42'}])
        analysis['evidence_gaps'] = [{
            'candidate_id': finding['candidate_id'], 'reason': 'CandidateNotRecordedInRepresentativeFrame',
            'evidence_refs': ['SEARCH-EMPTY', 'FRAME-42'], 'blocked_by': 'Exact event missing at reference frame 42.',
            'minimum_additional_evidence': ['Confirm invocation with matching instrumentation.']}]
        return analysis

    def test_candidate_trail_shows_limited_outcome_and_frame_receipt(self):
        with tempfile.TemporaryDirectory() as temp:
            analysis = self.fixture(Path(temp))
            text = load_contract().render_analysis_process(analysis)
            self.assertIn('verified_not_recorded', text)
            self.assertIn('SEARCH-EMPTY', text)
            self.assertIn('FRAME-42', text)
            self.assertIn('不代表根因已确认', text)

    def test_main_and_gap_specialty_keep_candidate_blocker_and_evidence(self):
        with tempfile.TemporaryDirectory() as temp:
            analysis = self.fixture(Path(temp))
            specialty = load_contract().render_specialty_reports(analysis, {})[
                '09-Investigation-Backlog-and-Evidence-Gaps.md']
            sys.path.insert(0, str(SKILL_ROOT / 'scripts'))
            spec = importlib.util.spec_from_file_location('visibility_builder', SKILL_ROOT / 'scripts/build_report.py')
            builder = importlib.util.module_from_spec(spec); spec.loader.exec_module(builder)
            main = builder.render_main_report(analysis, {}, {}, {}, False)
            for rendered in (specialty, main):
                gap_section = rendered.split('Evidence Gaps', 1)[1]
                self.assertIn('candidate-tsr', gap_section)
                self.assertIn('Exact event missing at reference frame 42.', gap_section)
                self.assertIn('SEARCH-EMPTY', gap_section)
                self.assertIn('FRAME-42', gap_section)
                self.assertIn('Confirm invocation with matching instrumentation.', gap_section)


if __name__ == '__main__':
    unittest.main()
