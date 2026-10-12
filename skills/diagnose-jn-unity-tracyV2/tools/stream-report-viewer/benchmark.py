"""Measure real native input-to-encoded-frame latency and sustained drag delivery."""
import argparse
import json
from pathlib import Path
import re
import statistics
import threading
import time
from urllib.request import urlopen
from export import navigate,fetch
from server import atomic,read

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--url',required=True);p.add_argument('--view',default='cleanup-544');p.add_argument('--output',required=True);a=p.parse_args()
    url=a.url;ident=a.view;navigate(url,ident)
    token=re.search(r'token="([a-f0-9]+)"',fetch(url+'/open/'+ident).decode())[1]
    def send(events):return json.loads(fetch(url+'/api/input/'+ident,{'events':events},token))
    def wait(ack):
        start=time.perf_counter()
        while time.perf_counter()-start<8:
            state=json.loads(fetch(url+'/api/live'))
            if int(state.get('sequence','0'))>=int(ack):return state
            time.sleep(.003)
        raise TimeoutError(state)
    samples=[]
    for n in range(20):
        start=time.perf_counter();q=send([{'type':'wheel','x':600,'y':400,'delta':.5 if n%2==0 else -.5}]);state=wait(q['sequence'])
        samples.append((time.perf_counter()-start)*1000)
    states=[];stop=threading.Event()
    def receive():
        after='0'
        while not stop.is_set():
            with urlopen(url+'/api/frame?view='+ident+'&after='+after,timeout=3) as response:
                if response.status==204:continue
                state=json.loads(response.headers['X-Tracy-State']);response.read();after=state['frame'];states.append((time.perf_counter(),state))
    t=threading.Thread(target=receive,daemon=True);t.start();sent=[];start=time.perf_counter()
    send([{'type':'button','button':1,'down':True,'x':600,'y':700}])
    for n in range(120):
        # Two seconds of motion at the input producer, no waiting for display.
        deadline=start+(n+1)/60
        q=send([{'type':'move','x':600+(n%60)*3,'y':700}]);sent.append((q['sequence'],time.perf_counter()))
        remaining=deadline-time.perf_counter()
        if remaining>0:time.sleep(remaining)
    release_at=time.perf_counter();q=send([{'type':'button','button':1,'down':False,'x':780,'y':700}]);last=wait(q['sequence']);drain_ms=(time.perf_counter()-release_at)*1000
    time.sleep(.2);stop.set();t.join(timeout=4)
    active=[(at,s) for at,s in states if start<=at<=release_at]
    ranges={s['begin_ns'] for _,s in active};queue=max([s['input_queue_depth'] for _,s in states] or [999])
    result={'input_to_encoded_frame_ms':{'median':round(statistics.median(samples),2),'p95':round(sorted(samples)[18],2),'max':round(max(samples),2),'samples':[round(v,2) for v in samples]},
            'continuous_drag':{'input_events':120,'duration_s':round(release_at-start,3),'frames_during_drag':len(active),'distinct_ranges':len(ranges),'frame_hz':round(len(active)/(release_at-start),2),'max_native_input_queue':queue,'release_ack_ms':round(drain_ms,2),'final_native_state':last},
            'frame_encoding_ms':[s['encode_ms'] for _,s in states],
            'scope':'Client POST to complete PNG available at server; includes IPC/native render/encode, excludes browser PNG decode and painting'}
    result['passed']=result['input_to_encoded_frame_ms']['p95']<150 and len(active)>=25 and len(ranges)>=10 and queue<=4 and drain_ms<150
    atomic(a.output,result);print(json.dumps(result,ensure_ascii=False))
    if not result['passed']:raise SystemExit(1)

if __name__=='__main__':main()
