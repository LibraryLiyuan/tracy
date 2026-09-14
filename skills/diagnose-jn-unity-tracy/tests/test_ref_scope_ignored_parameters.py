"""Ref-based trees must prove time coverage from returned facts, not ignored args."""
import tempfile
import unittest
from pathlib import Path

from tests import test_unresolved_completion_receipts as waits
from tests import test_investigation_receipts as fixtures


class RefIgnoredScopeTests(unittest.TestCase):
    def test_ignored_cpu_time_range_cannot_cover_a_frame_outside_the_real_zone(self):
        with tempfile.TemporaryDirectory() as temp:
            case = waits.CompletionNotRecordedReceiptTests(); values = case.fixture()
            node = values['replies']['TREE42']['result']['structuredContent']['data']['root']
            node.update(start_ns='500', end_ns='600')
            values['replies']['PARENT']['result']['structuredContent']['data'].update(start_ns='0', end_ns='1000')
            values['requests']['TREE42']['params']['arguments']['params'].update(start_ns='100', end_ns='200')
            self.assertTrue(case.validate(Path(temp), values))

    def test_ignored_gpu_time_range_cannot_invent_representative_segment_coverage(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            c, f, e, req, reply = fixtures.ReceiptTests().fixture(root)
            c.update(domain='gpu', thread_or_queue='queue:direct', observation_unit='l0_segment',
                     representative_frames=[], representative_intervals=[
                         {'begin_ns':'100', 'end_ns':'200', 'event_refs':['gpu:l0']}])
            req['params']['arguments'].update(method='zone.gpu.tree', params={
                'ref':'gpu:work', 'start_ns':'100', 'end_ns':'200'})
            reply['result']['structuredContent']['data']['nodes'] = [{
                'ref':'gpu:work', 'name':'Work', 'path':'Parent > Work', 'context_ref':'queue:direct',
                'gpu_start_ns':'300', 'gpu_end_ns':'400', 'complete':True}]
            e['evidence'][0].update(request=fixtures.store(root,'request.json',req),
                                   response=fixtures.store(root,'response.json',reply))
            self.assertTrue(fixtures.load().validate_receipts(c,f,e,root))


if __name__ == '__main__':
    unittest.main()
