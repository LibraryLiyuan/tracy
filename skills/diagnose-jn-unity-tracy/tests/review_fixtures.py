from pathlib import Path
import copy, hashlib, json
from tests.test_query_native_report_contract import _legacy_query_native_analysis
def save(p, data):
    p.write_text(json.dumps(data), encoding='utf-8')
def current_policy_fixture(d):
    d.mkdir(exist_ok=True)
    ap,ep,cp,a,c= _legacy_query_native_analysis(d)
    c['policy_algorithm']='candidate-policy-v3'
    candidate=c['candidates'][0]
    candidate.update(domain='cpu',structural_signature='Work',frame_scope='Player.Frame',
        thread_or_queue='thread:main',metric='exclusive',manifestation='frame_cost',observation_unit='frame',
        representative_frames=['42'],representative_frame_details=[{'frame':'42','reasons':['worst'],'event_refs':['zone:work']}])
    evidence=json.loads(ep.read_text(encoding='utf-8'))
    def add(eid,method,params,data,rid):
        req={'jsonrpc':'2.0','id':rid,'method':'tools/call','params':{'name':'tracy_inspect','arguments':{
            'trace_id':'trace-1','method':method,'params':params}}}
        reply={'jsonrpc':'2.0','id':rid,'result':{'structuredContent':{'ok':True,'trace':{'fingerprint':'a'*64},
            'data':data,'partial':False,'omitted_count':'0','page':{'next_cursor':None,'has_more':False}}}}
        qp=d/(eid+'-request.json');rp=d/(eid+'-response.json');save(qp,req);save(rp,reply)
        record=copy.deepcopy(evidence['evidence'][0]);record.update(evidence_id=eid,type='mcp_query',
            mcp={'method':'tools/call','params':req['params']},data_path=rp.name,
            request={'path':qp.name,'sha256':hashlib.sha256(qp.read_bytes()).hexdigest()},
            response={'path':rp.name,'sha256':hashlib.sha256(rp.read_bytes()).hexdigest()})
        evidence['evidence'].append(record)
    add('E-TREE','zone.cpu.tree',{'ref':'zone:work'},
        {'root':{'ref':'zone:work','name':'Work','thread_ref':'thread:main','parent_ref':None,
         'start_ns':'1000000','end_ns':'18000000','complete':True},'children':[]},10)
    add('E-FRAME','frame.get',{'frame_set':'Player.Frame','index':42},
        {'frame_set_ref':'Player.Frame','index':'42','begin_ns':'1000000','end_ns':'18000000'},11)
    f=a['candidate_findings'][0];f.update(representative_frames=copy.deepcopy(candidate['representative_frame_details']),
        representative_intervals=[],evidence_refs=['E-TREE'],confirmation_basis=[],investigation_receipts=[{
            'evidence_id':'E-TREE','purpose':'zone_tree','manifestation':'frame_cost',
            'entity_json_pointers':['/data/root'],'frame_anchor_evidence_id':'E-FRAME','frame_anchor_json_pointer':'/data'}])
    save(cp,c);a['query_scan']['candidate_manifest_sha256']=hashlib.sha256(cp.read_bytes()).hexdigest()
    save(ep,evidence);save(ap,a)
    return ap,ep,cp,a,c
