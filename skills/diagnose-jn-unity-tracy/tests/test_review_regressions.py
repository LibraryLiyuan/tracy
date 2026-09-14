import copy
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from tests.review_fixtures import current_policy_fixture, save
from tests.test_query_native_report_contract import load_contract
from tests.test_investigation_receipts import store

ROOT = Path(__file__).resolve().parents[1]
load_contract()
from report_common import validate_analysis_and_evidence

class ReviewRegressions(unittest.TestCase):
    def fixture(self, root):
        ap, ep, cp, a, c = current_policy_fixture(root)
        return a, c, json.loads(ep.read_text()), ap, ep, cp

    def test_unknown_missing_and_old_policy_cannot_bypass_receipts(self):
        import hashlib
        for policy in (None, 'candidate-policy-v1', 'candidate-policy-v999'):
            with self.subTest(policy=policy), tempfile.TemporaryDirectory() as temp:
                root = Path(temp); a,c,e,ap,ep,cp = self.fixture(root)
                c['policy_algorithm'] = policy
                a['candidate_findings'][0].pop('investigation_receipts')
                errors = load_contract().validate_query_native_analysis(a,c,e,root)
                self.assertTrue(any('policy_algorithm' in x for x in errors),errors)
                save(cp,c);a['query_scan']['candidate_manifest_sha256']=hashlib.sha256(cp.read_bytes()).hexdigest();save(ap,a)
                for script,extra in [('validate_analysis_result.py',[]),('build_report.py',['--validate-only'])]:
                    result=subprocess.run([sys.executable,'-X','utf8','-B',str(ROOT/'scripts'/script),'--analysis',str(ap),
                        '--evidence',str(ep),'--candidates',str(cp),*extra],capture_output=True,text=True,encoding='utf-8')
                    self.assertEqual(1,result.returncode,result.stdout)
                    self.assertIn('policy_algorithm',result.stderr)

    def test_real_relation_cannot_be_reversed_or_retyped(self):
        for patch in ({'from_ref':'zone-tsr'}, {'to_ref':'zone-player-frame'}, {'relation':'invented'}):
            with self.subTest(patch=patch), tempfile.TemporaryDirectory() as temp:
                root=Path(temp); a,c,e,*_=self.fixture(root)
                self.assertEqual([],validate_analysis_and_evidence(a,e,root)[0])
                a['bottleneck_signatures'][0]['causal_chain'][0].update(patch)
                self.assertTrue(validate_analysis_and_evidence(a,e,root)[0])

    def test_asserted_source_flags_are_not_source_confirmation(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp); a,c,e,*_=self.fixture(root)
            a['candidate_findings'][0].update(conclusion_status='Confirmed',confirmation_basis=['source_revision_match'],
                source_evidence=[{'source_id':'source-asserted','revision_match':True,'used_for_confirmation':True}])
            self.assertTrue(load_contract().validate_query_native_analysis(a,c,e,root))

    def test_relation_from_another_frame_is_not_support_for_this_signature(self):
        import hashlib
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);a,c,e,*_=self.fixture(root)
            item=e['evidence'][0];path=root/item['data_path'];data=json.loads(path.read_text())
            data['frame_id']=9999;save(path,data);item['response']['sha256']=hashlib.sha256(path.read_bytes()).hexdigest()
            self.assertTrue(validate_analysis_and_evidence(a,e,root)[0])

    def test_duplicate_refs_and_receipt_aliases_are_not_independent(self):
        for alias in (False,True):
            with self.subTest(alias=alias), tempfile.TemporaryDirectory() as temp:
                root=Path(temp); a,c,e,*_=self.fixture(root)
                other=copy.deepcopy(e['evidence'][-2]); other['evidence_id']='E-ALIAS'; e['evidence'].append(other)
                a['candidate_findings'][0].update(conclusion_status='Confirmed',confirmation_basis=['independent_evidence'],
                    evidence_refs=['E-TREE','E-ALIAS' if alias else 'E-TREE'])
                self.assertTrue(load_contract().validate_query_native_analysis(a,c,e,root))

    def test_completed_state_cannot_bypass_audit_with_unknown_policy(self):
        spec=importlib.util.spec_from_file_location('review_state',ROOT/'scripts/manage-analysis-state.py')
        module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
        for policy in (None,'candidate-policy-v1','candidate-policy-v999'):
            state={'schema_version':4,'task':{'state':'completed'},'scan':{'policy_algorithm':policy},
                'investigations':{'completed':[],'coverage':{'required_total':0,'required_completed':0}}}
            self.assertTrue(module.validate_state(state))

    def test_query_native_entry_rejects_legacy_analysis_instead_of_skipping_it(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);a,c,e,*_=self.fixture(root)
            a.pop('query_scan');a['report_versions'].pop('analysis_workflow')
            self.assertTrue(load_contract().validate_query_native_analysis(a,c,e,root))

    def test_terminal_state_rechecks_actual_coverage_and_relations(self):
        import hashlib
        spec=importlib.util.spec_from_file_location('audited_state',ROOT/'scripts/manage-analysis-state.py')
        module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp)
            from tests.test_deep_workflow import DeepWorkflowTests
            a,c,e=DeepWorkflowTests().fixture(root)
            ap,ep,cp=root/'analysis-result.json',root/'evidence-manifest.json',root/'candidate-manifest.json'
            state=module.new_state(ROOT/'assets/analysis-state-template.json')
            state['scan'].update(policy_algorithm='candidate-policy-v3',candidate_content_sha256=c['content_sha256'])
            state['task']['state']='completed'
            state['investigations'].update(completed=[{'candidate_id':c['candidates'][0]['candidate_id']}],
                queue=[],backlog=[],coverage=copy.deepcopy(a['query_scan']['coverage']))
            def audit():
                state['investigations']['artifact_audit']={k:{'path':str(p),'sha256':hashlib.sha256(p.read_bytes()).hexdigest()}
                    for k,p in [('analysis',ap),('evidence',ep),('candidates',cp)]}
            audit();self.assertEqual([],module.validate_state(state))
            state['investigations']['coverage']['required_total']=0
            state['investigations']['coverage']['required_completed']=0
            self.assertTrue(module.validate_state(state),'Terminal counters must match the audited candidate manifest')
            state['investigations']['coverage']=copy.deepcopy(a['query_scan']['coverage'])
            a['bottleneck_signatures'][0]['causal_chain'][0]['relation']='invented'
            save(ap,a);audit();self.assertTrue(module.validate_state(state))

    def test_real_wire_unavailable_uses_verified_entity_anchor(self):
        from investigation_receipts import validate_receipts
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp); a,c,e,*_=self.fixture(root); f=a['candidate_findings'][0]
            # Source ref is returned by a successful directed query, not invented in the failed request.
            tree=next(x for x in e['evidence'] if x['evidence_id']=='E-TREE')
            response=json.loads((root/tree['response']['path']).read_text())
            response['result']['structuredContent']['data']['root']['source_location_ref']='source:work'
            tree['response']=store(root,tree['response']['path'],response)
            f['conclusion_status']='Unresolved'
            anchor=copy.deepcopy(f['investigation_receipts'][0])
            for i in range(3):
                req={'jsonrpc':'2.0','id':20+i,'method':'tools/call','params':{'name':'tracy_inspect','arguments':{
                    'trace_id':'trace-1','method':'source.lines','params':{'ref':'source:work'}}}}
                res={'jsonrpc':'2.0','id':20+i,'result':{'isError':True,'structuredContent':{
                    'protocol':'tracy-query','schema_version':'1.35.0','id':20+i,'ok':False,
                    'error':{'code':'CAPABILITY_UNAVAILABLE','message':'source is not embedded in the trace','retryable':False,'details':{}}}}}
                eid='E-FAIL-'+str(i)
                e['evidence'].append({'evidence_id':eid,'request':store(root,eid+'-q.json',req),'response':store(root,eid+'-r.json',res)})
                f['investigation_receipts'].append({'evidence_id':eid,'purpose':'source_lookup','manifestation':'frame_cost',
                    'outcome':'unavailable','unavailable_anchor':anchor,'unavailable_ref_json_pointer':'/data/root/source_location_ref'})
            self.assertEqual([],validate_receipts(c['candidates'][0],f,e,root))
            original=e['evidence'][-1]['response']
            for code in ('INVALID_PARAMS','BUDGET_EXCEEDED','TRACE_NOT_FOUND'):
                res['result']['structuredContent']['error']['code']=code
                e['evidence'][-1]['response']=store(root,'bad-error.json',res)
                self.assertTrue(validate_receipts(c['candidates'][0],f,e,root),code)
            e['evidence'][-1]['response']=original
            original=e['evidence'][-1]['request']
            req['params']['arguments']['trace_id']='other-trace'
            e['evidence'][-1]['request']=store(root,'bad-trace.json',req)
            self.assertTrue(validate_receipts(c['candidates'][0],f,e,root))
            e['evidence'][-1]['request']=original
            f['investigation_receipts'][-1]['unavailable_ref_json_pointer']='/data/root/ref'
            self.assertTrue(validate_receipts(c['candidates'][0],f,e,root))

    def test_source_confirmation_checks_real_git_content_and_capture_revision(self):
        import hashlib
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp); a,c,e,*_=self.fixture(root)
            repo=root/'repo';repo.mkdir();(repo/'work.cpp').write_bytes(b'void Work() {}\n')
            def git(*args):
                result=subprocess.run(['git','-C',str(repo),*args],capture_output=True,check=True)
                return result.stdout.decode().strip()
            git('init');git('add','work.cpp');git('-c','user.name=Regression','-c','user.email=regression@example.invalid','commit','-m','fixture')
            revision=git('rev-parse','HEAD');payload=(repo/'work.cpp').read_bytes();(root/'source.cpp').write_bytes(payload)
            req={'jsonrpc':'2.0','id':60,'method':'tools/call','params':{'name':'tracy_inspect','arguments':{
                'trace_id':'trace-1','method':'trace.identity','params':{}}}}
            res={'jsonrpc':'2.0','id':60,'result':{'structuredContent':{'ok':True,'trace':{'fingerprint':'a'*64},'data':{
                'present':True,'complete':True,'conflicts':[],'invalid_records':[],
                'identity':{'build':{'repositories':{'engine':{'revision':revision}}}}}}}}
            build=copy.deepcopy(e['evidence'][-1]);build.update(evidence_id='E-BUILD',type='mcp_query',data_path='build-r.json',
                mcp={'method':'tools/call','params':req['params']},request=store(root,'build-q.json',req),response=store(root,'build-r.json',res))
            e['evidence'].append(build)
            source={'source_id':'source-work','revision_match':True,'used_for_confirmation':True,
                'repository':str(repo),'repository_key':'engine','revision':revision,'expected_revision':revision,
                'path':'work.cpp','line':1,'symbol':'Work','finding':'The inspected function is empty.',
                'build_identity_evidence_id':'E-BUILD','snapshot':{'path':'source.cpp','sha256':hashlib.sha256(payload).hexdigest()}}
            f=a['candidate_findings'][0];f.update(conclusion_status='Confirmed',confirmation_basis=['source_revision_match'],source_evidence=[source])
            validate=lambda:load_contract().validate_query_native_analysis(a,c,e,root)
            self.assertEqual([],validate())
            save(root/'analysis-result.json',a);save(root/'evidence-manifest.json',e)
            output=root/'source-report'
            result=subprocess.run([sys.executable,'-X','utf8','-B',str(ROOT/'scripts/build_report.py'), '--legacy-regression','--analysis',str(root/'analysis-result.json'),
                '--evidence',str(root/'evidence-manifest.json'),'--candidates',str(root/'candidate-manifest.json'),'--output',str(output),'--deterministic'],
                capture_output=True,text=True,encoding='utf-8')
            self.assertEqual(0,result.returncode,result.stderr)
            self.assertEqual(payload,(output/'source.cpp').read_bytes())
            self.assertEqual([],load_contract().validate_query_native_analysis(a,c,e,output))
            for field in ('repository','revision','expected_revision','path','snapshot','build_identity_evidence_id','symbol','line'):
                original=source.pop(field)
                self.assertTrue(validate(),field)
                source[field]=original
            source['path']='missing.cpp';self.assertTrue(validate());source['path']='work.cpp'
            source['expected_revision']='b'*40;self.assertTrue(validate());source['expected_revision']=revision
            (root/'source.cpp').write_bytes(b'void Other() {}\n')
            source['snapshot']['sha256']=hashlib.sha256((root/'source.cpp').read_bytes()).hexdigest()
            self.assertTrue(validate(),'A self-consistent snapshot hash must still match the Git object')

    def test_independent_channels_have_explicit_support_and_distinct_results(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);a,c,e,*_=self.fixture(root);f=a['candidate_findings'][0]
            f.update(conclusion_status='Confirmed',confirmation_basis=['independent_evidence'],evidence_refs=['E-TREE','E-FRAME'],
                independent_evidence=[{'evidence_id':'E-TREE','channel':'zone','supports':'The event occupies this interval.'},
                    {'evidence_id':'E-FRAME','channel':'frame','supports':'The independent frame marker bounds this interval.'}])
            self.assertEqual([],load_contract().validate_query_native_analysis(a,c,e,root))
            f['independent_evidence'][1]['channel']='zone'
            self.assertTrue(load_contract().validate_query_native_analysis(a,c,e,root))

    def test_built_report_keeps_raw_receipts_for_revalidation(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);a,c,e,ap,ep,cp=self.fixture(root);out=root/'report'
            result=subprocess.run([sys.executable,'-B',str(ROOT/'scripts/build_report.py'), '--legacy-regression','--analysis',str(ap),
                '--evidence',str(ep),'--candidates',str(cp),'--output',str(out),'--deterministic'],capture_output=True,text=True)
            self.assertEqual(0,result.returncode,result.stderr)
            result=subprocess.run([sys.executable,'-B',str(ROOT/'scripts/validate_analysis_result.py'), '--legacy-regression','--analysis',str(out/'analysis-result.json'),
                '--evidence',str(out/'evidence-manifest.json'),'--candidates',str(out/'candidate-manifest.json')],capture_output=True,text=True)
            self.assertEqual(0,result.returncode,result.stderr)

if __name__=='__main__':unittest.main()
