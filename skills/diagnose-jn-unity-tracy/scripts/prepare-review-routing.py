#!/usr/bin/env python3
"""Create an unapproved review skeleton from saved Query candidates."""
import argparse
from pathlib import Path
from candidate_manifest import load_candidate_manifest
from report_common import write_json
from review_routing import make_routing


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--candidates', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.output.exists():
        parser.error('output exists; preserve previous screening and tester decisions')
    manifest = load_candidate_manifest(args.candidates)
    if manifest.get('policy_algorithm') != 'candidate-policy-v3':
        parser.error('candidate-policy-v3 is required')
    write_json(args.output, make_routing(manifest))


if __name__ == '__main__':
    main()
