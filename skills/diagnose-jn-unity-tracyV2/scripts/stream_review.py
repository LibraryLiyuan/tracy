"""Index investigation obligations from saved native groups, not performance statistics."""
import argparse
import json
from pathlib import Path
from stream_workflow import audit, read, write_new, require


def checks_for(group, budget_ms):
    name = group['name'].lower()
    owner = group['owner_name'].lower()
    checks = []
    maximum = group['inclusive'].get('max_ms', 0)
    own_maximum = group['self'].get('max_ms', 0)
    if 'worker' in owner and name != 'idle' and maximum >= budget_ms:
        checks.append('inspect_worker_and_dependency_endpoint')
    if own_maximum >= budget_ms and name != 'idle':
        checks.append('separate_execution_internal_wait_and_scheduling')
    if name in {'createresourcewithtag','createcommittedresourcewithtag','bufferd3d12.reinitialize',
                'gfx.uploadtexture','texture2d.apply','d3d12.memory.processreleasequeue'}:
        checks.append('trace_resource_caller_lifetime_and_existing_reuse')
    if 'pso.drivercall' in name:
        checks.append('inspect_collection_parser_subassets_quality_path_and_cache_key')
    if name == 'gfxdeviced3d12.pluginevent.flush':
        checks.append('inspect_exact_plugin_event_id_flags_and_submission')
    if ('waitforjob' in name or 'waitinstanceshaders' in name) and maximum >= budget_ms:
        checks.append('inspect_all_workers_and_producer_completion_relation')
    return checks


def build(run, output):
    run, output = Path(run).resolve(), Path(output).resolve()
    verified = audit(run)
    require(verified.get('full_triage_verified'), 'Full native triage required')
    require(not output.exists(), 'Review output must be new')
    output.mkdir(parents=True)
    manifest = read(run/'manifest.json')
    budget = read(run/'triage.json')['budget_ms']
    found = []
    with (output/'review-obligations.ndjson').open('x',encoding='utf-8') as stream:
        for item in manifest['files']:
            name = item['path']
            if not (name.startswith('window-') and name.endswith('-cpu.json')): continue
            window = read(run/name)
            for index, group in enumerate(window['groups']):
                if not group.get('max_ref'): continue
                checks = checks_for(group,budget)
                if not checks: continue
                value = {'evidence_file':str(run/name),'evidence_pointer':f'/groups/{index}',
                         'window_start_ns':window['start_ns'],'window_end_ns':window['end_ns'],
                         'native_group':group,'required_investigation':checks,
                         'readiness':'scope_located','causal_claim_verified':False}
                stream.write(json.dumps(value,ensure_ascii=False)+'\n')
                found.append((name,index))
    result = {'schema':'stream-review-obligations/v1','verified_native_audit':verified,
              'indexed_evidence_groups':len(found),'index':'review-obligations.ndjson',
              'semantics':'Evidence references and investigation prompts, not new performance candidates, rankings or confirmed causes',
              'all_thread_owners_considered':True,'native_statistics_recomputed':False}
    write_new(output/'review-manifest.json',result)
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run',required=True);parser.add_argument('--output',required=True)
    args = parser.parse_args()
    print(json.dumps(build(args.run,args.output),ensure_ascii=False))
