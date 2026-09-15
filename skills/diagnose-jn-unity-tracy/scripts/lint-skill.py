#!/usr/bin/env python3
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""Dependency-free package checks; not a semantic proof of performance analysis."""
import ast
import json
from pathlib import Path
import re

def check(root):
    errors=[];skill=(root/'SKILL.md').read_text(encoding='utf-8')
    if not skill.startswith('---\n'):errors.append('SKILL frontmatter missing')
    if not re.search(r'^name: diagnose-jn-unity-tracy$',skill,re.M):errors.append('skill name differs')
    if not re.search(r'^description: Use when ',skill,re.M):errors.append('routing description missing')
    if 'version: 2.3.0' not in skill:errors.append('package version missing')
    for link in re.findall(r'\]\(([^)]+)\)',skill):
        if '://' not in link and not (root/link.split('#')[0]).is_file():errors.append('missing skill reference: '+link)
    for path in (root/'scripts').glob('*.py'):
        try:ast.parse(path.read_text(encoding='utf-8'),filename=str(path))
        except SyntaxError as exc:errors.append(str(exc))
    for path in (root/'schemas').glob('*.json'):
        try:json.loads(path.read_text(encoding='utf-8'))
        except ValueError as exc:errors.append(str(exc))
    for term in ('performance_review','源码必读','如何发现','原因去重','全文可读','legacy-regression'):
        if term not in skill:errors.append('missing workflow contract: '+term)
    return errors

if __name__=='__main__':
    errors=check(Path(__file__).resolve().parents[1])
    print(json.dumps({'passed':not errors,'errors':errors,'scope':'package syntax, links and required contract terms'},ensure_ascii=False))
    raise SystemExit(bool(errors))
