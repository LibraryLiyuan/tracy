"""Validate handoff navigation and optionally the recorded local source identities."""
from __future__ import annotations
import argparse
import hashlib
import html
import json
import os
import re
import subprocess
from pathlib import Path
from urllib.parse import unquote

def sha(p):
    h=hashlib.sha256()
    with p.open('rb') as f:
        for b in iter(lambda:f.read(1024*1024),b''):h.update(b)
    return h.hexdigest()

def visible(text):
    out=[];fence=None
    for line in text.splitlines():
        m=re.match(r'^\s*(`{3,}|~{3,})',line)
        if m:
            marker=m.group(1)[0]
            if fence is None:fence=marker
            elif marker==fence:fence=None
            continue
        if fence is None:out.append(line)
    return '\n'.join(out), fence is None

def anchors(text):
    out=set(re.findall(r'<(?:a|h[1-6])\b[^>]*\bid=["\']([^"\']+)',text,re.I))
    seen={}
    clean,_=visible(text)
    for m in re.finditer(r'^#{1,6}\s+(.+?)\s*#*$',clean,re.M):
        label=html.unescape(re.sub(r'<[^>]+>','',m.group(1)))
        slug=re.sub(r'[^\w\-\s]','',label.lower()).replace(' ','-')
        num=seen.get(slug,0);seen[slug]=num+1;out.add(slug+(f'-{num}' if num else ''))
    return out

def within(path,root):
    try:path.relative_to(root);return True
    except ValueError:return False

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--doc-root',type=Path,default=Path(__file__).resolve().parents[1])
    p.add_argument('--repo',type=Path)
    p.add_argument('--check-source-hashes',action='store_true')
    p.add_argument('--output',type=Path)
    a=p.parse_args();doc=a.doc_root.resolve();repo=(a.repo or doc.parent).resolve();logical_doc=repo/'doc'
    authored=json.loads((doc/'evidence/authored-pages.json').read_text(encoding='utf-8-sig'))['pages']
    md_files=sorted(doc.rglob('*.md'));errors=[];warnings=[];checked=[];external_urls=0
    link_re=re.compile(r'(!?)\[([^\]]*)\]\((?:<([^>]+)>|([^\)]+))\)')
    for file in md_files:
        rel=file.relative_to(doc).as_posix();text=file.read_text(encoding='utf-8-sig');clean,balanced=visible(text)
        if not balanced:errors.append(dict(kind='unbalanced_fence',file=rel))
        if rel in authored:
            if len(re.findall(r'^#\s+',clean,re.M))!=1:errors.append(dict(kind='h1_count',file=rel))
            if '[src:' in text or '[[HEAD_' in text or '[[BASE_' in text:errors.append(dict(kind='unresolved_authoring_token',file=rel))
        logical_file=logical_doc/rel
        for m in link_re.finditer(clean):
            raw=(m.group(3) or m.group(4)).strip()
            if re.match(r'^(https?://|codex://|mailto:|data:)',raw):external_urls+=1;continue
            target=unquote(raw.replace('\\','/'))
            if re.match(r'^/[A-Za-z]:/',target):target=target[1:]
            bare,_,fragment=target.partition('#')
            suffix=re.search(r':(\d+)$',bare);line=None
            if suffix:line=int(suffix.group(1));bare=bare[:suffix.start()]
            logical=(Path(bare) if Path(bare).is_absolute() else logical_file.parent/bare).resolve() if bare else logical_file
            actual=(doc/logical.relative_to(logical_doc)) if within(logical,logical_doc) else logical
            row=dict(file=rel,target=raw,exists=actual.exists(),internal_doc=within(logical,logical_doc));checked.append(row)
            if not actual.exists():errors.append(dict(kind='missing_link',**row));continue
            if actual.is_file() and (line or re.fullmatch(r'L\d+',fragment)):
                ln=line or int(fragment[1:]);count=len(actual.read_text(encoding='utf-8-sig',errors='replace').splitlines())
                if not 1<=ln<=count:errors.append(dict(kind='line_out_of_range',file=rel,target=raw,lines=count))
            elif fragment and actual.suffix.lower()=='.md':
                content=actual.read_text(encoding='utf-8-sig')
                if fragment not in anchors(content):
                    result=dict(kind='unverified_heading_anchor',file=rel,target=raw)
                    (warnings if rel.startswith('history/') else errors).append(result)
    json_count=0
    for f in doc.rglob('*.json'):
        if f.name in {'document-validation.json','delivery-manifest.json'}:continue
        try:json.loads(f.read_text(encoding='utf-8-sig'));json_count+=1
        except Exception as e:errors.append(dict(kind='invalid_json',file=f.relative_to(doc).as_posix(),error=str(e)))
    source_checks=[]
    if a.check_source_hashes:
        registry=json.loads((doc/'evidence/source-registry.json').read_text(encoding='utf-8-sig'))
        for key,s in registry.items():
            f=Path(s['absolute_path']);matches=f.is_file() and sha(f)==s['sha256'];source_checks.append(dict(key=key,matches=matches))
            if not matches:errors.append(dict(kind='source_changed',key=key,path=str(f)))
            elif s.get('pattern'):
                lines=f.read_text(encoding='utf-8-sig',errors='replace').splitlines()
                if not re.search(s['pattern'],lines[s['line']-1]):errors.append(dict(kind='source_anchor_mismatch',key=key))
    snapshots=json.loads((doc/'evidence/repository-snapshot.json').read_text(encoding='utf-8-sig'))
    heads=[]
    git_exe=Path('C:/Users/Admin/.cache/codex-runtimes/codex-primary-runtime/dependencies/native/git/cmd/git.exe')
    for r in snapshots['repositories']:
        command=[str(git_exe) if git_exe.is_file() else 'git','-c',f"safe.directory={r['root']}",'-C',r['root'],'rev-parse',r['branch']]
        try:
            actual=subprocess.run(command,capture_output=True,text=True,check=True).stdout.strip()
            heads.append(dict(repo=r['name'],expected=r['head'],actual=actual,matches=actual==r['head']))
            if actual!=r['head']:warnings.append(dict(kind='branch_moved',repo=r['name']))
        except Exception as e:warnings.append(dict(kind='git_unavailable',repo=r['name'],error=str(e)))
    result=dict(schema='jntracy-document-validation/v1',date='2026-10-14',passed=not errors,markdown_files=len(md_files),authored_pages=len(authored),archived_markdown=sum(f.relative_to(doc).parts[0]=='history' and f.relative_to(doc).as_posix()!='history/README.zh-CN.md' for f in md_files),local_links_checked=len(checked),external_urls_not_fetched=external_urls,json_files_parsed=json_count,source_hashes_checked=len(source_checks),source_hash_matches=sum(x['matches'] for x in source_checks),git_heads=heads,errors=errors,warnings=warnings)
    output=a.output or doc/'evidence/document-validation.json';output.parent.mkdir(parents=True,exist_ok=True);output.write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(result,ensure_ascii=False,indent=2));return 0 if result['passed'] else 1

if __name__=='__main__':raise SystemExit(main())
