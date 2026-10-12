"""Export registered views through the live native renderer, checking receipts."""
import argparse
import json
from pathlib import Path
import re
import time
from urllib.request import Request, urlopen
from urllib.error import HTTPError
from server import read, sha, atomic

def fetch(url,body=None,token=None):
    headers={'Content-Type':'application/json'}
    if token:headers['X-Tracy-Token']=token
    try:
        with urlopen(Request(url,data=None if body is None else json.dumps(body).encode(),headers=headers),timeout=10) as r:return r.read()
    except HTTPError as error:raise RuntimeError(error.read().decode()) from error

def navigate(url,ident,action='open',timeout=180):
    page=fetch(url+'/open/'+ident).decode()
    token=re.search(r'token="([a-f0-9]+)"',page)[1]
    request=json.loads(fetch(url+'/api/open/'+ident,{'action':action},token))
    start=time.monotonic()
    while time.monotonic()-start<timeout:
        state=json.loads(fetch(url+'/api/status'))
        if state.get('sequence')==request['sequence']:
            if state.get('phase')=='failed':raise RuntimeError(json.dumps(state,ensure_ascii=False))
            if state.get('phase')=='ready':return state
        time.sleep(.15)
    raise TimeoutError(f'{ident}: {state}')

def validate(state,view,config):
    loc=view['location']
    for key in ['frame_set','frame_index','frame_begin_ns','frame_end_ns','focus_name','focus_gpu']:
        if state.get(key)!=loc[key]:raise ValueError('Location mismatch: '+key)
    if not state.get('ok') or not state.get('layout_ready') or state.get('content_verification_state')!=2:raise ValueError('Native renderer not verified/ready')
    if state['source_sha256']!=config['capture']['sha256'] or str(state['revision'])!=str(config['capture']['revision']):raise ValueError('Wrong capture')
    for a,b in [('actual_view_begin_ns','view_begin_ns'),('actual_view_end_ns','view_end_ns')]:
        if state[a]!=loc[b]:raise ValueError('View range mismatch')
    if loc.get('event'):
        for key in ['query_index','gpu','begin_ns','end_ns','name']:
            if state['event'][key]!=loc['event'][key]:raise ValueError('Wrong event: '+key)
        if not state['selected_event_visible']:raise ValueError('Event not visible')

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--state-dir',required=True);p.add_argument('--only',nargs='*');a=p.parse_args()
    directory=Path(a.state_dir);service=read(directory/'server-state.json');config=read(service['config'])
    results=[]
    for view in config['views']:
        if a.only and view['id'] not in a.only:continue
        start=time.monotonic();state=navigate(service['url'],view['id']);validate(state,view,config)
        target=directory/'views'/(view['id']+'.json')
        for _ in range(50):
            if target.exists() and read(target).get('sequence')==state['sequence']:break
            time.sleep(.1)
        receipt=read(target);image=target.with_suffix('.png')
        if receipt['sequence']!=state['sequence'] or sha(image)!=receipt['image_sha256']:raise ValueError('Saved image/receipt mismatch')
        result={'view_id':view['id'],'elapsed_seconds':round(time.monotonic()-start,3),'native_receipt':str(target),'image':str(image),'passed':True}
        results.append(result);print(json.dumps(result,ensure_ascii=False),flush=True)
        atomic(directory/'export-validation.json',{'schema_version':1,'views':results,'all_requested_complete':False})
    atomic(directory/'export-validation.json',{'schema_version':1,'views':results,'all_requested_complete':True})

if __name__=='__main__':main()
