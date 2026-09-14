#!/usr/bin/env python3
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""Organize Query candidates without changing rank, thresholds or numeric facts."""
import argparse
from collections import Counter
from pathlib import Path
from candidate_manifest import load_candidate_manifest
from performance_review import classify,required,OPAQUE
from report_common import write_json
from review_routing import in_scope, priority

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--candidates',required=True,type=Path)
    parser.add_argument('--output',required=True,type=Path)
    args=parser.parse_args()
    manifest=load_candidate_manifest(args.candidates)
    if manifest.get('policy_algorithm')!='candidate-policy-v3':parser.error('current candidate policy is required')
    views=[];counts=Counter()
    for candidate in manifest['candidates']:
        facet=classify(candidate);counts[(facet['domain'],facet['pattern'])]+=1
        if in_scope(candidate):
            name=candidate.get('structural_signature','')
            views.append({'candidate_id':candidate['candidate_id'],'name':'' if OPAQUE.search(name) else name,'classification':facet,
                'query_priority':candidate['priority'],'analysis_priority':priority(candidate)})
    write_json(args.output,{'schema_version':1,'candidate_views':views,
        'retained_summary':[{'domain':d,'pattern':p,'count':n} for (d,p),n in sorted(counts.items())]})

if __name__=='__main__':main()
