"""Refresh only generated viewer Markdown links when the local service port changes."""
import argparse,json,re
from pathlib import Path
def main():
 p=argparse.ArgumentParser();p.add_argument('--state',type=Path,required=True);p.add_argument('--report-dir',type=Path,required=True);a=p.parse_args()
 state=json.loads(a.state.read_text(encoding='utf-8'));url=state['url'];allowed=set(state['views']);count=0
 if not re.fullmatch(r'http://127\.0\.0\.1:[0-9]+',url):raise ValueError('Not a local service URL')
 for f in a.report_dir.glob('*.md'):
  text=f.read_text(encoding='utf-8');marker=re.match(r'<!-- tracy-local-report-view:([A-Za-z0-9_-]+) -->',text)
  if not marker or marker[1] not in allowed:continue
  text=re.sub(r'http://127\.0\.0\.1:[0-9]+/open/'+re.escape(marker[1])+r'(?=\))',url+'/open/'+marker[1],text)
  f.write_text(text,encoding='utf-8');count+=1
 print(json.dumps({'updated_reading_pages':count,'url':url}))
if __name__=='__main__':main()
