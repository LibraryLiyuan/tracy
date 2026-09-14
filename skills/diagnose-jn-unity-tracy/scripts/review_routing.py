"""Workflow routing over immutable Query candidates; no Trace statistics."""
from pathlib import Path
from collections import Counter

VERSION = '2.2.0'
POLICY = 'renumbered-p0-deep-p1-tester-v1'
FIELDS = ('phenomenon', 'selection_reason', 'scope', 'frequency', 'slow_frame_relation',
          'overlap', 'impact_reason', 'uncertainty', 'recommendation', 'next_evidence')
METRICS = ('peak_cost', 'typical_cost', 'self_cost', 'inclusive_cost', 'calls',
           'affected_frames', 'complete_frames', 'frame_budget')


def active(analysis):
    return analysis.get('report_versions', {}).get('analysis_workflow') == VERSION


def priority(candidate):
    """Map legacy Query levels once; capture quality has no performance level."""
    p = candidate.get('priority')
    if p == 'P0' or candidate.get('domain') == 'quality' or 'quality' in candidate.get('triggers', []):
        return None
    if p not in {'P1', 'P2', 'P3', 'P4'}:
        raise ValueError('unknown Query priority scheme; do not guess or shift twice')
    if 'user_focus' in candidate.get('triggers', []):
        return 'P0'
    return {'P1':'P0', 'P2':'P1', 'P3':'P2', 'P4':'P3'}[p]


def in_scope(candidate):
    return priority(candidate) is not None and (candidate.get('selected') is True or priority(candidate) == 'P0')


def mandatory_deep(candidate):
    # Only legacy P2 (new P1) gets the tester-choice exception.
    return priority(candidate) == 'P0' or (candidate.get('selected') is True and priority(candidate) in {'P2', 'P3'})


def make_routing(manifest):
    rows = []
    for c in manifest['candidates']:
        if not in_scope(c):
            continue
        deep = mandatory_deep(c)
        rows.append({'candidate_id': c['candidate_id'], 'query_priority': c['priority'],
                     'analysis_priority': priority(c), 'route': 'mandatory_deep' if deep else 'review',
                     'decision': 'not_applicable' if deep else 'pending',
                     'name': '', 'screening_reason': '', 'review': None})
    return {'schema_version': 1, 'policy': POLICY,
            'candidate_content_sha256': manifest['content_sha256'], 'query_priority_scheme': 'query-v3-P1-P4', 'records': rows}


def query_required(candidate, routing):
    if mandatory_deep(candidate):
        return True
    row = routing.get(candidate['candidate_id'], {})
    return in_scope(candidate) and row.get('route') != 'filtered'


def deep_required(candidate, routing):
    return mandatory_deep(candidate) or routing.get(candidate['candidate_id'], {}).get('decision') == 'approved'


def validate_routing(analysis, manifest, evidence=None, root=None):
    """Validate complete routing, explicit tester choices, and receipt-bound metrics."""
    from performance_review import readable
    from investigation_receipts import _file, _receipt, _pointer
    r = analysis.get('review_routing')
    if not isinstance(r, dict) or r.get('schema_version') != 1 or r.get('policy') != POLICY:
        raise ValueError('review_routing schema 1 and current policy are required')
    if r.get('candidate_content_sha256') != manifest.get('content_sha256'):
        raise ValueError('review routing candidate identity mismatch')
    if r.get('query_priority_scheme') != 'query-v3-P1-P4':
        raise ValueError('explicit original Query priority scheme required')
    issues = analysis.get('process_issues')
    if not isinstance(issues, list):
        raise ValueError('process_issues must explicitly record collection/analysis problems')
    evidence_by_id = {e['evidence_id']:e for e in (evidence or {}).get('evidence',[])}
    for issue in issues:
        if issue.get('category') not in {'capture','query','source','ai_analysis'} or 'priority' in issue:
            raise ValueError('process problems must not carry a performance priority')
        for key in ('name','fact','impact_scope','consequence','next_action'):
            readable(issue.get(key),'process issue '+key)
        refs=issue.get('evidence_refs')
        if not isinstance(refs,list) or not refs:
            raise ValueError('process issue needs actual evidence')
        for eid in refs:
            if eid not in evidence_by_id or eid not in analysis.get('performance_review',{}).get('evidence_labels',{}):
                raise ValueError('process issue evidence must have a readable label')
            raw=evidence_by_id[eid].get('mcp_receipt',evidence_by_id[eid])
            _file(Path(root),raw['request']); _file(Path(root),raw['response'])
    rows = r.get('records')
    if not isinstance(rows, list):
        raise ValueError('review routing records must be an array')
    by_id = {}
    for row in rows:
        cid = row.get('candidate_id')
        if not isinstance(cid, str) or cid in by_id:
            raise ValueError('duplicate or invalid routing candidate')
        by_id[cid] = row
    expected = set()
    ev = {e['evidence_id']: e for e in (evidence or {}).get('evidence', [])}
    labels = analysis.get('performance_review', {}).get('evidence_labels', {})
    receipt_cache = {}
    counts = Counter()
    def receipt(eid):
        if eid not in ev or eid not in labels:
            raise ValueError('triage needs named, registered Query evidence')
        if eid not in receipt_cache:
            trace = (evidence or {})['trace']
            receipt_cache[eid] = _receipt(Path(root), ev[eid], trace['trace_id'], trace.get('sha256', trace.get('trace_sha256', '')))
        return receipt_cache[eid]
    for c in manifest['candidates']:
        if not in_scope(c):
            continue
        cid = c['candidate_id']; expected.add(cid)
        if cid not in by_id:
            raise ValueError('routing omits a selected or promoted candidate')
        row = by_id[cid]
        if row.get('query_priority') != c['priority'] or row.get('analysis_priority') != priority(c):
            raise ValueError('routing must preserve Query priority and deterministic analysis priority')
        readable(row.get('name'), 'candidate readable name')
        mandatory = mandatory_deep(c)
        if mandatory:
            if row.get('route') != 'mandatory_deep' or row.get('decision') != 'not_applicable':
                raise ValueError('mandatory candidate cannot be filtered or deferred by tester routing')
            counts['mandatory_deep_total'] += 1
            continue
        if row.get('route') not in {'review', 'filtered'}:
            raise ValueError('new P1 route must be review or filtered')
        readable(row.get('screening_reason'), 'screening reason')
        if row['route'] == 'filtered':
            if row.get('decision') != 'not_applicable':
                raise ValueError('filtered candidate cannot claim tester approval')
            # Bind exclusion to actual Query facts, not a fabricated top-N cutoff.
            if row.get('screening_facts') != c.get('trigger_evidence', []):
                raise ValueError('filtered candidate must retain exact Query screening facts')
            counts['filtered'] += 1
            continue
        choice = row.get('decision')
        if choice not in {'pending', 'approved', 'declined', 'deferred'}:
            raise ValueError('invalid tester decision')
        if choice != 'pending':
            decision_path = row.get('decision_record', {}).get('path','')
            if (not decision_path.startswith('tester-decisions/') or not decision_path.endswith('.json')
                    or len(Path(decision_path).parts)!=2 or '..' in Path(decision_path).parts):
                raise ValueError('tester decision must use tester-decisions/*.json')
            proof = _file(Path(root), row.get('decision_record', {}))
            if (proof.get('candidate_id') != cid or proof.get('candidate_content_sha256') != manifest.get('content_sha256')
                    or proof.get('decision') != choice or proof.get('origin') not in {'user_message', 'tester_record'}):
                raise ValueError('tester choice must bind this candidate and manifest')
            for key in ('actor', 'at', 'reason', 'source_reference'):
                readable(proof.get(key), 'tester decision ' + key)
        review = row.get('review')
        if not isinstance(review, dict):
            raise ValueError('screened candidate requires a tester-readable review')
        for key in FIELDS:
            readable(review.get(key), 'review ' + key)
        if review.get('impact') not in {'high', 'moderate', 'low', 'unknown'}:
            raise ValueError('impact must be separate from priority')
        if review.get('impact_evidence') not in {'critical_path', 'correlation_only', 'not_established'}:
            raise ValueError('impact relationship must be explicit')
        finding = next((f for f in analysis.get('candidate_findings',[]) if f.get('candidate_id')==cid),{})
        if review['impact_evidence']=='critical_path' and 'typed_relation' not in finding.get('confirmation_basis',[]):
            raise ValueError('critical-path impact needs independently validated typed relation evidence')
        metrics = review.get('metrics')
        if not isinstance(metrics, dict) or set(metrics) != set(METRICS):
            raise ValueError('review metrics must record each value or an explicit evidence gap')
        available = 0
        for key, metric in metrics.items():
            if metric.get('status') == 'unavailable':
                readable(metric.get('reason'), key + ' gap')
                continue
            if metric.get('status') != 'available':
                raise ValueError('invalid metric status')
            readable(metric.get('unit'), key + ' unit')
            readable(metric.get('scope'), key + ' scope/denominator')
            # Use validated MCP content, not author-supplied numeric prose.
            if metric.get('origin') == 'candidate':
                value = _pointer(c, metric.get('pointer'))
            else:
                method, params, content = receipt(metric.get('evidence_id'))
                finding = next((f for f in analysis.get('candidate_findings',[]) if f.get('candidate_id')==cid),{})
                refs=set(finding.get('evidence_refs',[]))
                for item in finding.get('investigation_receipts',[]):
                    refs.update([item.get('evidence_id'),item.get('frame_anchor_evidence_id')])
                if metric.get('evidence_id') not in refs:
                    raise ValueError('triage measurement must bind this candidate investigation')
                value = _pointer(content, metric.get('pointer'))
            if value != metric.get('value') or type(value) != type(metric.get('value')):
                raise ValueError('triage metric differs from saved Query value')
            from decimal import Decimal, InvalidOperation
            if isinstance(value,bool) or not isinstance(value,(str,int,float)):
                raise ValueError('triage measurement must be a numeric scalar')
            try:
                if not Decimal(str(value)).is_finite(): raise ValueError('nonfinite triage metric')
            except InvalidOperation:
                raise ValueError('triage measurement must be numeric')
            available += 1
        if not available:
            raise ValueError('review cannot replace all Query measurements with gaps')
        counts['review_total'] += 1
        counts[choice] += 1
    if expected != set(by_id):
        raise ValueError('routing contains unknown or out-of-scope candidates')
    return by_id, dict(counts)


def render_routing(analysis):
    """Small decision report; source excerpts belong only in approved deep reports."""
    from performance_report import md
    rows = analysis['review_routing']['records']
    labels = analysis['performance_review']['evidence_labels']
    order = {eid: n for n, eid in enumerate(sorted(labels), 1)}
    impact = {'high': '影响较大', 'moderate': '局部成本较高', 'low': '当前影响较小', 'unknown': '证据不足'}
    choice = {'pending': '待测试人员选择', 'approved': '已选择源码深查', 'declined': '本轮不深查', 'deferred': '暂缓选择'}
    lines = ['# 源码深查候选报告', '', '这些是筛选后的成本观察，不是已确认缺陷。新 P0 另行强制深查；本页保留 Query 原优先级与分析优先级。', '',
             '| 候选 | Query / 分析优先级 | 影响 | 现象 | 测试人员决定 |', '|---|---|---|---|---|']
    review_rows = [r for r in rows if r['route'] == 'review']
    for n, r in enumerate(review_rows, 1):
        v = r['review']
        lines.append(f'| [{md(r["name"])}](#review-{n}) | {md(r["query_priority"])} / {md(r["analysis_priority"])} | {impact[v["impact"]]} | {md(v["phenomenon"])} | {choice[r["decision"]]} |')
    titles = dict(zip(FIELDS, ('现象', 'Query 入选理由', '影响范围', '发生频率', '与慢帧的关系', '父子及并发重叠', '影响判断依据', '尚未解释部分', '深查建议', '下一步所需证据')))
    for n, r in enumerate(review_rows, 1):
        v = r['review']; lines += ['', f'<a id="review-{n}"></a>', f'## {md(r["name"])}', '', f'测试人员决定：{choice[r["decision"]]}。', '']
        for key in FIELDS:
            lines.append(f'- {titles[key]}：{md(v[key])}')
        lines += ['', '| 指标 | 原始值 | 单位与统计范围 | 证据 |', '|---|---|---|---|']
        metric_names = dict(zip(METRICS, ('峰值成本', '典型成本', '自身成本', '包含子调用的成本', '调用次数', '受影响帧数', '完整帧数', '帧预算')))
        for key, m in v['metrics'].items():
            if m['status'] == 'unavailable':
                lines.append(f'| {metric_names[key]} | 未获得 | {md(m["reason"])} | — |')
            elif m.get('origin') == 'candidate':
                lines.append(f'| {metric_names[key]} | {md(m["value"])} | {md(m["unit"])}；{md(m["scope"])} | 原始候选触发事实（随报告机器数据保存） |')
            else:
                eid = m['evidence_id']
                lines.append(f'| {metric_names[key]} | {md(m["value"])} | {md(m["unit"])}；{md(m["scope"])} | [{md(labels[eid])}](Evidence-Index.md#query-{order[eid]}) |')
    lines += ['', '## 筛选排除记录', '', '以下排除不表示没有成本；原候选和 Query 数值完整保存在机器数据中。', '']
    for r in rows:
        if r['route'] == 'filtered':
            lines.append(f'- {md(r["name"])}：{md(r["screening_reason"])}')
    return '\n'.join(lines) + '\n'
