"""Query Job signatures are opaque identities, not filesystem-safe labels."""
import json
import sys
import tempfile
import unittest
from pathlib import Path

from tests.test_report_builder import valid_inputs

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from report_common import stable_id, validate_analysis_and_evidence


class QuerySignatureIdentityTests(unittest.TestCase):
    def errors_for(self, signature, *, query_native=True):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            ap, ep = valid_inputs(root)
            analysis = json.loads(ap.read_text(encoding='utf-8'))
            evidence = json.loads(ep.read_text(encoding='utf-8'))
            if query_native:
                analysis['query_scan'] = {}
                analysis['report_identity']['configuration'] = {'analysis_profile': {'path': 'p.yaml', 'sha256': '1' * 64}}
            analysis['bottleneck_signatures'][0]['id'] = signature
            analysis['evidence_cards'][0]['signature_id'] = signature
            errors, _, _ = validate_analysis_and_evidence(analysis, evidence, root)
            return [e for e in errors if '.id is invalid' in e]

    def test_actual_query_burst_identity_is_preserved(self):
        self.assertEqual(self.errors_for('job:type:5:StreamingAssetLoaderV2:CalcDistanceJob (Burst)'), [])

    def test_generic_unicode_job_identity_is_not_rewritten(self):
        self.assertEqual(self.errors_for('job:type:17:加载器/任务<T> (Burst)'), [])

    def test_other_unsafe_ids_and_control_characters_stay_rejected(self):
        for value in ('not a Query signature', 'job:type:x:Job', 'job:type:5:',
                      'job:type:5:Job\nInjected', 'job:type:5:Job\x00', 'job:type:5:' + 'x' * 4096):
            with self.subTest(value=value[:50]):
                self.assertTrue(self.errors_for(value))

    def test_legacy_id_contract_is_not_broadened(self):
        self.assertTrue(self.errors_for('job:type:5:A Job (Burst)', query_native=False))

    def test_anchor_identity_does_not_collapse_distinct_job_names(self):
        names = ['job:type:5:A/B', 'job:type:5:A B', 'job:type:5:A-B',
                 'job:type:5:加载', 'job:type:5:另一任务', 'job:type:5:Foo', 'job:type:5:foo']
        encoded = [stable_id(value).casefold() for value in names]
        self.assertEqual(len(set(encoded)), len(names))
        for anchor in encoded:
            self.assertNotIn('/', anchor)
            self.assertNotIn(' ', anchor)

    def test_ordinary_existing_safe_anchor_is_stable(self):
        self.assertEqual(stable_id('SIG-0001'), 'SIG-0001')
        self.assertEqual(stable_id('cpu-exact:abcdef'), 'cpu-exact:abcdef')


if __name__ == '__main__':
    unittest.main()
