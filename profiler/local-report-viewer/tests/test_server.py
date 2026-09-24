import hashlib,json,sys,tempfile,threading,unittest,urllib.request,urllib.error
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from viewer_server import Registry, create_server

class ViewerServerTests(unittest.TestCase):
 def setUp(self):
  self.tmp=tempfile.TemporaryDirectory();self.root=Path(self.tmp.name)
  self.trace=self.root/'sample.tracy';self.trace.write_bytes(b'private-trace-fixture')
  self.build=self.root/'web';self.build.mkdir();(self.build/'index.html').write_text('profiler')
  self.sha=hashlib.sha256(self.trace.read_bytes()).hexdigest()
  self.config={'schema_version':1,'captures':{'capture':{'path':str(self.trace),'sha256':self.sha}},'views':{'sample':{'title':'Sample','capture':'capture','frame_set':'Render.Frame','frame_index':4,'begin_ns':'100','end_ns':'200','threads':[]}}}
 def tearDown(self):self.tmp.cleanup()
 def registry(self):
  p=self.root/'viewer.json';p.write_text(json.dumps(self.config));return Registry(p,self.build)
 def start(self):
  self.server=create_server(self.registry(),port=0);self.thread=threading.Thread(target=self.server.serve_forever,daemon=True);self.thread.start();self.url=f'http://127.0.0.1:{self.server.server_port}'
  self.addCleanup(self.server.server_close);self.addCleanup(self.server.shutdown)
 def get(self,path,headers=None):return urllib.request.urlopen(urllib.request.Request(self.url+path,headers=headers or {}),timeout=3)
 def test_registered_view_has_identity_not_local_path(self):
  self.start()
  with self.get('/api/views/sample') as r:
   d=json.load(r);self.assertEqual(d['capture']['sha256'],self.sha);self.assertEqual(d['location']['frame_index'],4);self.assertNotIn(str(self.trace),json.dumps(d));self.assertEqual(r.headers['Cross-Origin-Opener-Policy'],'same-origin');self.assertEqual(r.headers['Cross-Origin-Embedder-Policy'],'require-corp')
 def test_trace_bytes_are_exact(self):
  self.start()
  with self.get('/api/captures/capture/trace') as r:self.assertEqual(r.read(),self.trace.read_bytes());self.assertEqual(int(r.headers['Content-Length']),len(self.trace.read_bytes()))
 def test_port_is_exclusive(self):
  self.start()
  with self.assertRaises(OSError):create_server(self.registry(),port=self.server.server_port)
 def test_trace_identity_mismatch_rejected(self):
  self.config['captures']['capture']['sha256']='0'*64
  with self.assertRaises(ValueError):self.registry()
 def test_changed_file_is_not_served_under_old_identity(self):
  self.start();self.trace.write_bytes(b'changed')
  with self.assertRaises(urllib.error.HTTPError) as e:self.get('/api/captures/capture/trace')
  self.assertEqual(e.exception.code,409)
 def test_invalid_frame_fields_rejected(self):
  for field,value in [('frame_index',-1),('frame_index',True),('frame_index',1.5),('begin_ns',100),('begin_ns','-1'),('end_ns','100'),('end_ns',str(2**63))]:
   with self.subTest(field=field,value=value):
    old=self.config['views']['sample'][field];self.config['views']['sample'][field]=value
    with self.assertRaises(ValueError):self.registry()
    self.config['views']['sample'][field]=old
 def test_unknown_capture_rejected(self):
  self.config['views']['sample']['capture']='missing'
  with self.assertRaises(ValueError):self.registry()
 def test_unknown_view_and_path_escape_rejected(self):
  self.start()
  for path in ['/api/views/missing','/api/captures/missing/trace','/profiler/%2e%2e/viewer.json','/profiler/%2e%2e%2fviewer.json','/viewer.json']:
   with self.subTest(path=path),self.assertRaises(urllib.error.HTTPError):self.get(path)
 def test_nonlocal_host_and_cross_origin_denied(self):
  self.start()
  for headers in [{'Host':'evil.example'},{'Origin':'https://evil.example'}]:
   with self.subTest(headers=headers),self.assertRaises(urllib.error.HTTPError) as e:self.get('/api/views/sample',headers)
   self.assertEqual(e.exception.code,403)
 def test_capabilities_do_not_advertise_job_selection(self):
  self.start()
  with self.get('/api/capabilities') as r:
   d=json.load(r);self.assertTrue(d['frame_navigation']);self.assertFalse(d['job_object_selection'])
 def test_capability_identifies_registry_and_bundle(self):
  meta=self.build/'viewer-build.json';meta.write_text('{"version":"fixture"}')
  self.start()
  with self.get('/api/capabilities') as r:
   d=json.load(r);self.assertEqual(d['registry_sha256'],hashlib.sha256((self.root/'viewer.json').read_bytes()).hexdigest());self.assertEqual(d['bundle_identity'],hashlib.sha256(meta.read_bytes()).hexdigest())
 def test_markdown_handoff_is_html_not_screenshot(self):
  self.start()
  with self.get('/open/sample') as r:
   text=r.read().decode();self.assertIn('/profiler/?view=sample',text);self.assertNotIn('<img',text)

if __name__=='__main__':unittest.main()
