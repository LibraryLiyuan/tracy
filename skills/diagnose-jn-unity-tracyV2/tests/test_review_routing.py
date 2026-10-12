import copy
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from review_routing import priority, make_routing, validate_routing, deep_required
from tests.test_deep_workflow import DeepWorkflowTests
from tests.review_fixtures import save


class RoutingTests(unittest.TestCase):
    def fixture(self, root):
        helper = DeepWorkflowTests()
        a, c, e = helper.fixture(root)
        a['report_versions'].update(analysis_workflow='2.2.0', report_builder='2.2.0', template='2.2.0')
        candidate = c['candidates'][0]
        candidate['priority'] = 'P2'
        a['candidate_findings'][0]['priority'] = 'P2'
        a['performance_review'].update(issues=[], events=[], sources=[], dedup_review=[], candidate_splits=[])
        r = make_routing(c)
        row = r['records'][0]
        row.update(name='Work', screening_reason='Visible cost warrants tester review')
        from review_routing import FIELDS, METRICS
        review = {key: 'Observed work; causal impact is not established' for key in FIELDS}
        review.update(impact='unknown', impact_evidence='not_established', metrics={key: {'status': 'unavailable', 'reason': 'Not returned by this query'} for key in METRICS})
        review['metrics']['peak_cost'] = {'status': 'available', 'value': '17000000', 'unit': 'ns',
            'scope': 'Whole event duration, not frame intersection', 'evidence_id': 'E-TREE', 'pointer': '/data/root/duration_ns'}
        row['review'] = review
        a['review_routing'] = r
        a['process_issues'] = []
        return a, c, e

    def test_final_numbering_and_window_rules(self):
        for old,new in [('P1','P0'),('P2','P1'),('P3','P2'),('P4','P3')]:
            c={'priority':old,'manifestation':'frame_cost'}
            self.assertEqual(new,priority(c)); self.assertEqual(old,c['priority'])
        for pattern in ['stable_slow:1:30','sustained_pressure:1:30']:
            self.assertEqual('P1',priority({'priority':'P2','manifestation':pattern}))
        self.assertEqual('P0',priority({'priority':'P2','triggers':['user_focus']}))
        self.assertIsNone(priority({'priority':'P0'}))

    def test_unchosen_p2_can_complete_review_without_source(self):
        from performance_review import validate_review
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); a,c,e = self.fixture(root)
            errors, model = validate_review(a,c,e,root)
            self.assertEqual([], errors)
            self.assertEqual(0, model['counts']['required_pending'])
            self.assertEqual(1, model['routing_counts']['pending'])
            from performance_report import render_documents
            doc = render_documents(a,model)['P1-Source-Review-Candidates.md']
            self.assertIn('17000000', doc); self.assertIn('待测试人员选择', doc)

    def test_p1_cannot_be_filtered_or_deferred(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);a,c,e=self.fixture(root)
            c['candidates'][0]['triggers']=['user_focus']
            row=a['review_routing']['records'][0];row['analysis_priority']='P0'
            with self.assertRaisesRegex(ValueError,'mandatory candidate cannot'):
                validate_routing(a,c,e,root)

    def test_missing_or_duplicate_route_fails(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);a,c,e=self.fixture(root)
            a['review_routing']['records'] *= 2
            with self.assertRaises(ValueError):validate_routing(a,c,e,root)
            a['review_routing']['records']=[]
            with self.assertRaises(ValueError):validate_routing(a,c,e,root)

    def test_changed_measurement_and_stale_manifest_fail(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);a,c,e=self.fixture(root)
            a['review_routing']['records'][0]['review']['metrics']['peak_cost']['value']='1'
            with self.assertRaisesRegex(ValueError,'differs'):validate_routing(a,c,e,root)
            a['review_routing']['candidate_content_sha256']='stale'
            with self.assertRaisesRegex(ValueError,'identity mismatch'):validate_routing(a,c,e,root)

    def test_approval_requires_bound_tester_record_and_source(self):
        from performance_review import validate_review
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);a,c,e=self.fixture(root)
            row=a['review_routing']['records'][0];row['decision']='approved'
            with self.assertRaises((KeyError,ValueError)):validate_routing(a,c,e,root)
            proof={'candidate_id':row['candidate_id'],'candidate_content_sha256':c['content_sha256'],
                'decision':'approved','origin':'tester_record','actor':'Tester','at':'2026-09-14T12:00:00+08:00',
                'reason':'Investigate this cost','source_reference':'Local tester selection'}
            (root/'tester-decisions').mkdir()
            save(root/'tester-decisions/choice.json',proof)
            row['decision_record']={'path':'tester-decisions/choice.json','sha256':hashlib.sha256((root/'tester-decisions/choice.json').read_bytes()).hexdigest()}
            routes,_=validate_routing(a,c,e,root)
            self.assertTrue(deep_required(c['candidates'][0],routes))
            errors,_=validate_review(a,c,e,root)
            self.assertTrue(errors, 'Approved P2 without source must not complete')

    def test_unselected_focus_mandatory_but_window_not_promoted(self):
        m={'content_sha256':'test','candidates':[{'candidate_id':'a','selected':False,'priority':'P2','manifestation':'stable_slow:1:30'}]}
        self.assertEqual([],make_routing(m)['records'])
        m['candidates'][0]['triggers']=['user_focus']
        self.assertEqual('mandatory_deep',make_routing(m)['records'][0]['route'])

    def test_selected_lower_tiers_keep_existing_deep_requirement(self):
        for old in ['P3','P4']:
            m={'content_sha256':'test','candidates':[{'candidate_id':'a','selected':True,'priority':old}]}
            self.assertEqual('mandatory_deep',make_routing(m)['records'][0]['route'])

    def test_complete_build_and_revalidate_package(self):
        import subprocess
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);a,c,e=self.fixture(root)
            save(root/'analysis-result.json',a);save(root/'candidate-manifest.json',c);save(root/'evidence-manifest.json',e)
            # Existing numeric validator binds the manifest file as well as its content identity.
            a['query_scan']['candidate_manifest_sha256']=hashlib.sha256((root/'candidate-manifest.json').read_bytes()).hexdigest()
            save(root/'analysis-result.json',a)
            helper=DeepWorkflowTests()
            result=helper.command(root,'build_report.py',('--output',str(root/'report'),'--deterministic'))
            self.assertEqual(0,result.returncode,result.stdout+result.stderr)
            result=helper.command(root/'report')
            self.assertEqual(0,result.returncode,result.stdout+result.stderr)
            self.assertTrue((root/'report/P1-Source-Review-Candidates.md').is_file())

    def test_filtered_candidate_preserves_facts_and_does_not_claim_source(self):
        from performance_review import validate_review
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);a,c,e=self.fixture(root)
            row=a['review_routing']['records'][0]
            row.update(route='filtered',decision='not_applicable',screening_facts=copy.deepcopy(c['candidates'][0]['trigger_evidence']))
            a['candidate_findings']=[]
            errors,model=validate_review(a,c,e,root)
            self.assertEqual([],errors)
            self.assertEqual(0,model['counts']['deep_completed'])
            row['screening_facts']=[]
            with self.assertRaisesRegex(ValueError,'screening facts'):validate_routing(a,c,e,root)

    def test_native_p1_is_new_p0_and_still_requires_source(self):
        from performance_review import validate_review
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);a,c,e=self.fixture(root)
            c['candidates'][0]['priority']='P1';a['candidate_findings'][0]['priority']='P1'
            row=a['review_routing']['records'][0]
            row.update(query_priority='P1',analysis_priority='P0',route='mandatory_deep',decision='not_applicable')
            errors,_=validate_review(a,c,e,root)
            self.assertTrue(errors)

    def test_process_issue_cannot_have_performance_priority(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);a,c,e=self.fixture(root)
            a['process_issues']=[{'priority':'P0','category':'query'}]
            with self.assertRaisesRegex(ValueError,'performance priority'):validate_routing(a,c,e,root)

    def test_resume_state_separates_query_review_from_source_completion(self):
        import subprocess
        from tests.test_skill_v2_contract import load_state_tool, STATE_TEMPLATE
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);a,c,e=self.fixture(root)
            save(root/'candidate-manifest.json',c)
            a['query_scan']['candidate_manifest_sha256']=hashlib.sha256((root/'candidate-manifest.json').read_bytes()).hexdigest()
            save(root/'analysis-result.json',a);save(root/'evidence-manifest.json',e)
            tool=load_state_tool();state=tool.new_state(STATE_TEMPLATE)
            state['scan']['candidate_content_sha256']=c['content_sha256']
            state['scan']['policy_algorithm']='candidate-policy-v3'
            save(root/'state.json',state)
            result=subprocess.run([sys.executable,'-X','utf8','-B',str(ROOT/'scripts/manage-analysis-state.py'),
                'record-investigation-audit','--state',str(root/'state.json'),'--analysis',str(root/'analysis-result.json'),
                '--candidates',str(root/'candidate-manifest.json'),'--evidence',str(root/'evidence-manifest.json')],capture_output=True,text=True,encoding='utf-8')
            self.assertEqual(0,result.returncode,result.stdout+result.stderr)
            saved=json.loads((root/'state.json').read_text(encoding='utf-8'))
            self.assertEqual([],saved['investigations']['completed'])
            self.assertEqual([c['candidates'][0]['candidate_id']],saved['investigations']['tester_pending_ids'])
            self.assertEqual([],tool.validate_investigation_audit(saved))
            before=copy.deepcopy(saved['investigations'])
            tool.ingest_candidate_pages(saved,[{'items':c['candidates'],'page':{'cursor':'','next_cursor':None,'done':True,'total':'1'}}])
            self.assertEqual(before,saved['investigations'])



if __name__ == '__main__':
    unittest.main()
