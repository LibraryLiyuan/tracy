"""Job origin identity is not a Player.Frame array index or a name match."""
import tempfile
import unittest
from pathlib import Path
from tests.test_investigation_receipts import load, store


class JobReceiptTests(unittest.TestCase):
    def fixture(self):
        candidate = dict(candidate_id='candidate:job', domain='job', metric='exclusive',
            signature_id='job:type:1:Unity.Job.NativeSingle', structural_signature='Unity.Job.NativeSingle',
            observation_unit='frame', frame_scope='Player.Frame', manifestation='',
            thread_or_queue='', representative_frames=['281474976711017'])
        finding = dict(candidate_id='candidate:job', conclusion_status='Unresolved', investigation_receipts=[
            dict(evidence_id='J', purpose='wait_completion', manifestation='',
                entity_json_pointers=['/data'], job_frame_identity_evidence_id='F')])
        def request(i, method, params):
            return dict(jsonrpc='2.0', id=i, method='tools/call', params=dict(name='tracy_inspect',
                arguments=dict(trace_id='trace-1', method=method, params=params)))
        def response(i, data):
            return dict(jsonrpc='2.0', id=i, result=dict(structuredContent=dict(ok=True, partial=False,
                trace=dict(fingerprint='a'*64), data=data)))
        job = dict(ref='job:44', job_id='68', type_id=1, name='Unity.Job.NativeSingle',
            origin_frame_ref='frame:origin361', origin_frame_sequence=361,
            schedule_ns='150', first_run_ns='160', completed_ns='230',
            truncated=False, stages=[dict(stage='worker_slice_begin', time_ns='160')])
        identity = dict(ref='frame:origin361', frame_id='281474976711017', sequence=361,
            complete=True, evidence_kind='exact', begin_ns='100', end_ns='200', events=[
                dict(canonical=True, alias=False, domain='player', phase='begin',
                    frame_id='281474976711017', frame_ref='frame:origin361', time_ns='100'),
                dict(canonical=True, alias=False, domain='player', phase='end',
                    frame_id='281474976711017', frame_ref='frame:origin361', time_ns='200')])
        return candidate, finding, request(1,'job.get',dict(ref='job:44')), response(1,job), \
            request(2,'frame.identity',dict(frame_id='281474976711017')), response(2,dict(present=True,identity=identity))

    def validate(self, root, c, f, jq, jr, fq, fr):
        evidence = dict(trace=dict(trace_id='trace-1',sha256='a'*64), evidence=[
            dict(evidence_id='J',request=store(root,'jq.json',jq),response=store(root,'jr.json',jr)),
            dict(evidence_id='F',request=store(root,'fq.json',fq),response=store(root,'fr.json',fr))])
        return load().validate_receipts(c,f,evidence,root)

    def test_complete_job_receipt_binds_origin_id_and_allows_cross_frame_completion(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.assertEqual([], self.validate(Path(tmp), *self.fixture()))

    def test_wrong_job_frame_type_and_incomplete_identity_cannot_close_candidate(self):
        for mutation in ('job_ref','job_type','job_name','origin','sequence','requested_frame','returned_frame',
                         'player_index','missing_anchor','noncanonical','missing_end','partial','truncated',
                         'other_capture','missing_job','wrong_pointer','summary_only'):
            with self.subTest(mutation=mutation), tempfile.TemporaryDirectory() as tmp:
                c,f,jq,jr,fq,fr = self.fixture()
                job=jr['result']['structuredContent']['data']; identity=fr['result']['structuredContent']['data']['identity']
                if mutation=='job_ref': job['ref']='job:other'
                elif mutation=='job_type': job['type_id']=2
                elif mutation=='job_name': job['name']='Unrelated'
                elif mutation=='origin': job['origin_frame_ref']='frame:other'
                elif mutation=='sequence': job['origin_frame_sequence']=362
                elif mutation=='requested_frame': fq['params']['arguments']['params']['frame_id']='361'
                elif mutation=='returned_frame': identity['frame_id']='361'
                elif mutation=='player_index':
                    f['investigation_receipts'][0].pop('job_frame_identity_evidence_id')
                    jq['params']['arguments']['params'].update(index='281474976711017',frame_scope='Player.Frame')
                elif mutation=='missing_anchor': f['investigation_receipts'][0].pop('job_frame_identity_evidence_id')
                elif mutation=='noncanonical': identity['events'][0]['canonical']=False
                elif mutation=='missing_end': identity['events'].pop()
                elif mutation=='partial': fr['result']['structuredContent']['partial']=True
                elif mutation=='truncated': job['truncated']=True
                elif mutation=='other_capture': fr['result']['structuredContent']['trace']['fingerprint']='b'*64
                elif mutation=='missing_job': jr['result']['structuredContent']['data']={}
                elif mutation=='wrong_pointer': f['investigation_receipts'][0]['entity_json_pointers']=[]
                elif mutation=='summary_only': jq['params']['arguments'].update(method='job.statistics',params={})
                self.assertTrue(self.validate(Path(tmp),c,f,jq,jr,fq,fr))


if __name__=='__main__': unittest.main()
