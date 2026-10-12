"""A fully saved cursor chain is evidence; a prefix or edited chain is not."""
import copy
import json
import tempfile
import unittest
from pathlib import Path

from tests import test_investigation_receipts as receipt_tests
load, store = receipt_tests.load, receipt_tests.store


class PaginationChainTests(unittest.TestCase):
    def test_report_schema_exposes_the_checked_page_chain(self):
        schema = json.loads((Path(__file__).resolve().parents[1] / 'schemas/analysis-result.schema.json').read_text(encoding='utf-8'))
        properties = schema['$defs']['investigationReceipt']['properties']
        self.assertIn('page_evidence_ids', properties)
        self.assertEqual(properties['page_evidence_ids']['maxItems'], 128)

    def fixture(self, root, nested=False):
        c, f, e, q1, r1 = receipt_tests.ReceiptTests().fixture(root)
        q2, r2 = copy.deepcopy(q1), copy.deepcopy(r1)
        q2['id'] = r2['id'] = 2
        q2['params']['arguments']['params']['cursor'] = 'real-cursor-1'
        for reply, cursor in ((r1, 'real-cursor-1'), (r2, None)):
            content = reply['result']['structuredContent']
            content.update(partial=False, budget={'exhausted_by': []})
            container = content['data'] if nested else content
            container['page'] = {'next_cursor': cursor, 'has_more': bool(cursor),
                                 'partial': False, 'truncated': False}
        e['evidence'][0].update(request=store(root, 'q1.json', q1), response=store(root, 'r1.json', r1))
        e['evidence'].append({'evidence_id': 'E2', 'request': store(root, 'q2.json', q2),
                              'response': store(root, 'r2.json', r2)})
        f['investigation_receipts'][0]['page_evidence_ids'] = ['E1', 'E2']
        return c, f, e, q1, r1, q2, r2

    def test_complete_chain_retains_raw_first_page_and_passes(self):
        for nested in (False, True):
            with self.subTest(nested=nested), tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                c, f, e, *_ = self.fixture(root, nested)
                self.assertEqual([], load().validate_receipts(c, f, e, root))

    def test_missing_or_different_pages_do_not_close(self):
        for mutation in ('missing_tail', 'wrong_cursor', 'wrong_params', 'wrong_method',
                         'duplicate_id', 'partial', 'exhausted', 'fingerprint',
                         'cycle', 'tail_has_more', 'middle_without_cursor', 'starts_at_tail',
                         'revision_changed', 'checksum', 'tail_no_end_marker', 'root_changed'):
            with self.subTest(mutation=mutation), tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                c, f, e, q1, r1, q2, r2 = self.fixture(root)
                receipt = f['investigation_receipts'][0]
                content = r2['result']['structuredContent']
                if mutation == 'missing_tail': receipt['page_evidence_ids'] = ['E1']
                elif mutation == 'wrong_cursor': q2['params']['arguments']['params']['cursor'] = 'invented'
                elif mutation == 'wrong_params': q2['params']['arguments']['params']['frame_index'] = 41
                elif mutation == 'wrong_method': q2['params']['arguments']['method'] = 'zone.cpu.search'
                elif mutation == 'duplicate_id': q2['id'] = r2['id'] = 1
                elif mutation == 'partial': content['partial'] = True
                elif mutation == 'exhausted': content['budget']['exhausted_by'] = ['max_cpu_ms']
                elif mutation == 'fingerprint': content['trace']['fingerprint'] = 'b' * 64
                elif mutation == 'cycle': content['page']['next_cursor'] = 'real-cursor-1'
                elif mutation == 'tail_has_more': content['page']['has_more'] = True
                elif mutation == 'middle_without_cursor':
                    r1['result']['structuredContent']['page']['next_cursor'] = None
                elif mutation == 'starts_at_tail':
                    receipt.update(evidence_id='E2', page_evidence_ids=['E2'])
                elif mutation == 'revision_changed': content['trace']['revision'] = 'new'
                elif mutation == 'checksum': e['evidence'][1]['response']['sha256'] = '0' * 64
                elif mutation == 'tail_no_end_marker': content['page'] = {'returned': 7, 'partial': False}
                elif mutation == 'root_changed': content['data']['root'] = {'ref': 'other-root'}
                e['evidence'][0]['response'] = store(root, 'r1.json', r1)
                e['evidence'][1]['request'] = store(root, 'q2.json', q2)
                if mutation != 'checksum':
                    e['evidence'][1]['response'] = store(root, 'r2.json', r2)
                self.assertTrue(load().validate_receipts(c, f, e, root))


if __name__ == '__main__':
    unittest.main()
