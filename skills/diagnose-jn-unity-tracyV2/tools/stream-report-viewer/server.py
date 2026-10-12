"""Loopback report links to the native Tracy stream renderer. No trace parsing."""
from __future__ import annotations
import argparse
import hashlib
import html
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import threading
import time
import uuid
import gzip
from urllib.parse import urlsplit, unquote, parse_qs
from frame_channel import Frames

VERSION='stream-report-2.1'
ID=re.compile(r'^[a-zA-Z0-9_-]{1,80}$')

def read(path): return json.loads(Path(path).read_text(encoding='utf-8-sig'))
def sha(path):
    with Path(path).open('rb') as source:return hashlib.file_digest(source,'sha256').hexdigest()
def atomic(path,value):
    path=Path(path);path.parent.mkdir(parents=True,exist_ok=True)
    tmp=path.with_suffix('.'+uuid.uuid4().hex+'.tmp')
    tmp.write_text(json.dumps(value,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    for attempt in range(30):
        try:os.replace(tmp,path);break
        except PermissionError:
            if attempt==29:raise
            time.sleep(.001)

class Broker:
    def __init__(self,config,state_dir):
        self.config_path=Path(config).resolve();self.config=read(config)
        if self.config.get('schema_version')!=1:raise ValueError('Unsupported registry')
        self.capture=Path(self.config['capture']['path']).resolve()
        self.exe=Path(self.config['profiler']['path']).resolve()
        if not self.capture.is_file() or self.capture.suffix!='.tracy-stream':raise ValueError('Expected registered stream')
        if not self.exe.is_file() or sha(self.exe)!=self.config['profiler']['sha256']:raise ValueError('Profiler binary identity mismatch')
        self.capture_stat=(self.capture.stat().st_size,self.capture.stat().st_mtime_ns)
        self.views={}
        for view in self.config['views']:
            ident=view['id']
            if not ID.fullmatch(ident) or ident in self.views:raise ValueError('Invalid/duplicate view ID')
            loc=view['location']
            if int(loc['view_begin_ns'])>=int(loc['view_end_ns']):raise ValueError('Invalid view range')
            self.views[ident]=view
        self.directory=Path(state_dir).resolve();self.directory.mkdir(parents=True,exist_ok=True)
        self.channel=self.directory/('channel-'+uuid.uuid4().hex);self.channel.mkdir(exist_ok=True)
        self.artifacts=self.directory/'views';self.artifacts.mkdir(exist_ok=True)
        self.process=None;self.lock=threading.Lock();self.token=uuid.uuid4().hex
        self.sequence=time.time_ns()//1000;self.current=None;self.saved_sequence=None;self.log=None
        self.web_sequence=time.time_ns()//1000;self.web_saved=None;self.compressed_frame=None
        self.input_history=[];self.frame_condition=threading.Condition();self.frames=Frames(self.channel)
        self.frame_error=None;self.frame_at=0;self.ready_seen_at=0
        self.watcher=threading.Thread(target=self.watch,daemon=True);self.watcher.start()
        self.frame_watcher=threading.Thread(target=self.watch_frames,daemon=True);self.frame_watcher.start()

    def watch_frames(self):
        while True:
            try:
                frame=self.frames.read()
                if frame:
                    state,png=frame
                    if self.current and state['navigation']==self.current['sequence']:
                        state.update({'view_id':self.current['view_id'],'source_sha256':self.config['capture']['sha256']})
                        with self.frame_condition:
                            self.compressed_frame=(state,png);self.frame_at=time.monotonic();self.frame_error=None;self.frame_condition.notify_all()
            except FileNotFoundError:pass # mapping descriptor appears during startup
            except (OSError,ValueError) as error:self.frame_error=str(error)
            time.sleep(.003)

    def wait_frame(self,after,view,navigation=None,timeout=1):
        with self.frame_condition:
            def matches():
                return self.compressed_frame and self.compressed_frame[0]['frame']!=after and self.compressed_frame[0]['view_id']==view and (navigation is None or self.compressed_frame[0]['navigation']==navigation)
            self.frame_condition.wait_for(matches,timeout)
            frame=self.compressed_frame
            if matches():return frame
            return None

    def watch(self):
        while True:
            try:
                value=read(self.channel/'state.json')
                seq=value.get('sequence');ident=value.get('view_id')
                if (value.get('phase')=='ready' and value.get('ok') and ident in self.views and seq!=self.saved_sequence
                        and value.get('source_sha256')==self.config['capture']['sha256']
                        and str(value.get('revision'))==str(self.config['capture']['revision'])):
                    image=value.get('image_file')
                    if image:
                        if Path(image).name!=image or not re.fullmatch(r'view-[0-9]+\.png',image):raise ValueError('Unsafe image filename')
                        source=self.channel/image
                        target=self.artifacts/(ident+'.png')
                        shutil.copy2(source,target)
                        value['saved_image']=target.name;value['image_sha256']=sha(target)
                    value['profiler_sha256']=self.config['profiler']['sha256']
                    atomic(self.artifacts/(ident+'.json'),value)
                    self.saved_sequence=seq
            except (OSError,ValueError,KeyError):pass
            time.sleep(.1)

    def open(self,ident,action='open'):
        if ident not in self.views:raise ValueError('Unregistered view')
        if action not in {'open','details','zoom_in','zoom_out','pan_left','pan_right'}:raise ValueError('Unsupported action')
        with self.lock:
            if (self.capture.stat().st_size,self.capture.stat().st_mtime_ns)!=self.capture_stat:raise ValueError('Capture changed since registration')
            previous=self.status()
            # Opening the same report in two browsers must not cancel a load.
            if action=='open' and self.current and self.current['view_id']==ident and previous.get('phase') in {'starting','loading','rendering','capturing'}:
                return self.current
            location=dict(self.views[ident]['location'])
            if action in {'zoom_in','zoom_out','pan_left','pan_right'}:
                previous=self.status()
                if previous.get('view_id')!=ident or previous.get('phase')!='ready':raise ValueError('Open this view first')
                begin=int(previous['actual_view_begin_ns']);end=int(previous['actual_view_end_ns']);span=end-begin
                if action=='zoom_in': begin+=span//8;end-=span//8
                elif action=='zoom_out':begin-=span//6;end+=span//6
                elif action=='pan_left':begin-=span//12;end-=span//12
                else:begin+=span//12;end+=span//12
                begin=max(int(self.config['capture']['begin_ns']),begin)
                end=min(int(self.config['capture']['end_ns']),end)
                location['view_begin_ns']=str(begin);location['view_end_ns']=str(end)
            self.sequence+=1
            self.input_history=[]
            self.ready_seen_at=0
            request={**location,'sequence':str(self.sequence),'view_id':ident,
                     'source_sha256':self.config['capture']['sha256'],'revision':str(self.config['capture']['revision']),
                     'screenshot':True,'show_event':action=='details'}
            atomic(self.channel/'request.json',request)
            self.current={'sequence':str(self.sequence),'view_id':ident,'phase':'starting'}
            if self.process is None or self.process.poll() is not None:
                self.log=(self.directory/f'profiler-{self.sequence}.log').open('wb')
                # The native ImGui renderer is embedded through this local web UI.
                self.process=subprocess.Popen([str(self.exe),'--stream',str(self.capture),'--report-channel',str(self.channel),'--report-web','1'],
                                               stdout=self.log,stderr=subprocess.STDOUT)
            return self.current

    def input(self,ident,body):
        with self.lock:
            state=self.status()
            if state.get('view_id')!=ident or state.get('phase')!='ready':raise ValueError('Open this view and wait until ready')
            if body.get('navigation') and body['navigation']!=state.get('sequence'):raise ValueError('Another tab moved this view; return to the report position')
            events=body.get('events')
            if not isinstance(events,list) or not 1<=len(events)<=64:raise ValueError('Invalid input batch')
            for event in events:
                if event.get('type') not in {'move','button','wheel','refresh'}:raise ValueError('Unsupported input')
                for coord in ['x','y']:
                    if coord in event and (not isinstance(event[coord],(int,float)) or not -100<=event[coord]<=8192):raise ValueError('Invalid position')
                if event['type']=='button' and (event.get('button') not in (0,1,2) or not isinstance(event.get('down'),bool)):raise ValueError('Invalid button')
                if event['type']=='wheel' and (not isinstance(event.get('delta'),(int,float)) or not -8<=event['delta']<=8):raise ValueError('Invalid wheel')
            ack=int(self.compressed_frame[0]['sequence']) if self.compressed_frame else 0
            self.input_history=[e for e in self.input_history if int(e['seq'])>ack]
            for event in events:
                self.web_sequence+=1
                item={**event,'seq':str(self.web_sequence)}
                if self.input_history and item['type']=='move' and self.input_history[-1]['type']=='move':self.input_history[-1]=item
                else:self.input_history.append(item)
            if len(self.input_history)>128:
                self.input_history=[e for e in self.input_history if e['type']=='button']+self.input_history[-1:]
                if len(self.input_history)>128:raise ValueError('Too many unacknowledged button transitions')
            value={'view_id':ident,'sequence':str(self.web_sequence),'events':self.input_history}
            atomic(self.channel/'input.json',value)
            return {'sequence':value['sequence'],'phase':'queued'}

    def live(self):
        state=self.status()
        if self.compressed_frame and self.compressed_frame[0]['view_id']==state.get('view_id') and self.compressed_frame[0]['navigation']==state.get('sequence'):
            return self.compressed_frame[0]
        return {'phase':state.get('phase'),'view_id':state.get('view_id')}

    def status(self):
        if self.process is not None and self.process.poll() is not None:
            return {**(self.current or {}),'phase':'failed','ok':False,'error':'Native Profiler exited','exit_code':self.process.returncode}
        try:
            state=read(self.channel/'state.json')
            if self.current and state.get('sequence')!=self.current['sequence']:return self.current
            state['profiler_pid']=self.process.pid if self.process else None
            frame_ok=bool(self.compressed_frame and self.compressed_frame[0]['navigation']==state.get('sequence'))
            state['frame_channel_ready']=frame_ok
            if state.get('phase')=='ready':
                if not self.ready_seen_at:self.ready_seen_at=time.monotonic()
                if not frame_ok and time.monotonic()-self.ready_seen_at>5:
                    state['frame_channel_error']=self.frame_error or '定位已完成，但没有收到本次导航的实时画面。请重连；查看器画面通道需要检查。'
            if self.frame_error:state['frame_channel_error']=self.frame_error
            return state
        except (OSError,ValueError):return self.current or {'phase':'idle'}

    def page(self,ident=None):
        if ident:
            template=Path(__file__).with_name('viewer.html').read_text(encoding='utf-8')
            script=Path(__file__).with_name('viewer.js').read_text(encoding='utf-8')
            return template.replace('__TITLE__',html.escape(self.views[ident]['title'])).replace('__NOTE__',html.escape(self.views[ident].get('note',''))).replace('__ID__',json.dumps(ident)).replace('__TOKEN__',json.dumps(self.token)).replace('__SCRIPT__',script).encode()
        body='<h1>Tracy 问题回查</h1><p>点击问题，在网页内操作原生 Tracy 时间线。直接读取原始 stream；无需转换。</p><ul>'
        body+=''.join(f'<li><a href="/open/{v["id"]}">{html.escape(v["title"])}</a> — {html.escape(v.get("note",""))}<br><a href="/open/{v["id"]}"><img loading="lazy" style="width:560px" src="/images/{v["id"]}.png" alt="{html.escape(v["title"])} 原生预览"></a></li>' for v in self.views.values())+'</ul>'
        script=''
        return ('<!doctype html><meta charset="utf-8"><title>Tracy 报告回查</title><style>body{font-family:Segoe UI,Arial;margin:28px;background:#151918;color:#e8eeea}a{color:#88cafa}li{margin:12px 0}button{padding:9px;margin:4px;cursor:pointer}pre{white-space:pre-wrap;font-size:12px}img{max-width:100%;border:1px solid #536057}</style>'+body+'<script>'+script+'</script>').encode('utf-8')

def serve(broker,port):
    class Handler(BaseHTTPRequestHandler):
        def allowed(self):
            host=self.headers.get('Host','')
            if host!=f'127.0.0.1:{self.server.server_port}':return False
            origin=self.headers.get('Origin')
            return not origin or origin=='http://'+host
        def send(self,status,data,mime='application/json',compressed=False):
            if not isinstance(data,bytes):data=json.dumps(data,ensure_ascii=False).encode('utf-8')
            self.send_response(status);self.send_header('Content-Type',mime);self.send_header('Content-Length',str(len(data)))
            self.send_header('Cache-Control','no-store');self.send_header('X-Content-Type-Options','nosniff')
            if compressed:self.send_header('Content-Encoding','gzip')
            try:self.end_headers();self.wfile.write(data)
            except ConnectionError:pass
        def do_GET(self):
            if not self.allowed():return self.send(403,{'error':'local origin only'})
            path=unquote(urlsplit(self.path).path)
            if path=='/':return self.send(200,broker.page(),'text/html; charset=utf-8')
            if path=='/api/capabilities':return self.send(200,{'api_version':VERSION,'native_stream':True,'frame_navigation':True,'embedded_native_canvas':True,'mouse_input':True,'registered_views':list(broker.views),'registry_sha256':sha(broker.config_path),'profiler_sha256':broker.config['profiler']['sha256']})
            if path=='/api/status':return self.send(200,broker.status())
            if path=='/api/live':return self.send(200,broker.live())
            if path=='/native-frame.png' and broker.compressed_frame:return self.send(200,broker.compressed_frame[1],'image/png')
            if path=='/api/frame':
                params=parse_qs(urlsplit(self.path).query)
                navigation=params.get('navigation',[None])[0]
                if navigation and broker.current and navigation!=broker.current['sequence']:
                    return self.send(409,{'error':'另一个页面已切换视图，点击“回到报告位置”继续。'})
                frame=broker.wait_frame(params.get('after',['0'])[0],params.get('view',[''])[0],navigation)
                if not frame:return self.send(204,b'')
                state,png=frame
                self.send_response(200);self.send_header('Content-Type','image/png');self.send_header('Content-Length',str(len(png)))
                self.send_header('X-Tracy-State',json.dumps(state,ensure_ascii=True));self.send_header('Cache-Control','no-store');self.end_headers()
                try:self.wfile.write(png)
                except ConnectionError:pass
                return
            if path.startswith('/open/') and path[6:] in broker.views:return self.send(200,broker.page(path[6:]),'text/html; charset=utf-8')
            if path.startswith('/images/'):
                name=path[8:]
                if name.endswith('.png') and name[:-4] in broker.views:
                    image=broker.artifacts/name
                    if image.is_file():return self.send(200,image.read_bytes(),'image/png')
            return self.send(404,{'error':'unregistered resource'})
        def do_POST(self):
            if not self.allowed() or self.headers.get('X-Tracy-Token')!=broker.token:return self.send(403,{'error':'invalid local request'})
            path=unquote(urlsplit(self.path).path)
            if not path.startswith(('/api/open/','/api/input/')):return self.send(404,{'error':'unknown operation'})
            try:
                length=int(self.headers.get('Content-Length','0'))
                if not 0<length<=16384:raise ValueError('invalid request size')
                body=json.loads(self.rfile.read(length))
                if path.startswith('/api/input/'):return self.send(200,broker.input(path[11:],body))
                return self.send(200,broker.open(path[10:],body.get('action','open')))
            except (OSError,ValueError,KeyError) as error:return self.send(400,{'error':str(error)})
        def log_message(self,*args):pass
    server=ThreadingHTTPServer(('127.0.0.1',port),Handler)
    return server

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config',required=True);parser.add_argument('--state-dir',required=True);parser.add_argument('--port',type=int,default=0)
    args=parser.parse_args();broker=Broker(args.config,args.state_dir);server=serve(broker,args.port)
    state={'url':f'http://127.0.0.1:{server.server_port}','pid':os.getpid(),'config':str(broker.config_path),'api_version':VERSION}
    atomic(broker.directory/'server-state.json',state);print(json.dumps(state),flush=True)
    server.serve_forever()

if __name__=='__main__':main()
