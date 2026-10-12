"""Run native stream analysis and audit saved receipts; never parse trace/store events."""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import subprocess
import sys

SKILL = Path(__file__).resolve().parents[1]


def read(path):
    return json.loads(Path(path).read_text(encoding='utf-8-sig'))


def rows(path):
    with Path(path).open(encoding='utf-8-sig') as source:
        for line in source:
            if line.strip():
                yield json.loads(line)


def sha(path):
    with Path(path).open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def require(condition, message):
    if not condition:
        raise ValueError(message)


def write_new(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open('x', encoding='utf-8') as output:
        json.dump(value, output, ensure_ascii=False, indent=2)
        output.write('\n')


def capture_path(value):
    path = Path(value).resolve()
    if path.is_dir():
        candidates = list(path.glob('*.tracy-stream'))
        require(len(candidates) == 1, 'Capture directory must contain exactly one .tracy-stream; specify the file otherwise')
        path = candidates[0]
    require(path.is_file() and path.suffix == '.tracy-stream', 'Input must be an existing .tracy-stream')
    require(Path(str(path) + '.store').is_dir(), 'Published EventStore directory is required; do not convert the full trace')
    return path


def audit(run):
    run = Path(run).resolve()
    manifest = read(run/'manifest.json')
    require(manifest.get('complete') is True, 'Native run incomplete; preserve failures for inspection')
    source = manifest['source']
    require(source.get('schema') == 17, 'Unverified EventStore schema')
    names = set()
    for item in manifest['files']:
        name = item['path']
        require(Path(name).name == name and '/' not in name and '\\' not in name and ':' not in name,
                'Manifest file must be a local basename')
        require(name not in names, 'Duplicate manifest path: '+name)
        names.add(name)
        path = run/name
        require(path.stat().st_size == item['bytes'] and sha(path) == item['sha256'], 'Output checksum mismatch: '+name)
    pages_checked = 0
    for req in manifest.get('requests', []):
        index = req['index']
        path = run/f'request-{index}.json'
        prior_cursor, cursors, entity_ids = None, set(), []
        page_number = 0
        first = read(path)
        method = first['request']['method']
        while True:
            require(path.name in names, 'Query receipt absent from hash manifest')
            receipt = read(path)
            if page_number:
                require(receipt['request']['params'].get('cursor') == prior_cursor, 'Broken page cursor chain')
            response = receipt['response']
            require(response.get('ok') is True and not response.get('partial', False), 'Failed/partial query: '+path.name)
            require(response.get('schema_version') == '1.35.0', 'Unverified Query schema')
            identity = response['trace']
            require(identity['fingerprint'] == source['source_sha256'] and str(identity['revision']) == str(source['revision']),
                    'Query source/revision mismatch')
            page = response.get('page', {})
            require(not page.get('partial', False), 'Partial page: '+path.name)
            data = response.get('data', {})
            key = 'children' if method in ('zone.cpu.tree', 'zone.gpu.tree') else 'plots' if method == 'plot.list' else None
            if key:
                entity_ids.extend(x['ref'] for x in data.get(key, []))
            cursor = page.get('next_cursor')
            pages_checked += 1
            if cursor is None:
                require(not page.get('truncated', False), 'Truncated terminal page')
                break
            require(cursor and cursor not in cursors, 'Empty/repeated continuation cursor')
            cursors.add(cursor)
            prior_cursor = cursor
            page_number += 1
            path = run/f'request-{index}-page-{page_number}.json'
        require(len(entity_ids) == len(set(entity_ids)), 'Duplicate paged entities')
        if method in ('zone.cpu.tree', 'zone.gpu.tree'):
            require(len(entity_ids) == first['response']['data']['root']['child_count'], 'Incomplete child enumeration')
    result = {'schema':'stream-analysis-audit/v1', 'validation':'passed',
              'scope':'Saved native outputs only; no trace parsing or performance aggregation',
              'manifest_sha256':sha(run/'manifest.json'), 'source':source,
              'verified_files':len(names), 'verified_query_pages':pages_checked,
              'causal_classification_complete':manifest.get('causal_classification_complete', False),
              'all_domain_coverage_complete':manifest.get('all_domain_coverage_complete', False)}
    if not manifest.get('cpu_gpu_scanned'):
        result.update({'analysis_scope':'enrichment_or_shortlist','full_triage_verified':False})
        return result
    for name in ['triage.json','frames.ndjson','fast-frames.ndjson','candidates.ndjson','native-candidates.ndjson']:
        require(name in names, 'Missing full-triage artifact: '+name)
    frames = list(rows(run/'frames.ndjson'))
    fast_all = list(rows(run/'fast-frames.ndjson'))
    fast = [f for f in fast_all if f.get('quality') == 'complete']
    tail = [f for f in fast_all if f.get('quality') == 'open_tail']
    fast_summary = read(run/'fast-summary.json')
    require(len(fast) == fast_summary['complete_frames'] and len(tail) == fast_summary['open_tail'],
            'Fast complete/tail accounting mismatch')
    require(len(fast_all) == len(fast) + len(tail), 'Unexpected invalid/unknown fast frame quality')
    require(all(f.get('end_ns') is None for f in tail), 'Open tail must not have a fabricated end')
    triage = read(run/'triage.json')
    require(len(frames) == len(fast) == triage['frame_set']['complete_frames'], 'Frame count mismatch')
    expected, reasons, indices, severe = {}, Counter(), set(), []
    for item, short in zip(frames, fast):
        require(item['index'] not in indices, 'Duplicate frame index')
        indices.add(item['index'])
        for key in ['index','start_ns','end_ns','duration_ns']:
            require(item[key] == short[key], 'Fast/full frame boundary mismatch')
        duration = int(item['duration_ns'])
        require(duration == int(item['end_ns']) - int(item['start_ns']) and duration > 0, 'Invalid duration')
        require(0 <= int(item['gpu_union_ns']) <= duration and 0 <= int(item['gpu_copy_union_ns']) <= duration,
                'GPU union outside frame bounds')
        require(bool(item['selected']) == bool(item['reasons']), 'Candidate selection/reason mismatch')
        if item['selected']:
            require(item['candidate_id'] not in expected, 'Duplicate candidate identity')
            expected[item['candidate_id']] = item['reasons']
            reasons.update(item['reasons'])
        if item['severe']:
            severe.append(item)
            require(item['selected'] and 'severe_frame' in item['reasons'], 'Severe frame omitted from candidates')
    candidates = list(rows(run/'candidates.ndjson'))
    require(len(candidates) == len(expected) == triage['selection']['candidate_frames'], 'Candidate count mismatch')
    require({c['candidate_id']:c['reasons'] for c in candidates} == expected, 'Candidate identities/reasons differ')
    require(dict(reasons) == triage['selection']['reason_counts'], 'Native reason audit mismatch')
    require(len(severe) == triage['selection']['severe_frames'], 'Severe count mismatch')
    native = list(rows(run/'native-candidates.ndjson'))
    require(len({x['candidate_id'] for x in native}) == len(native) == triage['native_candidates_written'], 'Native candidate identity mismatch')
    require(manifest.get('recorded_candidate_enumeration_complete') is True, 'Native enumeration incomplete')
    # A window covers a frame only if its CPU and GPU outputs actually exist and were hash-verified.
    windows = triage.get('recommended_windows', [])
    valid_windows = [w for i,w in enumerate(windows) if f'window-{i}-cpu.json' in names and f'window-{i}-gpu.json' in names]
    missing = [f['index'] for f in severe if not any(int(w['start_ns']) <= int(f['start_ns']) and
               int(w['end_ns']) >= int(f['end_ns']) for w in valid_windows)]
    result.update({'analysis_scope':'full_native_triage','full_triage_verified':True,'frames_verified':len(frames),
                   'candidate_id_reason_sets_verified':len(expected),'native_candidates_verified':len(native),
                   'severe_candidates_verified':len(severe),'severe_without_cpu_gpu_window':missing,
                   'severe_window_coverage_complete':not missing,'excluded_open_tail_verified':len(tail),
                   'manual_causal_investigation_complete':False})
    return result


def evidence_plans(run, output, all_severe=False):
    run, output = Path(run), Path(output)
    result = audit(run)
    require(result.get('full_triage_verified'), 'Evidence planning requires full triage')
    require(not output.exists(), 'Plan directory must be new')
    output.mkdir(parents=True)
    missing = set(result['severe_without_cpu_gpu_window'])
    windows = [{'start_ns':f['start_ns'],'end_ns':f['end_ns'],'cpu':True,'gpu':True,
                'export_events':False,'frame_index':f['index']} for f in rows(run/'frames.ndjson')
               if f['severe'] and (all_severe or f['index'] in missing)]
    requests, refs = [], set()
    for c in rows(run/'native-candidates.ndjson'):
        ref = c.get('zone_ref')
        if ref and ref not in refs:
            requests.append({'method':'zone.cpu.tree','params':{'ref':ref,'limit':1000}})
            refs.add(ref)
        if c.get('kind') == 'gpu-root-span':
            windows.append({'start_ns':c['start_ns'],'end_ns':c['end_ns'],'cpu':True,'gpu':True,
                            'native_candidate_id':c['candidate_id']})
    count = max((len(windows)+63)//64, (len(requests)+63)//64, 1)
    paths = []
    for i in range(count):
        plan = output/f'plan-{i:03}.json'
        write_new(plan, {'windows':windows[i*64:(i+1)*64], 'requests':requests[i*64:(i+1)*64]})
        paths.append(plan.name)
    ledger = {'schema':'stream-evidence-plan/v1','source_audit':result,'all_severe':all_severe,
              'windows':len(windows),'requests':len(requests),'plans':paths}
    write_new(output/'plan-manifest.json',ledger)
    return ledger


def run_native(args):
    local_path = SKILL/'config/stream-local.json'
    local = read(local_path) if local_path.exists() else {}
    exe_value = args.exe or local.get('executable')
    require(exe_value, 'Supply --exe or config/stream-local.json with a verified analyzer')
    exe = Path(exe_value).resolve()
    require(exe.is_file(), 'Analyzer does not exist: '+str(exe))
    expected = args.expected_sha256 or local.get('expected_sha256')
    actual = sha(exe)
    require(expected and actual.lower() == expected.lower(), 'Verified EXE SHA256 is required and must match')
    source = capture_path(args.capture)
    output = Path(args.output).resolve()
    require(not output.exists(), 'Output directory must be new')
    policy = read(SKILL/'config/stream-profile.json')
    for key,arg in [('target_fps','fps'),('vram_limit_bytes','vram_limit_bytes'),('max_representatives','max_representatives'),('workers','workers')]:
        if getattr(args,arg) is not None:
            policy[key] = getattr(args,arg)
    require(0 < policy['target_fps'] <= 1000 and 1 <= policy['max_representatives'] <= 64 and
            1 <= policy['workers'] <= 64 and policy['vram_limit_bytes'] > 0, 'Invalid stream policy')
    output.mkdir(parents=True)
    write_new(output/'requested-policy.json',policy)
    plan = Path(args.plan).resolve() if args.plan else output/'initial-plan.json'
    if not args.plan:
        write_new(plan, {'requests':[{'method':'trace.info','params':{}},
           {'method':'gpu.vram.summary','params':{'buckets':20}},
           {'method':'plot.list','params':{'limit':1000}}, {'method':'runtime.script.summary','params':{}}]})
    # The native diagnose mode requires explicit windows when triage is skipped;
    # triage mode accepts a requests-only enrichment plan as well as explicit windows.
    mode = 'triage' if args.skip_triage else 'diagnose'
    command = [str(exe),'--trace',str(source),'--output-dir',str(output/'native-run'),'--mode',mode,
               '--fps',str(policy['target_fps']),'--vram-limit-bytes',str(policy['vram_limit_bytes']),
               '--max-representatives',str(policy['max_representatives']),'--workers',str(policy['workers']),
               '--plan',str(plan)]
    if args.no_cache: command.append('--no-cache')
    if args.skip_triage: command.append('--skip-triage')
    write_new(output/'invocation.json',{'argv':command,'executable_sha256':actual,'plan_sha256':sha(plan)})
    # Stream progress to the caller; no shell expansion and no trace reads in Python.
    result = subprocess.run(command, check=False)
    write_new(output/'process-result.json',{'returncode':result.returncode})
    require(result.returncode == 0, f'Native analysis returned {result.returncode}; inspect saved receipts')
    verified = audit(output/'native-run')
    write_new(output/'audit.json',verified)
    return verified


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    run = sub.add_parser('run')
    run.add_argument('--capture',required=True); run.add_argument('--output',required=True)
    run.add_argument('--exe'); run.add_argument('--expected-sha256'); run.add_argument('--plan')
    run.add_argument('--fps',type=float); run.add_argument('--vram-limit-bytes',type=int)
    run.add_argument('--max-representatives',type=int); run.add_argument('--workers',type=int)
    run.add_argument('--no-cache',action='store_true'); run.add_argument('--skip-triage',action='store_true')
    verify = sub.add_parser('audit'); verify.add_argument('--run',required=True); verify.add_argument('--output',required=True)
    plan = sub.add_parser('evidence-plan'); plan.add_argument('--run',required=True); plan.add_argument('--output',required=True)
    plan.add_argument('--all-severe',action='store_true')
    args = parser.parse_args()
    try:
        if args.command == 'run': result = run_native(args)
        elif args.command == 'audit':
            result = audit(args.run); write_new(args.output,result)
        else: result = evidence_plans(args.run,args.output,args.all_severe)
        print(json.dumps(result,ensure_ascii=False))
    except (ValueError, OSError, KeyError, TypeError) as error:
        print(json.dumps({'validation':'failed','error':str(error)},ensure_ascii=False),file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
