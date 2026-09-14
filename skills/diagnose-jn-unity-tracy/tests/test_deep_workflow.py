"""End-to-end acceptance for source-required, issue-oriented performance reports."""
import copy
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from tests.review_fixtures import current_policy_fixture,save

ROOT=Path(__file__).resolve().parents[1]

class DeepWorkflowTests(unittest.TestCase):
    def rewrite_receipt(self,root,e,eid,method=None,params=None,data=None):
        item=next(x for x in e['evidence'] if x['evidence_id']==eid)
        request=json.loads((root/item['request']['path']).read_text(encoding='utf-8'))
        response=json.loads((root/item['response']['path']).read_text(encoding='utf-8'))
        if method:request['params']['arguments']['method']=method
        if params is not None:request['params']['arguments']['params']=params
        if data is not None:response['result']['structuredContent']['data']=data
        item['mcp']['params']=request['params']
        for key,value in [('request',request),('response',response)]:
            path=root/item[key]['path'];save(path,value);item[key]['sha256']=hashlib.sha256(path.read_bytes()).hexdigest()
        save(root/'evidence-manifest.json',e)

    def command(self,root,script='validate_analysis_result.py',extra=()):
        return subprocess.run([sys.executable,'-X','utf8','-B',str(ROOT/'scripts'/script),
            '--analysis',str(root/'analysis-result.json'),'--evidence',str(root/'evidence-manifest.json'),
            '--candidates',str(root/'candidate-manifest.json'),*extra],capture_output=True,text=True,encoding='utf-8')

    def test_old_supported_without_source_cannot_complete_new_workflow(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);ap,ep,cp,a,c=current_policy_fixture(root)
            self.assertNotEqual(0,self.command(root).returncode,'Normal entry must require the new deep analysis contract')

    def fixture(self,root):
        ap,ep,cp,a,c=current_policy_fixture(root)
        e=json.loads(ep.read_text(encoding='utf-8'))
        tree=next(x for x in e['evidence'] if x['evidence_id']=='E-TREE')
        reply=json.loads((root/tree['response']['path']).read_text(encoding='utf-8'))
        node=reply['result']['structuredContent']['data']['root'];node.update(duration_ns='17000000',self_time_ns='7000000',running_time_ns='12000000')
        save(root/tree['response']['path'],reply);tree['response']['sha256']=hashlib.sha256((root/tree['response']['path']).read_bytes()).hexdigest()
        repo=root/'source';repo.mkdir();payload=b'void Work() {\n    RebuildIfDirty();\n}\n'
        (repo/'Work.cpp').write_bytes(payload);(root/'Work-source.cpp').write_bytes(payload)
        profile={'frame_budget':{'target_fps':60.0,'frame_ms':16.666667}}
        c['profile_identity']=hashlib.sha256(json.dumps(profile,sort_keys=True,separators=(',',':')).encode()).hexdigest()
        req={'jsonrpc':'2.0','id':99,'method':'tools/call','params':{'name':'tracy_scan','arguments':{'operation':'profile_validate','profile':profile}}}
        res={'jsonrpc':'2.0','id':99,'result':{'structuredContent':{'ok':True,'data':{'valid':True,'profile_identity':c['profile_identity'],'normalized_profile':profile}}}}
        from tests.test_investigation_receipts import store
        item=copy.deepcopy(e['evidence'][-1]);item.update(evidence_id='E-PROFILE',mcp={'method':'tools/call','params':req['params']},data_path='profile-response.json',
            request=store(root,'profile-request.json',req),response=store(root,'profile-response.json',res));e['evidence'].append(item)
        save(cp,c);a['query_scan']['candidate_manifest_sha256']=hashlib.sha256(cp.read_bytes()).hexdigest()
        a['report_versions'].update(analysis_workflow='2.1.0',report_builder='2.1.0',template='2.1.0')
        a['performance_review']={
            'schema_version':1,
            'profile_evidence_id':'E-PROFILE',
            'candidate_views':[{'candidate_id':c['candidates'][0]['candidate_id'],'name':'Work event cost',
                'classification':{'level':'event','pattern':'frame_cost','cost_kind':'cpu_event_cost','domain':'cpu',
                    'frame_scope':'Player.Frame','thread_or_queue':'thread:main','selection_reasons':['budget']}}],
            'evidence_labels':{'E-TREE':'Work event tree','E-FRAME':'Player.Frame timing','E-PROFILE':'Validated frame budget'},
            'events':[{'event_id':'event-work','name':'Work','thread_name':'Main thread','evidence_id':'E-TREE','pointer':'/data/root'}],
            'sources':[{'source_id':'source-work','name':'Work implementation','repository':str(repo),'path':'Work.cpp','line':1,'end_line':3,
                'symbol':'Work','revision':'current-checkout','version_status':'unverified',
                'snapshot':{'path':'Work-source.cpp','sha256':hashlib.sha256(payload).hexdigest()},
                'behavior':'Rebuilds dirty state.','conditions':'The dirty flag determines whether rebuilding runs.',
                'existing_controls':'Already tests dirty state.','runtime_support':'The event establishes local cost; branch execution is not yet proved.',
                'evidence_refs':['E-TREE']}],
            'issues':[{'issue_id':'issue-work','title':'Work rebuild cost','candidate_ids':[c['candidates'][0]['candidate_id']],
                'analysis_state':'complete','readiness':'mechanism_supported','source_refs':['source-work'],
                'source_applicability':'This implementation explains the named operation; capture revision is unverified.',
                'merge':{'basis':'single_candidate','reason':'Only one candidate is included.'},
                'mechanism':'Dirty-state rebuilding participates in this operation; actual branch frequency is not yet known.',
                'trigger':'Dirty state or work volume requires verification.',
                'samples':[{'sample_id':'sample-42','role':'peak','reason':'Directed representative from the candidate.',
                    'frame_name':'Player.Frame','frame_evidence_id':'E-FRAME','frame_pointer':'/data','event_ids':['event-work']}],
                'comparison_gap':'A comparable normal frame has not yet been queried.',
                'discovery_steps':[{'question':'Where does the frame spend time?','action':'Read the directed event tree.',
                    'observation':'The recorded Work event spans the representative frame.','judgment':'Inspect the implementation and dirty-state trigger.',
                    'sample_ids':['sample-42'],'evidence_refs':['E-TREE','E-FRAME'],'source_refs':['source-work']}],
                'counterevidence':[{'observation':'The implementation already avoids work when clean.','effect':'Do not recommend adding a redundant dirty cache.',
                    'evidence_refs':['E-TREE'],'source_refs':['source-work']}],
                'scope':'Only the observed representative event is established.',
                'conclusion':'The local operation is expensive; avoidable work has not been proved.',
                'limitations':['Source revision and runtime dirty-state conditions remain unverified.'],
                'next_actions':['Query comparable normal frames and dirty-state changes.']}],
            'dedup_review':[]}
        save(ap,a);save(ep,e)
        return a,c,e

    def test_source_and_discovery_contract_positive_and_negative(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);a,c,e=self.fixture(root)
            self.assertEqual(0,self.command(root).returncode,self.command(root).stderr)
            for field in ('sources','events','candidate_views'):
                bad=copy.deepcopy(a);bad['performance_review'][field]=[];save(root/'analysis-result.json',bad)
                self.assertNotEqual(0,self.command(root).returncode,field)
            for field in ('source_refs','discovery_steps','counterevidence','samples'):
                bad=copy.deepcopy(a);bad['performance_review']['issues'][0][field]=[];save(root/'analysis-result.json',bad)
                self.assertNotEqual(0,self.command(root).returncode,field)

    def test_group_duplicate_candidates_but_not_duplicate_issues(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);a,c,e=self.fixture(root)
            other=copy.deepcopy(c['candidates'][0]);other['candidate_id']='candidate-other';c['candidates'].append(other)
            finding=copy.deepcopy(a['candidate_findings'][0]);finding['candidate_id']='candidate-other';a['candidate_findings'].append(finding)
            view=copy.deepcopy(a['performance_review']['candidate_views'][0]);view['candidate_id']='candidate-other';a['performance_review']['candidate_views'].append(view)
            issue=a['performance_review']['issues'][0];issue['candidate_ids'].append('candidate-other');issue['merge']={'basis':'same_event','reason':'Both candidates cite the same actual event.'}
            a['query_scan']['coverage'].update(candidate_total=2,required_total=2,required_completed=2,investigated_total=2)
            save(root/'candidate-manifest.json',c);a['query_scan']['candidate_manifest_sha256']=hashlib.sha256((root/'candidate-manifest.json').read_bytes()).hexdigest();save(root/'analysis-result.json',a)
            self.assertEqual(0,self.command(root).returncode,self.command(root).stderr)
            duplicate=copy.deepcopy(issue);duplicate['issue_id']='issue-copy';a['performance_review']['issues'].append(duplicate);save(root/'analysis-result.json',a)
            self.assertNotEqual(0,self.command(root).returncode)

    def test_readable_report_has_frame_chain_percentages_source_and_no_internal_ids(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);a,c,e=self.fixture(root);out=root/'report'
            result=self.command(root,'build_report.py',('--output',str(out),'--deterministic','--html','--standalone'))
            self.assertEqual(0,result.returncode,result.stderr)
            main=(out/'Performance-Analysis-Report.md').read_text(encoding='utf-8')
            for expected in ('Player.Frame','42','17.000','100.00%','7.000','Work.cpp','RebuildIfDirty','为什么','源码'):
                self.assertIn(expected,main)
            for p in out.rglob('*'):
                if p.suffix in {'.md','.html','.svg'}:
                    content=p.read_text(encoding='utf-8')
                    self.assertNotIn('candidate-tsr',content)
                    self.assertNotIn('E-TREE',content)
                    self.assertNotIn('a'*64,content)

    def test_blocked_source_is_stage_not_deep_completion(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);a,c,e=self.fixture(root);a['report_status']='in_progress'
            issue=a['performance_review']['issues'][0];issue.update(analysis_state='blocked',source_refs=[],
                source_blocker={'reason':'The actual implementation could not be located.','searches':['Searched the authorized engine source by Work symbol.'],
                    'next_action':'Resolve the exact implementation symbol.'})
            save(root/'analysis-result.json',a)
            result=self.command(root);self.assertEqual(0,result.returncode,result.stderr)
            self.assertFalse(json.loads(result.stdout)['analysis_complete'])
            a['report_status']='complete';save(root/'analysis-result.json',a);self.assertNotEqual(0,self.command(root).returncode)

    def test_one_frame_candidate_can_have_distinct_reviewed_causes(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);a,c,e=self.fixture(root);review=a['performance_review']
            other=copy.deepcopy(review['issues'][0]);other.update(issue_id='issue-second',title='Distinct part of Work',
                mechanism='A separate condition needs investigation.',trigger='A different branch is under investigation.')
            review['issues'].append(other)
            review['candidate_splits']=[{'candidate_id':c['candidates'][0]['candidate_id'],'issue_ids':['issue-work','issue-second'],
                'reason':'The frame candidate contains two different local explanations, not two counted frames.',
                'evidence_refs':['E-TREE'],'source_refs':['source-work']}]
            review['dedup_review']=[{'issue_ids':['issue-work','issue-second'],'reason':'The two explanations address distinct parts; cost is not summed.',
                'evidence_refs':['E-TREE'],'source_refs':['source-work']}]
            save(root/'analysis-result.json',a);result=self.command(root)
            self.assertEqual(0,result.returncode,result.stderr)

    def test_unresolved_raw_name_has_explicit_context_label(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);a,c,e=self.fixture(root)
            # Keep a real readable function-independent location without pretending to resolve the name.
            event=a['performance_review']['events'][0]
            event.update(display_name='未解析事件',display_name_basis='unresolved_context')
            # A context label is valid even before changing to an opaque raw name.
            save(root/'analysis-result.json',a);out=root/'report'
            result=self.command(root,'build_report.py',('--output',str(out)))
            self.assertEqual(0,result.returncode,result.stderr)
            self.assertIn('未解析事件',(out/'Performance-Analysis-Report.md').read_text(encoding='utf-8'))

    def test_gpu_interval_is_not_reported_as_player_frame(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);a,c,e=self.fixture(root);candidate=c['candidates'][0];finding=a['candidate_findings'][0]
            interval={'begin_ns':'1000000','end_ns':'18000000','event_refs':['zone:work']}
            candidate.update(domain='gpu',observation_unit='l0_segment',representative_frames=[],representative_frame_details=[],representative_intervals=[interval],thread_or_queue='queue:direct')
            finding.update(representative_frames=[],representative_intervals=[interval])
            finding['investigation_receipts']=[{'evidence_id':'E-TREE','purpose':'zone_tree','manifestation':'frame_cost',
                'entity_json_pointers':['/data/root'],'interval_anchor_evidence_id':'E-FRAME','interval_anchor_json_pointer':'/data'}]
            node={'ref':'zone:work','name':'Work','context_ref':'queue:direct','thread_ref':'thread:main','parent_ref':None,
                'complete':True,'gpu_start_ns':'1000000','gpu_end_ns':'18000000','gpu_duration_ns':'17000000','self_time_ns':'7000000'}
            self.rewrite_receipt(root,e,'E-TREE','zone.gpu.tree',{'ref':'zone:work'},{'root':node,'children':[]})
            self.rewrite_receipt(root,e,'E-FRAME','zone.gpu.get',{'ref':'zone:work'},node)
            review=a['performance_review'];review['candidate_views'][0]['classification'].update(domain='gpu',cost_kind='gpu_execution',thread_or_queue='queue:direct')
            review['issues'][0]['samples'][0].update(kind='gpu_interval',frame_name='GPU Direct interval')
            review['events'][0]['thread_name']='GPU Direct'
            save(root/'candidate-manifest.json',c);a['query_scan']['candidate_manifest_sha256']=hashlib.sha256((root/'candidate-manifest.json').read_bytes()).hexdigest();save(root/'analysis-result.json',a)
            result=self.command(root,'build_report.py',('--output',str(root/'report')))
            self.assertEqual(0,result.returncode,result.stderr)
            text=(root/'report/Performance-Analysis-Report.md').read_text(encoding='utf-8')
            self.assertIn('占GPU片段',text);self.assertNotIn('Player.Frame 42',text)
            review['issues'][0]['samples'][0]['kind']='frame';save(root/'analysis-result.json',a)
            self.assertNotEqual(0,self.command(root).returncode)

    def test_job_sample_uses_origin_identity_and_does_not_invent_cpu_duration(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);a,c,e=self.fixture(root)
            from tests.job_report_fixture import attach_job
            attach_job(root,a,c,root/'evidence-manifest.json')
            a['bottleneck_signatures']=[];a['evidence_cards']=[]
            e=json.loads((root/'evidence-manifest.json').read_text(encoding='utf-8'))
            review=a['performance_review'];issue=review['issues'][0]
            sys.path.insert(0,str(ROOT/'scripts'))
            from performance_review import classify
            review['candidate_views'][0]['classification']=classify(c['candidates'][0])
            review['evidence_labels']={'J':'Job execution record','F':'OriginFrame identity','E-PROFILE':'Validated frame budget'}
            review['events']=[{'event_id':'event-work','name':'Unity.Job.NativeSingle','thread_name':'Job worker',
                'evidence_id':'J','pointer':'/data','timing_gap':'Job has separate phases; no single CPU duration is established.'}]
            issue['samples'][0].update(kind='origin_frame',frame_name='Player OriginFrame',frame_evidence_id='F',frame_pointer='/data/identity')
            review['sources'][0]['evidence_refs']=['J'];issue['discovery_steps'][0]['evidence_refs']=['J','F'];issue['counterevidence'][0]['evidence_refs']=['J']
            save(root/'candidate-manifest.json',c);a['query_scan']['candidate_manifest_sha256']=hashlib.sha256((root/'candidate-manifest.json').read_bytes()).hexdigest();save(root/'analysis-result.json',a)
            result=self.command(root,'build_report.py',('--output',str(root/'report')))
            self.assertEqual(0,result.returncode,result.stderr)
            text=(root/'report/Performance-Analysis-Report.md').read_text(encoding='utf-8')
            self.assertIn('原生帧序列361',text);self.assertNotIn('281474976711017',text);self.assertNotIn('17.000 ms',text)

    def test_measurement_references_recorded_number_not_authored_value(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);a,c,e=self.fixture(root);issue=a['performance_review']['issues'][0]
            m={'name':'Work self duration','quantity':'duration_ns','scope':'Representative event in frame 42',
                'interpretation':'Recorded exclusive cost, not a saving estimate.','evidence_id':'E-TREE','pointer':'/data/root/self_time_ns','sample_ids':['sample-42']}
            issue['measurements']=[m];save(root/'analysis-result.json',a)
            self.assertEqual(0,self.command(root).returncode,self.command(root).stderr)
            m['value']=999;save(root/'analysis-result.json',a)
            self.assertNotEqual(0,self.command(root).returncode,'AI supplied values must not be silently accepted')

    def test_frame_budget_and_excess_are_from_validated_profile(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);a,c,e=self.fixture(root);out=root/'report'
            result=self.command(root,'build_report.py',('--output',str(out)))
            self.assertEqual(0,result.returncode,result.stderr)
            self.assertIn('0.333333 ms',(out/'Performance-Analysis-Report.md').read_text(encoding='utf-8'))
            a['performance_review'].pop('profile_evidence_id');save(root/'analysis-result.json',a)
            self.assertNotEqual(0,self.command(root).returncode)

    def test_discovery_can_cite_its_validated_frame_budget(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);a,c,e=self.fixture(root)
            a['performance_review']['issues'][0]['discovery_steps'][0]['evidence_refs'].append('E-PROFILE')
            save(root/'analysis-result.json',a);result=self.command(root)
            self.assertEqual(0,result.returncode,result.stderr)

    def test_names_wrong_parent_and_source_corruption_are_rejected(self):
        for mutation in ('opaque_name','parent','source','unit','frame_name'):
            with self.subTest(mutation=mutation),tempfile.TemporaryDirectory() as tmp:
                root=Path(tmp);a,c,e=self.fixture(root);review=a['performance_review']
                if mutation=='opaque_name':review['issues'][0]['title']='CPU/Unknown/'+'f'*24
                elif mutation=='parent':review['events'][0]['parent_event_id']='event-work'
                elif mutation=='source':(root/'Work-source.cpp').write_text('fake implementation',encoding='utf-8')
                elif mutation=='unit':review['issues'][0]['measurements']=[{'name':'Bad count','scope':'One event','interpretation':'Invalid unit declaration',
                    'quantity':'count','evidence_id':'E-TREE','pointer':'/data/root/self_time_ns','sample_ids':[]}]
                else:review['issues'][0]['samples'][0]['frame_name']='Another.Frame'
                save(root/'analysis-result.json',a);self.assertNotEqual(0,self.command(root).returncode,mutation)

if __name__=='__main__':unittest.main()
