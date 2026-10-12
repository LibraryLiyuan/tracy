"""Validate the human analysis contract against saved MCP and source artifacts.

This module never scans a Trace, ranks costs, or infers a cause from a name.
Receipt/source integrity is mechanical; explanations and merge decisions remain
reviewable judgments, not assertions that a validator can prove root cause.
"""
from __future__ import annotations

from collections import Counter
import hashlib
import re
from pathlib import Path
import subprocess

from investigation_receipts import _receipt, _pointer, _file
from decimal import Decimal
from review_routing import active as routing_active, validate_routing, deep_required, query_required, in_scope

OPAQUE = re.compile(r"(?i)(?:[0-9a-f]{16,}|\b[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}\b|tracy:v\d+:|(?:candidate|signature|evidence|zone|thread):[^\s]+|CPU[/ ]Unknown|\b(?:EVD|SIG|CAND)-[A-Za-z0-9_-]+)")
READINESS = {'cost_located','mechanism_supported','cause_verified','actionable','rejected','blocked'}
ROLES = {'normal','peak','repeated','sustained','recovery','user_focus'}


def readable(value, field):
    if not isinstance(value, str) or not value.strip():
        raise ValueError(field + ': readable text is required')
    if OPAQUE.search(value):
        raise ValueError(field + ': opaque identity cannot be displayed')
    return value


def classify(candidate):
    """Facets reflect Query facts only; no new ranking or inferred mechanism."""
    domain = candidate.get('domain','unknown')
    manifestation = candidate.get('manifestation','')
    pattern = manifestation.split(':',1)[0] or 'aggregate'
    root = candidate.get('frame_root') or candidate.get('signature_id','').startswith('frame-wall:')
    level = 'frame_window' if root else ('resource_operation' if domain in {'memory','gpu_resource','io'} else 'event')
    cost = 'cpu_wait' if domain == 'cpu' and candidate.get('metric') == 'wait' else {
        'cpu':'cpu_event_cost','gpu':'gpu_execution','job':'job_scheduling','memory':'memory',
        'gpu_resource':'gpu_resource','io':'io','sampling':'sampling','telemetry':'telemetry'}.get(domain,'unclassified')
    return {'level':level,'pattern':pattern,'cost_kind':cost,'domain':domain,
        'frame_scope':candidate.get('frame_scope',''),'thread_or_queue':candidate.get('thread_or_queue',''),
        'selection_reasons':list(candidate.get('triggers',[]))}


def required(candidate):
    return candidate.get('selected') is True or candidate.get('priority') in {'P0','P1'} or 'user_focus' in candidate.get('triggers',[])


def _records(value, identity, field):
    if not isinstance(value,list):
        raise ValueError(field + ': array required')
    result = {}
    for item in value:
        if not isinstance(item,dict) or not isinstance(item.get(identity),str) or not item[identity]:
            raise ValueError(field + ': record identity required')
        if item[identity] in result:
            raise ValueError(field + ': duplicate record')
        result[item[identity]] = item
    return result


def _strings(obj, fields):
    for field in fields:
        readable(obj.get(field),field)


def _refs(values, table, field, minimum=1):
    if not isinstance(values,list) or len(values)<minimum or any(not isinstance(v,str) or v not in table for v in values):
        raise ValueError(field + ': missing or unknown references')
    if len(set(values)) != len(values):
        raise ValueError(field + ': duplicate references')
    return values


def _source_record(record, root, evidence, trace):
    _strings(record,('name','symbol','behavior','conditions','existing_controls','runtime_support'))
    repository = Path(record['repository']).resolve(strict=True)
    relative = Path(record['path'])
    if relative.is_absolute() or '..' in relative.parts or ':' in str(relative):
        raise ValueError('source path must be repository relative')
    snapshot = record['snapshot']
    saved = (root / snapshot['path']).resolve()
    if Path(snapshot['path']).is_absolute() or not saved.is_relative_to(root.resolve()):
        raise ValueError('source snapshot escapes evidence root')
    if saved.stat().st_size > 16*1024*1024:
        raise ValueError('source snapshot exceeds supported file size')
    payload = saved.read_bytes()
    if hashlib.sha256(payload).hexdigest() != snapshot['sha256'].lower():
        raise ValueError('source snapshot checksum mismatch')
    local = (repository / relative).resolve()
    if not local.is_relative_to(repository):
        raise ValueError('source file escapes repository')
    revision = record.get('revision','')
    if not isinstance(revision,str) or not revision:
        raise ValueError('source revision observation is required')
    matches = local.is_file() and local.read_bytes() == payload
    if not matches and re.fullmatch(r'[a-fA-F0-9]{40}|[a-fA-F0-9]{64}',revision):
        git = subprocess.run(['git','-c','safe.directory='+repository.as_posix(),'-C',str(repository),
            'show',revision+':'+relative.as_posix()],capture_output=True,timeout=30)
        matches = git.returncode == 0 and git.stdout == payload
    if not matches:
        raise ValueError('source snapshot has no matching local file or Git object')
    lines = payload.decode('utf-8-sig').splitlines()
    begin,end = record.get('line'),record.get('end_line')
    if type(begin) is not int or type(end) is not int or not 1 <= begin <= end <= len(lines) or end-begin >= 80:
        raise ValueError('source reading range must identify 1 to 80 actual lines')
    if record.get('version_status') not in {'matched','unverified','mismatch'}:
        raise ValueError('source version status is required')
    if record['version_status'] == 'matched':
        from confirmation_evidence import _source
        _source(dict(record,expected_revision=revision,finding=record['behavior']),evidence,root,trace)
    excerpt = '\n'.join(lines[begin-1:end])
    readable(excerpt,'source excerpt')
    readable(str(relative),'source file name')
    return excerpt


def _interval(node):
    gpu = 'gpu_start_ns' in node
    begin,end = node.get('gpu_start_ns' if gpu else 'start_ns'),node.get('gpu_end_ns' if gpu else 'end_ns')
    if begin is None or end is None or node.get('complete') is not True or node.get('timing_valid') is False:
        raise ValueError('event interval is incomplete')
    begin,end = int(begin),int(end)
    if begin >= end:
        raise ValueError('event interval is invalid')
    duration = node.get('gpu_duration_ns' if gpu else 'duration_ns')
    if duration is None or int(duration) != end-begin:
        raise ValueError('event duration must be recorded by Query and match its boundaries')
    for key in ('self_time_ns','running_time_ns'):
        if node.get(key) is not None and not 0 <= int(node[key]) <= int(duration):
            raise ValueError('event component duration is invalid')
    return begin,end,int(duration),gpu


def audit_sample(sample, receipt, events):
    """Keep CPU frames, GPU intervals, OriginFrame and resource snapshots distinct."""
    method,params,content=receipt(sample['frame_evidence_id'])
    frame=_pointer(content,sample.get('frame_pointer','/data'))
    kind=sample.get('kind','frame')
    if not isinstance(frame,dict):raise ValueError('sample anchor is not an object')
    if kind=='frame':
        index=str(frame.get('index',''));scope=frame.get('frame_set_ref',frame.get('frame_scope'))
        if method!='frame.get' or not index or not scope or (str(params.get('index'))!=index and params.get('ref')!=frame.get('ref')):
            raise ValueError('frame request/response identity mismatch')
        known_name=frame.get('frame_set_name',params.get('frame_set'))
        if known_name and not OPAQUE.search(str(known_name)) and sample['frame_name']!=known_name:
            raise ValueError('frame display name differs from the recorded selection')
        begin,end=int(frame['begin_ns']),int(frame['end_ns'])
    elif kind=='gpu_interval':
        if method!='zone.gpu.get' or not frame.get('ref') or params.get('ref')!=frame['ref'] or frame.get('parent_ref') is not None:
            raise ValueError('GPU interval needs an actual L0 get anchor')
        begin,end,_,_=_interval(frame)
    elif kind=='origin_frame':
        if method!='frame.identity' or frame.get('complete') is not True or frame.get('evidence_kind')!='exact' or str(params.get('frame_id'))!=str(frame.get('frame_id')):
            raise ValueError('Job sample requires recorded OriginFrame identity')
        begin,end=int(frame['begin_ns']),int(frame['end_ns'])
    elif kind=='context':
        readable(sample.get('timing_gap'),'operation timing limitation')
        begin=end=None
    else:raise ValueError('sample kind is invalid')
    if begin is not None and begin>=end:raise ValueError('invalid sample boundaries')
    for eid in sample['event_ids']:
        event=events[eid];node=event['node'];interval=event['interval']
        if kind=='context':
            if event['record']['evidence_id']!=sample['frame_evidence_id']:
                raise ValueError('context snapshot must reference the actual inspected operation')
        elif kind=='origin_frame':
            if node.get('origin_frame_ref')!=frame.get('ref') or node.get('origin_frame_sequence')!=frame.get('sequence'):
                raise ValueError('Job operation belongs to a different OriginFrame')
        elif interval is None:raise ValueError('untimed operation cannot claim frame contribution')
        else:
            a,b,_,gpu=interval
            if gpu!=(kind=='gpu_interval'):raise ValueError('sample and event use different clock domains')
            if not a<end or not b>begin:raise ValueError('event does not overlap selected sample')
            if gpu:
                if node.get('context_ref')!=frame.get('context_ref') or not begin<=a<b<=end:
                    raise ValueError('GPU event does not belong to this queue interval')
                current=eid;ancestors=set()
                while current:
                    if current in ancestors:raise ValueError('event parent cycle')
                    ancestors.add(current)
                    if events[current]['node'].get('ref')==frame['ref']:break
                    current=events[current]['record'].get('parent_event_id')
                if not current:raise ValueError('GPU interval overlap is not ancestry')
    return {'record':sample,'frame':frame,'begin':begin,'end':end,'kind':kind}


def validate_review(analysis, candidates, evidence, root):
    """Return errors and an audited rendering model. All public 2.1 paths use it."""
    errors=[]
    model={'events':{},'sources':{},'samples':{},'classifications':{},'pending':[], 'counts':{},'measurements':{},'relations':{}}
    review = analysis.get('performance_review')
    if not isinstance(review,dict) or review.get('schema_version') != 1:
        return ['performance_review schema 1 is required; source/discovery/grouping cannot be skipped'],model
    version = analysis.get('report_versions', {}).get('analysis_workflow')
    if version not in {'2.1.0','2.2.0'} or any(analysis.get('report_versions',{}).get(key)!=version for key in ('analysis_workflow','report_builder','template')):
        return ['performance_review requires matching workflow, report_builder and template version 2.1.0 or 2.2.0'],model
    root=Path(root)
    try:
        findings=_records(analysis.get('candidate_findings',[]),'candidate_id','findings')
        views=_records(review.get('candidate_views'),'candidate_id','candidate views')
        issues=_records(review.get('issues'),'issue_id','issues')
        events=_records(review.get('events'),'event_id','events')
        sources=_records(review.get('sources'),'source_id','sources')
        ev=_records(evidence.get('evidence'),'evidence_id','evidence')
        labels=review.get('evidence_labels')
        if not isinstance(labels,dict):raise ValueError('readable evidence_labels are required')
        for eid,label in labels.items():
            if eid not in ev:raise ValueError('evidence label references missing receipt')
            readable(label,'evidence label')
            if label==eid:raise ValueError('evidence label must not be its internal identity')
        profile_eid=review['profile_evidence_id']
        if profile_eid not in labels:raise ValueError('validated profile needs a readable evidence label')
        raw=ev[profile_eid].get('mcp_receipt',ev[profile_eid])
        request,response=_file(root,raw['request']),_file(root,raw['response'])
        call=request.get('params',{});args=call.get('arguments',{})
        content=response.get('result',{}).get('structuredContent',{});data=content.get('data',{})
        if (request.get('jsonrpc')!='2.0' or request.get('method')!='tools/call' or request.get('id') is None or response.get('id')!=request['id']
                or call.get('name')!='tracy_scan' or args.get('operation')!='profile_validate' or not isinstance(args.get('profile'),dict)
                or content.get('ok') is not True or response.get('result',{}).get('isError') or content.get('partial') or content.get('truncated')
                or data.get('valid') is not True or data.get('profile_identity')!=candidates.get('profile_identity')):
            raise ValueError('frame budget must bind a successful profile_validate for this candidate profile')
        budget=Decimal(str(data['normalized_profile']['frame_budget']['frame_ms']))*1000000
        if not budget.is_finite() or budget<=0:raise ValueError('frame budget must be finite and positive')
        model['frame_budget_ns']=budget
        quality=review.get('quality_notes',[])
        if not isinstance(quality,list) or Counter(n.get('domain') for n in quality)!=Counter(n.get('domain') for n in candidates.get('capture_quality',[])):
            raise ValueError('readable quality notes must preserve every capture quality entry')
        for note in quality:
            readable(note.get('description'),'quality limitation')
            _refs(note.get('evidence_refs',[]),ev,'quality evidence',0)
        trace=evidence['trace']; fingerprint=trace.get('sha256',trace.get('trace_sha256',''))
        # Profile validation is trace-neutral and was audited separately above.
        # It is valid discovery evidence, but never a zone/frame/entity anchor.
        receipts={profile_eid:('analysis.scan.profile.validate',args,content)}
        def receipt(eid):
            if eid not in labels:raise ValueError('referenced evidence needs a readable label')
            if eid not in receipts:receipts[eid]=_receipt(root,ev[eid],trace['trace_id'],fingerprint)
            return receipts[eid]
        def evidence_refs(values,field,minimum=1):
            for eid in _refs(values,ev,field,minimum):receipt(eid)
        routing = None
        if routing_active(analysis):
            routing, routing_counts = validate_routing(analysis, candidates, evidence, root)
            model['routing_counts'] = routing_counts
        totals=Counter();wanted={};required_ids=set()
        for candidate in candidates.get('candidates',[]):
            cid=candidate['candidate_id'];facet=classify(candidate)
            totals[(facet['domain'],facet['pattern'])]+=1
            if (deep_required(candidate,routing) if routing is not None else required(candidate)):required_ids.add(cid)
            if (in_scope(candidate) if routing is not None else required(candidate)) or cid in findings:wanted[cid]=candidate
        if set(views) != set(wanted):raise ValueError('candidate views must cover every required or investigated candidate exactly')
        for cid,candidate in wanted.items():
            readable(views[cid].get('name'),'candidate name')
            if views[cid].get('classification') != classify(candidate):raise ValueError('candidate classification differs from Query facts')
        model['classifications']=views
        model['candidate_facts']=wanted
        model['counts']={'candidate_total':sum(totals.values()),'required_total':len(required_ids),'issue_total':len(issues)}
        model['retained_summary']=[{'domain':d,'pattern':p,'count':n} for (d,p),n in sorted(totals.items())]
        actual_events={}
        for eid,event in events.items():
            _strings(event,('thread_name',))
            method,params,content=receipt(event['evidence_id'])
            node=_pointer(content,event['pointer'])
            is_zone=method in {'zone.cpu.tree','zone.cpu.get','zone.gpu.tree','zone.gpu.get','zone.cpu.search','zone.gpu.search'}
            is_operation=method.startswith(('job.','io.','memory.','gpu.'))
            if not is_zone and not is_operation:
                raise ValueError('event must come from a directed zone or operation query')
            if not isinstance(node,dict) or not node.get('ref') or not node.get('name'):
                raise ValueError('event pointer does not identify a named entity')
            if method.endswith('.get') and params.get('ref') != node['ref']:raise ValueError('event get request does not target returned entity')
            if node['ref'] in actual_events:raise ValueError('same actual event is registered twice; reuse one event record')
            actual_events[node['ref']]=eid
            if event['name']!=node['name']:
                raise ValueError('event display name must preserve the returned event name')
            display=event.get('display_name',event['name'])
            readable(display,'event display name')
            if display!=event['name']:
                basis=event.get('display_name_basis')
                if not (basis=='query_function' and display==node.get('function') or
                        basis=='unresolved_context' and display in {'未解析事件','Unresolved event'}):
                    raise ValueError('display name must use a recorded function or explicit unresolved context')
            interval=_interval(node) if is_zone else None
            if not is_zone:readable(event.get('timing_gap'),'operation duration evidence gap')
            model['events'][eid]={'record':dict(event,name=display),'node':node,'interval':interval}
        for eid,item in model['events'].items():
            seen=set();current=eid
            while current:
                if current in seen:raise ValueError('event parent cycle')
                seen.add(current)
                current=events[current].get('parent_event_id')
                if current and current not in events:raise ValueError('parent event is missing')
            parent=item['record'].get('parent_event_id')
            if parent:
                if parent not in events:raise ValueError('parent event is missing')
                actual_parent=model['events'][parent]
                if item['interval'] is None or actual_parent['interval'] is None:
                    raise ValueError('operation dependency is not a zone parent relation')
                if item['node'].get('parent_ref') != actual_parent['node']['ref']:raise ValueError('event parent link is not recorded')
                a,b,_,gpu=item['interval'];pa,pb,_,pgpu=actual_parent['interval']
                target=lambda node:node.get('context_ref') if gpu else node.get('thread_ref')
                if gpu != pgpu or target(item['node']) != target(actual_parent['node']) or not pa<=a<b<=pb:
                    raise ValueError('parent interval or execution lane differs')
            elif item['node'].get('parent_ref') is not None:
                readable(item['record'].get('ancestry_gap'),'missing ancestor explanation')
        for sid,source in sources.items():
            evidence_refs(source.get('evidence_refs'),'source runtime evidence')
            model['sources'][sid]={'record':source,'excerpt':_source_record(source,root,ev,trace)}
        relations=_records(review.get('relations',[]),'relation_id','relations')
        for rid,relation in relations.items():
            _strings(relation,('name','meaning'))
            _refs([relation['from_event_id'],relation['to_event_id']],events,'relation endpoints',2)
            method,_,content=receipt(relation['evidence_id']);edge=_pointer(content,relation['pointer'])
            if not method.startswith(('relation.','job.','gfx.','timeline.')) or not isinstance(edge,dict):
                raise ValueError('relation must point to a real relation query result')
            if (edge.get('source_ref')!=model['events'][relation['from_event_id']]['node']['ref']
                    or edge.get('target_ref')!=model['events'][relation['to_event_id']]['node']['ref']
                    or edge.get('relation')!=relation.get('relation') or edge.get('evidence_kind')!='exact'):
                raise ValueError('relation direction, kind or exactness differs from original')
            model['relations'][rid]=relation
        ownership={};titles=set();mechanisms={};issue_event_refs={};blocked=set();issue_source_keys={};issue_names={}
        for iid,issue in issues.items():
            _strings(issue,('title','mechanism','trigger','scope','conclusion','source_applicability'))
            if issue['title'] in titles:raise ValueError('duplicate issue title; review causes before rendering')
            titles.add(issue['title'])
            if issue.get('analysis_state') not in {'complete','blocked'} or issue.get('readiness') not in READINESS:
                raise ValueError('explicit depth and readiness states are required')
            if issue['analysis_state']=='complete' and issue['readiness']=='blocked':raise ValueError('blocked issue cannot have complete depth state')
            cids=_refs(issue.get('candidate_ids'),findings,'issue candidates')
            _refs(issue.get('relation_refs',[]),relations,'issue relations',0)
            for cid in cids:
                ownership.setdefault(cid,set()).add(iid)
            if issue['analysis_state']=='complete':
                _refs(issue.get('source_refs'),sources,'deep analysis source reading')
            else:
                blocked.update(cids)
                gap=issue.get('source_blocker')
                if not isinstance(gap,dict):raise ValueError('blocked analysis requires actual source search record')
                _strings(gap,('reason','next_action'))
                if not gap.get('searches'):raise ValueError('blocked source must record searches performed')
                for text in gap['searches']:readable(text,'source search')
                _refs(issue.get('source_refs',[]),sources,'partial source references',0)
            if any(sources[s]['version_status']!='matched' for s in issue.get('source_refs',[])):
                if issue['readiness'] in {'cause_verified','actionable'}:raise ValueError('unmatched source cannot establish capture-specific verified cause')
                if not issue.get('limitations'):raise ValueError('source version limitation must be visible')
            if issue['readiness'] in {'cause_verified','actionable'}:
                verification=issue.get('runtime_verification',{})
                _strings(verification,('claim','reason','counterexample_review'))
                evidence_refs(verification.get('evidence_refs'),'runtime verification')
            merge=issue.get('merge',{});readable(merge.get('reason'),'merge reason')
            basis=merge.get('basis')
            if basis not in {'single_candidate','same_event','verified_mechanism'} or (len(cids)>1 and basis=='single_candidate'):
                raise ValueError('multi-candidate issue requires an explicit merge basis')
            samples=_records(issue.get('samples'),'sample_id','samples')
            if not samples:
                if issue['analysis_state']=='complete':raise ValueError('deep analysis requires actual representative samples')
                readable(issue.get('sample_gap'),'missing event sample explanation')
            sample_events=set();roles=set();query_eids=set()
            for sample_id,sample in samples.items():
                _strings(sample,('reason','frame_name'))
                if sample.get('role') not in ROLES:raise ValueError('sample role is invalid')
                roles.add(sample['role'])
                eids=_refs(sample.get('event_ids'),events,'sample events')
                audited_sample=audit_sample(sample,receipt,model['events'])
                for eid in eids:
                    item=model['events'][eid]
                    sample_events.add(item['node']['ref']);query_eids.add(item['record']['evidence_id'])
                model['samples'][(iid,sample_id)]=audited_sample
            if 'normal' not in roles:readable(issue.get('comparison_gap'),'normal comparison missing')
            steps=issue.get('discovery_steps')
            if not isinstance(steps,list) or not steps:raise ValueError('discovery steps are required')
            covered_samples=set();source_step=False
            for step in steps:
                _strings(step,('question','action','observation','judgment'))
                evidence_refs(step.get('evidence_refs'),'discovery evidence')
                covered_samples.update(_refs(step.get('sample_ids',[]),samples,'discovery samples',0))
                source_step=bool(_refs(step.get('source_refs',[]),sources,'discovery source',0)) or source_step
            if set(samples)-covered_samples:raise ValueError('sample is not explained by discovery steps')
            if issue['analysis_state']=='complete' and not source_step:raise ValueError('discovery must explain the source analysis')
            measurements=issue.get('measurements',[])
            if not isinstance(measurements,list):raise ValueError('measurements must be an array')
            model['measurements'][iid]=[]
            for measurement in measurements:
                if set(measurement)-{'name','quantity','scope','interpretation','evidence_id','pointer','sample_ids'}:
                    raise ValueError('measurements accept references, never authored numeric values or ratios')
                _strings(measurement,('name','scope','interpretation'))
                _,_,content=receipt(measurement['evidence_id'])
                pointer=measurement['pointer'];field=pointer.rsplit('/',1)[-1]
                quantity=measurement.get('quantity')
                patterns={'duration_ns':r'(?:gpu_|cpu_)?(?:duration|self_time|running_time|wait_time|exclusive|inclusive|self|total)_ns',
                    'count':r'(?:count|.*_count|calls|returned)', 'bytes':r'(?:bytes|.*_bytes)',
                    'sampling_percent':r'(?:sample_percent|sampling_percent)'}
                if quantity not in patterns or not re.fullmatch(patterns[quantity],field):
                    raise ValueError('measurement quantity does not match the Query field semantics')
                value=_pointer(content,pointer)
                if isinstance(value,bool) or not re.fullmatch(r'\d+(?:\.\d+)?',str(value)):
                    raise ValueError('measurement must reference a recorded numeric value')
                _refs(measurement.get('sample_ids',[]),samples,'measurement samples',0)
                model['measurements'][iid].append((measurement,value))
            counters=issue.get('counterevidence')
            if not isinstance(counters,list) or not counters:raise ValueError('counterevidence review is required')
            for counter in counters:
                _strings(counter,('observation','effect'))
                evidence_refs(counter.get('evidence_refs'),'counterevidence receipts')
                _refs(counter.get('source_refs',[]),sources,'counterevidence source',0)
            for key in ('limitations','next_actions'):
                if not isinstance(issue.get(key),list) or (key=='next_actions' and not issue[key]):raise ValueError(key+' must be an array')
                for text in issue[key]:readable(text,key)
            # Bind issue evidence to each member's independently audited investigation.
            for cid in cids:
                finding=findings[cid]
                finding_eids={r.get('evidence_id') for r in finding.get('investigation_receipts',[])}
                if issue['analysis_state']=='complete' and not finding_eids.intersection(query_eids):
                    raise ValueError('issue samples are unrelated to a member investigation')
            if basis=='same_event':
                member_refs=[]
                for cid in cids:
                    refs=set()
                    for r in findings[cid].get('investigation_receipts',[]):
                        if r.get('outcome','available')!='available':continue
                        _,_,content=receipt(r['evidence_id'])
                        for ptr in r.get('entity_json_pointers',[]):
                            n=_pointer(content,ptr)
                            if isinstance(n,dict) and n.get('ref'):refs.add(n['ref'])
                    member_refs.append(refs)
                if not member_refs or not set.intersection(*member_refs).intersection(sample_events):raise ValueError('same_event merge lacks shared actual entity')
            if basis=='verified_mechanism':
                if issue['readiness'] not in {'cause_verified','actionable'}:raise ValueError('cross-instance mechanism merge requires verified cause')
                evidence_refs(merge.get('evidence_refs'),'mechanism merge evidence')
            key=(issue['mechanism'].strip().casefold(),issue['trigger'].strip().casefold(),tuple(sorted(issue.get('source_refs',[]))))
            source_keys={(sources[s]['snapshot']['sha256'],sources[s]['path'],sources[s]['line'],sources[s]['end_line']) for s in issue.get('source_refs',[])}
            key=(key[0],key[1],tuple(sorted(source_keys)))
            if key in mechanisms:raise ValueError('same mechanism is duplicated in issue list')
            mechanisms[key]=iid;issue_event_refs[iid]=sample_events;issue_source_keys[iid]=source_keys
            issue_names[iid]={item['node']['name'] for item in model['events'].values() if item['node']['ref'] in sample_events}
        expected_ownership = set(findings) if routing is None else set(findings)&required_ids
        if set(ownership)!=expected_ownership:raise ValueError('deep issues must cover deep-required findings only; unchosen P2 cannot claim source completion')
        splits=_records(review.get('candidate_splits',[]),'candidate_id','candidate splits')
        expected_splits={cid for cid,owners in ownership.items() if len(owners)>1}
        if set(splits)!=expected_splits:raise ValueError('one candidate with multiple causes requires an explicit split review')
        for cid,split in splits.items():
            if set(_refs(split.get('issue_ids'),issues,'split issues',2))!=ownership[cid]:raise ValueError('split review does not cover all associated causes')
            readable(split.get('reason'),'distinct causes within candidate')
            evidence_refs(split.get('evidence_refs'),'candidate split evidence')
            _refs(split.get('source_refs'),sources,'candidate split source')
        decisions={}
        if not isinstance(review.get('dedup_review'),list):raise ValueError('dedup review is required')
        for decision in review['dedup_review']:
            pair=tuple(sorted(_refs(decision.get('issue_ids'),issues,'dedup issues',2)))
            if len(pair)!=2 or pair in decisions:raise ValueError('dedup pair is invalid or repeated')
            readable(decision.get('reason'),'distinct cause reason')
            evidence_refs(decision.get('evidence_refs'),'distinct cause evidence')
            _refs(decision.get('source_refs'),sources,'distinct cause source')
            decisions[pair]=decision
        keys=list(issues)
        for i,left in enumerate(keys):
            for right in keys[i+1:]:
                shared_events=issue_event_refs[left]&issue_event_refs[right]
                shared_sources=issue_source_keys[left]&issue_source_keys[right]
                if (shared_events or shared_sources or issue_names[left]&issue_names[right]) and tuple(sorted((left,right))) not in decisions:
                    raise ValueError('overlapping issues need a distinct-cause review or merge')
        used_events=set().union(*issue_event_refs.values()) if issue_event_refs else set()
        if used_events!=set(actual_events):raise ValueError('unreferenced event registry entries are not issue evidence')
        pending=required_ids-set(ownership) | (required_ids&blocked)
        model['pending']=sorted(pending)
        model['counts'].update(event_total=len(actual_events),deep_completed=len(set(ownership)-blocked),required_pending=len(pending))
        model['review']=review
        model['deep_completed_ids']=sorted(set(ownership)-blocked)
        if analysis.get('report_status','complete')=='complete' and pending:
            raise ValueError('required candidates remain without completed source analysis')
        from performance_report import render_documents,check_documents
        errors.extend(check_documents(render_documents(analysis,model),model))
    except (KeyError,TypeError,ValueError,OSError,IndexError,AttributeError,subprocess.SubprocessError) as exc:
        errors.append('performance_review: '+str(exc))
    return errors,model
