"""Build a native view registry from verified native analysis receipts."""
import argparse
import hashlib
import json
from pathlib import Path
import re

def read(p):return json.loads(Path(p).read_text(encoding='utf-8-sig'))
def sha(p):
    with Path(p).open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
def store_id(ref,kind,fingerprint):
    # CPU/GPU zone refs encode snapshot-order indices, resolved by native cursors.
    match=re.fullmatch(r'tracy:v1:'+re.escape(fingerprint[:16])+':'+re.escape(kind)+r':([0-9a-f]+)',ref)
    if not match:raise ValueError('Reference kind/source mismatch')
    return str(int(match[1],16))

def prepare(analysis,spec,profiler):
    analysis=Path(analysis).resolve();spec=read(spec);profiler=Path(profiler).resolve()
    m=read(analysis/'native-run/manifest.json');source=m['source'];fingerprint=source['source_sha256']
    if not m['complete'] or source['schema']!=17:raise ValueError('Verified schema-17 analysis required')
    frames={f['index']:f for f in map(json.loads,(analysis/'native-run/frames.ndjson').read_text().splitlines())}
    owners=read(analysis/'native-run/owners.json')
    capture=Path(m['trace']).resolve()
    views=[];ids=set()
    for entry in spec['views']:
        ident=entry['id']
        if not re.fullmatch(r'[a-zA-Z0-9_-]{1,80}',ident) or ident in ids:raise ValueError('Invalid view ID')
        ids.add(ident);frame=frames[entry['frame_index']]
        begin,end=int(frame['start_ns']),int(frame['end_ns'])
        loc={'frame_set':read(analysis/'native-run/triage.json')['frame_set']['name'],
             'frame_index':frame['index'],'frame_begin_ns':frame['start_ns'],'frame_end_ns':frame['end_ns']}
        evidence=[]
        if entry.get('event_receipt'):
            path=(analysis/entry['event_receipt']).resolve();receipt=read(path);response=receipt['response']
            if not response['ok'] or response.get('partial') or response['trace']['source_kind']!='segment' or response['trace']['fingerprint']!=fingerprint:
                raise ValueError('Event receipt is partial or from another capture')
            root=response['data']['root'];gpu=receipt['request']['method'].startswith('zone.gpu.')
            eb,ee=(root['gpu_start_ns'],root['gpu_end_ns']) if gpu else (root['start_ns'],root['end_ns'])
            if not root['complete'] or ee is None:raise ValueError('Complete event required')
            owner=root['context_ref'] if gpu else root['thread_ref']
            loc.update({'focus_owner':store_id(owner,'gpu-context' if gpu else 'thread',fingerprint),'focus_gpu':gpu,'focus_name':owners[owner],
                        'event':{'query_index':store_id(root['ref'],'gpu-zone' if gpu else 'cpu-zone',fingerprint),
                        'gpu':gpu,'begin_ns':eb,'end_ns':ee,'name':root['name']}})
            if int(ee)<=begin or int(eb)>=end:raise ValueError('Event does not overlap the registered anchor frame')
            begin=min(begin,int(eb));end=max(end,int(ee))
            evidence.append({'path':str(path),'sha256':sha(path),'query_ref':root['ref'],'encoding':'segment-snapshot-order-index'})
        else:
            owner=entry['focus_owner_ref'];loc.update({'focus_owner':store_id(owner,'thread',fingerprint),'focus_gpu':False,'focus_name':owners[owner]})
        padding=max(1000000,(end-begin)//12)
        loc['view_begin_ns']=str(max(int(frames[min(frames)]['start_ns']),begin-padding))
        loc['view_end_ns']=str(min(int(frames[max(frames)]['end_ns']),end+padding))
        views.append({'id':ident,'title':entry['title'],'note':entry.get('note',''),'location':loc,'evidence':evidence})
    return {'schema_version':1,'capture':{'path':str(capture),'sha256':fingerprint,'revision':source['revision'],
            'begin_ns':frames[min(frames)]['start_ns'],'end_ns':frames[max(frames)]['end_ns']},
            'profiler':{'path':str(profiler),'sha256':sha(profiler)},'views':views}

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--analysis',required=True);parser.add_argument('--spec',required=True);parser.add_argument('--profiler',required=True);parser.add_argument('--output',required=True)
    a=parser.parse_args();result=prepare(a.analysis,a.spec,a.profiler)
    with Path(a.output).open('x',encoding='utf-8') as out:json.dump(result,out,ensure_ascii=False,indent=2)
    print(json.dumps({'registered_views':len(result['views']),'profiler_sha256':result['profiler']['sha256']}))
