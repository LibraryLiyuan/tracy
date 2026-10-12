import importlib.util
from pathlib import Path
import sys
import unittest

SCRIPTS = Path(__file__).resolve().parents[1]/'scripts'
sys.path.insert(0,str(SCRIPTS))
from stream_review import checks_for


def group(name,owner,maximum,self_max=None):
    return {'name':name,'owner_name':owner,'inclusive':{'max_ms':maximum},
            'self':{'max_ms':maximum if self_max is None else self_max}}


class ReviewObligationTests(unittest.TestCase):
    def test_worker_allocation_is_not_hidden_by_main_render_focus(self):
        checks = checks_for(group('CreateCommittedResourceWithTag','Job.Worker 6',48.4),16.667)
        self.assertIn('inspect_worker_and_dependency_endpoint',checks)
        self.assertIn('trace_resource_caller_lifetime_and_existing_reuse',checks)

    def test_short_plugin_flush_survives_long_scope_filter(self):
        checks = checks_for(group('GfxDeviceD3D12.PluginEvent.Flush','Render Thread',0.3),16.667)
        self.assertIn('inspect_exact_plugin_event_id_flags_and_submission',checks)

    def test_wait_requires_dependency_review_not_compute_claim(self):
        checks = checks_for(group('WaitForJobGroupID','Render Thread',50,48),16.667)
        self.assertIn('inspect_all_workers_and_producer_completion_relation',checks)

    def test_idle_worker_not_promoted_to_compute_problem(self):
        self.assertEqual(checks_for(group('Idle','Job.Worker 1',5000),16.667),[])


if __name__ == '__main__':unittest.main()
