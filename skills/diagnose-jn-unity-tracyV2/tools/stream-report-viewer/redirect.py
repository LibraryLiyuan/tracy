"""Keep published local report links working after a viewer port migration."""
import argparse
import json
import os
from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlsplit
from server import atomic

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--port',required=True,type=int);p.add_argument('--target',required=True);p.add_argument('--state',required=True);a=p.parse_args()
    destination=urlsplit(a.target)
    if destination.scheme!='http' or destination.hostname!='127.0.0.1' or destination.path not in ('','/'):raise ValueError('Only a loopback viewer root is allowed')
    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            if self.headers.get('Host')!=f'127.0.0.1:{a.port}':self.send_error(403);return
            if self.path=='/redirect-status':
                body=json.dumps({'target':a.target,'pid':os.getpid()}).encode();self.send_response(200);self.send_header('Content-Length',str(len(body)));self.end_headers();self.wfile.write(body);return
            self.send_response(307);self.send_header('Location',a.target.rstrip('/')+self.path);self.send_header('Content-Length','0');self.send_header('Cache-Control','no-store');self.end_headers()
        def log_message(self,*args):pass
    server=ThreadingHTTPServer(('127.0.0.1',a.port),Handler)
    atomic(a.state,{'pid':os.getpid(),'url':f'http://127.0.0.1:{a.port}','target':a.target})
    server.serve_forever()

if __name__=='__main__':main()
