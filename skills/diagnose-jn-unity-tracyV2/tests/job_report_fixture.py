import copy, json
from tests.test_job_investigation_receipts import JobReceiptTests
from tests.test_investigation_receipts import store

def attach_job(root, analysis, manifest, evidence_path):
    c,f,jq,jr,fq,fr=JobReceiptTests().fixture()
    candidate=manifest['candidates'][0];c['candidate_id']=candidate['candidate_id'];candidate.update(c)
    candidate['representative_frame_details']=[{'frame':c['representative_frames'][0],'reasons':['worst'],'event_refs':['job:44']}]
    finding=analysis['candidate_findings'][0]
    finding.update(f);finding.update(candidate_id=candidate['candidate_id'],signature_id=c['signature_id'],
        representative_frames=copy.deepcopy(candidate['representative_frame_details']),confirmation_basis=[],evidence_refs=['J'])
    evidence=json.loads(evidence_path.read_text());template=evidence['evidence'][0]
    for eid,req,res in [('J',jq,jr),('F',fq,fr)]:
        item=copy.deepcopy(template);item.update(evidence_id=eid,type='mcp_query',
            mcp={'method':'tools/call','params':req['params']},data_path=eid+'-r.json',
            request=store(root,eid+'-q.json',req),response=store(root,eid+'-r.json',res))
        evidence['evidence'].append(item)
    evidence_path.write_text(json.dumps(evidence))
    return c['signature_id']
