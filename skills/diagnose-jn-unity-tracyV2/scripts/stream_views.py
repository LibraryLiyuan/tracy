"""Run the installed native stream report viewer tools with pinned local paths."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--local-config',type=Path,default=Path(__file__).resolve().parents[1]/'config/stream-viewer-local.json')
    parser.add_argument('operation',choices=['prepare','start','export'])
    args,rest=parser.parse_known_args()
    config=json.loads(args.local_config.read_text(encoding='utf-8-sig'))
    root=Path(config['tool_root']);profiler=Path(config['profiler'])
    with profiler.open('rb') as f:digest=hashlib.file_digest(f,'sha256').hexdigest()
    if digest!=config['profiler_sha256']:raise ValueError('Published Profiler identity mismatch')
    script=root/(args.operation+'.py')
    if not script.is_file():raise FileNotFoundError(script)
    if args.operation=='prepare':rest=['--profiler',str(profiler),*rest]
    subprocess.run([sys.executable,'-X','utf8',str(script),*rest],check=True)

if __name__=='__main__':main()
