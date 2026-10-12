"""Start/reuse the loopback link service, retaining the report's port."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import time
from urllib.request import urlopen
from server import read, sha, VERSION

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--config',required=True);p.add_argument('--state-dir',required=True);p.add_argument('--port',type=int,default=0);a=p.parse_args()
    directory=Path(a.state_dir).resolve();directory.mkdir(parents=True,exist_ok=True)
    config=Path(a.config).resolve();port=a.port;state=directory/'server-state.json'
    if state.exists():
        old=read(state)
        if Path(old['config']).resolve()!=config:raise ValueError('State directory belongs to another registry')
        try:
            with urlopen(old['url']+'/api/capabilities',timeout=2) as r:cap=json.load(r)
            if cap.get('registry_sha256')!=sha(config):raise ValueError('Running service registry differs')
            if cap.get('api_version')!=VERSION:raise ValueError('Running viewer protocol is older; restart its owned service first')
            print(old['url']);return
        except OSError:port=int(old['url'].rsplit(':',1)[1])
    out=(directory/'service.stdout.log').open('ab');err=(directory/'service.stderr.log').open('ab')
    flags=(subprocess.CREATE_NO_WINDOW|subprocess.CREATE_NEW_PROCESS_GROUP) if os.name=='nt' else 0
    process=subprocess.Popen([sys.executable,'-X','utf8','-u',str(Path(__file__).with_name('server.py')),'--config',str(config),'--state-dir',str(directory),'--port',str(port)],stdout=out,stderr=err,creationflags=flags,close_fds=True)
    for _ in range(100):
        if process.poll() is not None:raise RuntimeError('Service failed; see service.stderr.log')
        if state.exists():
            value=read(state)
            if value['pid']==process.pid:print(value['url']);return
        time.sleep(.1)
    raise TimeoutError('Service startup timed out; see logs')

if __name__=='__main__':main()
