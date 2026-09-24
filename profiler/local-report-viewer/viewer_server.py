"""Local, read-only registry for native Tracy report views. No analysis or Trace parsing."""
from __future__ import annotations
import argparse,errno,hashlib,html,json,mimetypes,re,shutil,sys,os,socket
from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
from pathlib import Path
from urllib.parse import unquote,urlsplit

VERSION='0.1.0'
IDENTIFIER=re.compile(r'^[A-Za-z0-9_-]{1,100}$')
def sha256(path):
 h=hashlib.sha256()
 with path.open('rb') as f:
  for block in iter(lambda:f.read(4*1024*1024),b''):h.update(block)
 return h.hexdigest()
def ns(value):
 if not isinstance(value,str) or not re.fullmatch(r'0|[1-9][0-9]*',value) or int(value)>2**63-1:raise ValueError('nanoseconds must be nonnegative decimal int64 strings')
 return int(value)
class Registry:
 def __init__(self,config_path,web_root):
  self.config_path=Path(config_path).resolve();self.web_root=Path(web_root).resolve()
  config_bytes=self.config_path.read_bytes();self.config_identity=hashlib.sha256(config_bytes).hexdigest()
  build_identity=self.web_root/'viewer-build.json';self.bundle_identity=sha256(build_identity) if build_identity.is_file() else None
  data=json.loads(config_bytes.decode('utf-8-sig'))
  if data.get('schema_version')!=1:raise ValueError('Unsupported registry version')
  if not (self.web_root/'index.html').is_file():raise ValueError('Missing built Profiler index.html')
  self.captures={};self.views={}
  for key,entry in data.get('captures',{}).items():
   if not IDENTIFIER.fullmatch(key):raise ValueError('Invalid capture key')
   path=Path(entry['path']);path=(path if path.is_absolute() else self.config_path.parent/path).resolve()
   if not path.is_file() or path.suffix.lower()!='.tracy':raise ValueError('Capture must be an existing .tracy')
   before=path.stat();identity=sha256(path);after=path.stat()
   if (before.st_size,before.st_mtime_ns)!=(after.st_size,after.st_mtime_ns):raise ValueError('Capture changed during identity verification')
   if identity!=entry['sha256']:raise ValueError('Capture SHA256 mismatch')
   self.captures[key]={'path':path,'sha256':identity,'size':after.st_size,'mtime_ns':after.st_mtime_ns,'name':entry.get('name',path.name)}
  for key,entry in data.get('views',{}).items():
   if not IDENTIFIER.fullmatch(key):raise ValueError('Invalid view key')
   if entry.get('capture') not in self.captures:raise ValueError('Unknown capture')
   if not isinstance(entry.get('frame_set'),str) or not entry['frame_set']:raise ValueError('Missing FrameSet')
   index=entry.get('frame_index')
   if type(index) is not int or index<0 or index>2**31-1:raise ValueError('Invalid frame index')
   if ns(entry['end_ns'])<=ns(entry['begin_ns']):raise ValueError('Invalid frame range')
   threads=entry.get('threads',[])
   if not isinstance(threads,list):raise ValueError('threads must be an array')
   for thread in threads:
    ns(thread['id'])
    if not isinstance(thread.get('name'),str) or not thread['name']:raise ValueError('Thread name required for identity check')
   if entry.get('event'):
    event=entry['event'];ns(event['thread_id'])
    if not isinstance(event.get('name'),str) or not event['name'] or ns(event['end_ns'])<=ns(event['begin_ns']):raise ValueError('Invalid event identity')
   self.views[key]=dict(entry)
 def view(self,key):
  entry=self.views[key];capture=self.captures[entry['capture']]
  location={k:entry[k] for k in ('frame_set','frame_index','begin_ns','end_ns','threads','event') if k in entry}
  return {'schema_version':1,'view_id':key,'title':entry.get('title',key),'bundle_identity':self.bundle_identity,'capture':{'id':entry['capture'],'name':capture['name'],'sha256':capture['sha256'],'bytes':capture['size'],'url':f'/api/captures/{entry["capture"]}/trace'},'location':location}

def create_server(registry,port=8140):
 class LocalHTTPServer(ThreadingHTTPServer):
  allow_reuse_address=False
  def server_bind(self):
   if hasattr(socket,'SO_EXCLUSIVEADDRUSE'):self.socket.setsockopt(socket.SOL_SOCKET,socket.SO_EXCLUSIVEADDRUSE,1)
   super().server_bind()
 class Handler(BaseHTTPRequestHandler):
  def log_message(self,fmt,*args):print(fmt%args,file=sys.stderr,flush=True)
  def do_HEAD(self):self.respond(head=True)
  def do_GET(self):self.respond(head=False)
  def write_headers(self,status,kind,length):
   self.send_response(status);self.send_header('Content-Type',kind);self.send_header('Content-Length',str(length))
   self.send_header('Cross-Origin-Opener-Policy','same-origin');self.send_header('Cross-Origin-Embedder-Policy','require-corp');self.send_header('Cross-Origin-Resource-Policy','same-origin');self.send_header('Cache-Control','no-store');self.send_header('X-Content-Type-Options','nosniff');self.end_headers()
  def send(self,status,data,kind='application/json; charset=utf-8',head=False):
   raw=data if isinstance(data,bytes) else json.dumps(data,ensure_ascii=False).encode();self.write_headers(status,kind,len(raw))
   if not head:self.wfile.write(raw)
  def respond(self,head=False):
   try:
    host=self.headers.get('Host','')
    allowed={f'127.0.0.1:{self.server.server_port}',f'localhost:{self.server.server_port}'}
    origin=self.headers.get('Origin')
    if host not in allowed or (origin and origin not in {'http://'+x for x in allowed}):return self.send(403,{'error':'local_origin_required'},head=head)
    path=unquote(urlsplit(self.path).path)
    if path=='/api/capabilities':return self.send(200,{'schema_version':1,'version':VERSION,'frame_navigation':True,'cpu_event_verification':True,'job_object_selection':False,'trace_transport':'registered_local_files','native_bridge_required':VERSION,'registry_sha256':registry.config_identity,'bundle_identity':registry.bundle_identity},head=head)
    if path.startswith('/api/views/'):
     key=path[len('/api/views/'):]
     if key not in registry.views:return self.send(404,{'error':'unknown_view'},head=head)
     return self.send(200,registry.view(key),head=head)
    if path.startswith('/api/captures/') and path.endswith('/trace'):
     key=path[len('/api/captures/'):-len('/trace')]
     if key not in registry.captures:return self.send(404,{'error':'unknown_capture'},head=head)
     record=registry.captures[key];stat=record['path'].stat()
     if (stat.st_size,stat.st_mtime_ns)!=(record['size'],record['mtime_ns']):return self.send(409,{'error':'capture_changed_restart_required'},head=head)
     return self.file(record['path'],'application/octet-stream',head)
    if path.startswith('/open/'):
     key=path[len('/open/'):]
     if key not in registry.views:return self.send(404,{'error':'unknown_view'},head=head)
     view=registry.view(key);url='/profiler/?view='+key;name='tracy-report-'+view['capture']['sha256']+'-'+(registry.bundle_identity or 'unversioned')
     page=f'''<!doctype html><html lang="zh-CN"><meta charset="utf-8"><title>{html.escape(view['title'])}</title><style>body{{font:18px/1.7 system-ui;max-width:850px;margin:60px auto}}a{{color:#175ab0}}</style><h1>{html.escape(view['title'])}</h1><p id="status">正在打开可交互 Profiler…</p><p><a id="open" href="{url}">在当前标签打开 Profiler</a></p><script>
const viewId={json.dumps(key)}, targetName={json.dumps(name)}, url={json.dumps(url)};
const channel=new BroadcastChannel(targetName);let acknowledged=false;
channel.onmessage=e=>{{if(e.data.view!==viewId)return;if(e.data.type==='accepted'){{acknowledged=true;document.getElementById('status').textContent='正在现有 Profiler 中定位…';}}else if(e.data.type==='completed'){{document.getElementById('status').textContent='已定位到现有 Profiler；若浏览器未切换标签，请选择 Profiler 标签，或点击下面链接。';}}else if(e.data.type==='failed'){{document.getElementById('status').textContent='定位失败：'+e.data.error;}}}};
channel.postMessage({{type:'navigate',view:viewId}});
setTimeout(()=>{{if(!acknowledged)location.replace(url);}},600);
</script></html>'''
     return self.send(200,page.encode(),'text/html; charset=utf-8',head)
    if path=='/':
     links=''.join(f'<li><a href="/open/{k}">{html.escape(v.get("title",k))}</a></li>' for k,v in registry.views.items())
     return self.send(200,('<!doctype html><meta charset="utf-8"><title>Tracy 本地报告查看器</title><h1>已登记视图</h1><ul>'+links+'</ul>').encode(),'text/html; charset=utf-8',head)
    if path.startswith('/profiler/'):
     relative=path[len('/profiler/'): ] or 'index.html';target=(registry.web_root/relative).resolve()
     if not target.is_relative_to(registry.web_root) or not target.is_file():return self.send(404,{'error':'not_found'},head=head)
     return self.file(target,mimetypes.guess_type(target.name)[0] or 'application/octet-stream',head)
    return self.send(404,{'error':'not_found'},head=head)
   except (BrokenPipeError,ConnectionResetError):pass
   except (FileNotFoundError,KeyError):self.send(404,{'error':'not_found'},head=head)
  def file(self,path,kind,head):
   with path.open('rb') as f:
    self.write_headers(200,kind,path.stat().st_size)
    if not head:shutil.copyfileobj(f,self.wfile,1024*1024)
 server=LocalHTTPServer(('127.0.0.1',port),Handler);server.daemon_threads=True;return server

def main():
 parser=argparse.ArgumentParser();parser.add_argument('--config',type=Path,required=True);parser.add_argument('--web-root',type=Path,required=True);parser.add_argument('--port',type=int,default=8140);parser.add_argument('--state',type=Path)
 args=parser.parse_args();registry=Registry(args.config,args.web_root)
 try:server=create_server(registry,args.port)
 except OSError as exc:
  if args.port==0 or (exc.errno not in (errno.EADDRINUSE,errno.EACCES) and getattr(exc,'winerror',None) not in (10048,10013)):raise
  server=create_server(registry,0)
 state={'version':VERSION,'pid':os.getpid(),'url':f'http://127.0.0.1:{server.server_port}','config':str(args.config.resolve()),'views':list(registry.views)}
 if args.state:
  args.state.parent.mkdir(parents=True,exist_ok=True);temporary=args.state.with_name(args.state.name+'.tmp');temporary.write_text(json.dumps(state,indent=2),encoding='utf-8');temporary.replace(args.state)
 print(json.dumps(state),flush=True)
 try:server.serve_forever()
 except KeyboardInterrupt:pass
 finally:server.server_close()
if __name__=='__main__':main()
