"""Mechanical provenance checks for confirmation, not a proof of causal reasoning."""
from __future__ import annotations
import hashlib
import json
from pathlib import Path
import re
import subprocess
from investigation_receipts import _receipt, _file


def _source(source, evidence, root, trace):
    for key in ('repository','repository_key','revision','expected_revision','path','symbol','finding'):
        if not isinstance(source.get(key),str) or not source[key].strip():
            raise ValueError('source_confirmation_missing_' + key)
    revision=source['revision']
    if not re.fullmatch(r'[0-9a-fA-F]{40}|[0-9a-fA-F]{64}',revision) or revision != source['expected_revision']:
        raise ValueError('source_confirmation_revision_mismatch')
    method,_,reply=_receipt(root,evidence[source['build_identity_evidence_id']],trace['trace_id'],trace.get('sha256',trace.get('trace_sha256','')))
    data=reply['data']
    if method != 'trace.identity' or data.get('present') is not True or data.get('complete') is not True:
        raise ValueError('source_confirmation_requires_complete_trace_build_identity')
    if data.get('conflicts') or data.get('invalid_records'):
        raise ValueError('source_confirmation_conflicting_build_identity')
    captured=data['identity']['build']['repositories'][source['repository_key']]
    if captured.get('revision') != revision or captured.get('dirty') is True:
        raise ValueError('source_confirmation_capture_revision_mismatch')
    relative=Path(source['path'])
    if relative.is_absolute() or '..' in relative.parts or ':' in source['path'] or source['path'].startswith('-'):
        raise ValueError('source_confirmation_invalid_repository_path')
    repository=Path(source['repository']).resolve(strict=True)
    snapshot=source['snapshot'];p=(root/ snapshot['path']).resolve()
    if Path(snapshot['path']).is_absolute() or not p.is_relative_to(root.resolve()):
        raise ValueError('source_confirmation_snapshot_outside_evidence_root')
    payload=p.read_bytes()
    if hashlib.sha256(payload).hexdigest() != snapshot['sha256'].lower():
        raise ValueError('source_confirmation_snapshot_checksum_mismatch')
    # Read the immutable Git object, never a dirty working file or executable shell command.
    result=subprocess.run(['git','-c','safe.directory='+repository.as_posix(),'-C',str(repository),
        'show',revision+':'+relative.as_posix()],capture_output=True,timeout=30)
    if result.returncode or result.stdout != payload:
        raise ValueError('source_confirmation_snapshot_differs_from_repository_revision')
    line=source.get('line')
    if type(line) is not int or not 1 <= line <= len(payload.splitlines()):
        raise ValueError('source_confirmation_line_out_of_range')


def validate_confirmation_evidence(finding, manifest, root, prefix):
    errors=[]; manifest=manifest or {}; by_id={v.get('evidence_id'):v for v in manifest.get('evidence',[]) if isinstance(v,dict)}
    refs=finding.get('evidence_refs',[])
    if not isinstance(refs,list):refs=[]
    if any(not isinstance(eid,str) or eid not in by_id for eid in refs):
        errors.append(prefix + ': evidence_refs contains missing Evidence')
    for source in finding.get('source_evidence',[]) or []:
        if not isinstance(source,dict) or source.get('used_for_confirmation') is not True:continue
        try:
            if source.get('revision_match') is not True:raise ValueError('source_confirmation_revision_mismatch')
            _source(source,by_id,Path(root),manifest['trace'])
        except (KeyError,TypeError,ValueError,OSError,subprocess.SubprocessError) as exc:
            errors.append(prefix+': '+str(exc))
    if finding.get('conclusion_status') != 'Confirmed' or 'independent_evidence' not in finding.get('confirmation_basis',[]):
        return errors
    try:
        entries=finding.get('independent_evidence',[])
        if len(set(refs)) != len(refs) or len(entries)<2:
            raise ValueError('independent_evidence requires distinct receipts and explicit support channels')
        requests=set();responses=set();channels=set();ids=set()
        for support in entries:
            eid=support['evidence_id'];channel=support['channel']
            if eid not in refs or eid in ids or not support.get('supports','').strip():
                raise ValueError('independent_evidence missing or duplicate support')
            item=by_id[eid];trace=manifest['trace']
            method,params,reply=_receipt(Path(root),item,trace['trace_id'],trace.get('sha256',trace.get('trace_sha256','')))
            # Separate measurement families, not two pagination pages or two aliases.
            actual_channel=method.split('.')[0]
            if channel != actual_channel:raise ValueError('independent_evidence channel differs from actual method')
            query=json.dumps([method,{k:v for k,v in params.items() if k not in {'cursor','limit'}}],sort_keys=True)
            result=json.dumps(reply.get('data'),sort_keys=True)
            if query in requests or result in responses or channel in channels:
                raise ValueError('independent_evidence repeats a query, result, or measurement channel')
            requests.add(query);responses.add(result);channels.add(channel);ids.add(eid)
    except (KeyError,TypeError,ValueError,OSError) as exc:
        errors.append(prefix+': '+str(exc))
    return errors
