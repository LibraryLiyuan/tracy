"""Render one readable explanation per reviewed cause; keep audit IDs in JSON."""
from __future__ import annotations

from decimal import Decimal
import hashlib
import html
import json
from pathlib import Path
import re
import shutil

from performance_review import OPAQUE, validate_review
from report_common import ValidationFailure, write_json

LABELS={'frame_window':'整帧或窗口','event':'具体事件','resource_operation':'资源或I/O操作',
    'frame_cost':'事件成本','stable_slow':'稳定偏慢','sustained_pressure':'持续压力','aggregate':'整体统计',
    'cpu_event_cost':'CPU事件成本','cpu_wait':'CPU等待候选','gpu_execution':'GPU执行','job_scheduling':'Job调度',
    'memory':'内存','gpu_resource':'GPU资源','io':'I/O','sampling':'采样','telemetry':'采集开销','unclassified':'类型待核实',
    'normal':'正常对照','peak':'高峰','repeated':'重复高峰','sustained':'持续偏慢','recovery':'恢复期','user_focus':'用户指定',
    'cost_located':'开销已定位','mechanism_supported':'源码机制有支持，运行原因待核实','cause_verified':'当前范围原因已核实',
    'actionable':'可进入优化设计','rejected':'解释已排除','blocked':'深查受阻',
    'matched':'与录制构建匹配','unverified':'尚未核实录制版本','mismatch':'与录制版本不匹配',
    'budget':'超预算','top':'排序入选','anomaly':'异常信号','capacity':'容量压力','local':'局部信号',
    'cpu':'CPU','gpu':'GPU','job':'Job','unknown':'类型待核实'}
SPECIALTIES=[('01-Trace-Quality-and-Scan-Coverage.md','录制质量与覆盖',set()),
    ('02-Frame-and-CPU-Analysis.md','帧与CPU',{'cpu'}),('03-GPU-and-Render-Analysis.md','GPU与渲染',{'gpu'}),
    ('04-Job-and-Scheduling-Analysis.md','Job与调度',{'job'}),
    ('05-Memory-and-GPU-Resource-Analysis.md','内存与GPU资源',{'memory','gpu_resource'}),
    ('06-IO-Sampling-and-System-Analysis.md','I/O、采样与系统',{'io','sampling'}),
    ('07-Tracy-Telemetry-Overhead.md','采集开销',{'telemetry'}),
    ('08-Profiler-Manual-Verification-Guide.md','人工回查指南',set()),
    ('09-Investigation-Backlog-and-Evidence-Gaps.md','深查待办与证据缺口',set())]


def md(value):
    return str(value).replace('&','&amp;').replace('<','&lt;').replace('>','&gt;').replace('|','\\|').replace('\n','<br>').replace('[','\\[').replace(']','\\]')


def ms(value):
    return '未记录' if value is None else f'{Decimal(str(value))/Decimal(1000000):.6f} ms'


def percent(value,total):
    return '不适用' if value is None or not total else f'{Decimal(value)*100/Decimal(total):.2f}%'


def status(analysis,model,errors=()):
    pending=model.get('counts',{}).get('required_pending')
    return {'schema_version':1,'passed':not errors,'errors':list(errors),'error_count':len(errors),
        'workflow':analysis.get('report_versions',{}).get('analysis_workflow','2.1.0'),'report_status':analysis.get('report_status','complete'),
        'analysis_complete':not errors and analysis.get('report_status','complete')=='complete' and pending==0,
        'required_pending':pending,'counts':model.get('counts',{}),
        'tester_decision_pending':model.get('routing_counts',{}).get('pending',0),
        'source_scope_complete':not errors and pending==0,
        'all_selected_source_complete':not errors and pending==0 and model.get('routing_counts',{}).get('review_total',0)==model.get('routing_counts',{}).get('approved',0) and not model.get('routing_counts',{}).get('filtered',0),
        'completion_scope':'new P0, previously mandatory lower tiers and tester-approved new P1; other new P1 reviewed only' if 'routing_counts' in model else 'all required candidates'}


def _html(markdown):
    # Restricted renderer: escape first; allow only our own headings/anchors.
    lines=['<!doctype html><html lang="zh-CN"><meta charset="utf-8"><title>性能分析报告</title>',
        '<style>body{max-width:1100px;margin:36px auto;font:16px/1.7 sans-serif;color:#243240;padding:0 24px}pre{white-space:pre-wrap;background:#f4f6f8;padding:16px}table{border-collapse:collapse;width:100%}td,th{padding:8px;border:1px solid #ccd4dc}a{color:#075ca8}</style><body>']
    def inline(text):
        escaped=html.escape(text)
        escaped=re.sub(r'\[([^\]]+)\]\(([^)]+)\)',lambda m:'<a href="'+m[2]+'">'+m[1]+'</a>',escaped)
        return re.sub(r'\*\*(.+?)\*\*',r'<strong>\1</strong>',escaped)
    code=False;table=False
    for line in markdown.splitlines():
        if line.startswith('```'):
            code=not code;lines.append('<pre>' if code else '</pre>');continue
        if code:lines.append(html.escape(line));continue
        if line.startswith('|'):
            if not table:lines.append('<table>');table=True
            cells=line.strip('|').split('|')
            if all(re.fullmatch(r'[\s:\-]+',c) for c in cells):continue
            lines.append('<tr>'+''.join('<td>'+inline(c.strip())+'</td>' for c in cells)+'</tr>');continue
        if table:lines.append('</table>');table=False
        if line.startswith('<a id="issue-'):
            lines.append(line);continue
        match=re.match(r'^(#{1,4}) (.*)',line)
        if match:lines.append(f'<h{len(match[1])}>'+html.escape(match[2])+f'</h{len(match[1])}>')
        elif line:lines.append('<p>'+inline(line)+'</p>')
    if table:lines.append('</table>')
    return '\n'.join(lines+['</body></html>'])


def render_documents(analysis,model):
    review=model['review'];issues=review['issues'];counts=model['counts']
    labels=review['evidence_labels'];views=model['classifications'];source_models=model['sources']
    ev_order={eid:i+1 for i,eid in enumerate(sorted(labels))}
    def references(ids):
        return '、'.join(f'[{md(labels[eid])}](Evidence-Index.md#query-{ev_order[eid]})' for eid in ids)
    state='完整深查报告' if analysis.get('report_status','complete')=='complete' else '阶段报告：仍有深查未完成'
    lines=['# 性能分析报告','',state,'',
        f'候选观测：{counts["candidate_total"]}；已登记独立事件：{counts["event_total"]}；整理后条目：{counts["issue_total"]}；必查待完成：{counts["required_pending"]}。',
        '整理后条目包括原因结论和待查线索，数量不等于已确认缺陷数。','',
        '耗时由原始Query结果引用；毫秒换算、同一实例的交集与占比仅作展示，不重新扫描或重排Trace。父子及并发耗时不能相加作为收益。','',
        '## 原因与调查进度','', '| 条目 | 当前可用程度 | 源码深查 |','|---|---|---|']
    for i,issue in enumerate(issues,1):
        lines.append(f'| [{md(issue["title"])}](#issue-{i}) | {LABELS[issue["readiness"]]} | {"已完成阅读" if issue["analysis_state"]=="complete" else "受阻"} |')
    manual=['# 人工回查指南',''];audit=['# 分析过程索引',''];todo=['# 深查待办与证据缺口','']
    source_index={sid:i+1 for i,sid in enumerate(sorted(source_models))}
    for i,issue in enumerate(issues,1):
        iid=issue['issue_id'];link=f'[问题{i}：{md(issue["title"])}](Performance-Analysis-Report.md#issue-{i})'
        lines += ['',f'<a id="issue-{i}"></a>',f'## 问题{i}：{md(issue["title"])}','',md(issue['conclusion']),'',
            f'适用范围：{md(issue["scope"])}',f'当前解释：{md(issue["mechanism"])}',f'触发条件：{md(issue["trigger"])}',
            f'归并依据：{md(issue["merge"]["reason"])}','', '### 筛选入口','',
            '| 候选名称 | Query 原级别 / 分析级别 | 观察层次 | 表现 | 成本类型 | 入选理由 |','|---|---|---|---|---|---|']
        for cid in issue['candidate_ids']:
            view=views[cid];c=view['classification']
            lines.append('| '+ ' | '.join([md(view['name']),(model['candidate_facts'][cid]['priority']+' / '+next(r['analysis_priority'] for r in analysis['review_routing']['records'] if r['candidate_id']==cid) if 'review_routing' in analysis else model['candidate_facts'][cid]['priority']),LABELS.get(c['level'],c['level']),LABELS.get(c['pattern'],c['pattern']),
                LABELS.get(c['cost_kind'],c['cost_kind']),'、'.join(LABELS.get(t,t) for t in c['selection_reasons'])])+' |')
        lines+=['','### 具体样本与调用链','']
        for sample in issue['samples']:
            sm=model['samples'][(iid,sample['sample_id'])];frame=sm['frame'];begin,end=sm['begin'],sm['end']
            kind=sm['kind'];number=('原生帧序列'+str(frame.get('sequence','未记录'))) if kind=='origin_frame' else frame.get('index','') if kind=='frame' else ''
            label=f'{sample["frame_name"]} {number} · {LABELS[sample["role"]]}'
            denominator='本帧' if kind=='frame' else 'GPU片段' if kind=='gpu_interval' else 'OriginFrame' if kind=='origin_frame' else '时间范围未核实'
            timing=f'{denominator}区间：{begin}–{end} ns；{denominator}：{ms(end-begin)}。' if begin is not None else md(sample['timing_gap'])
            if kind=='origin_frame':timing+='原生帧序列不等同于Player.Frame数组索引；内部帧身份保留在原始回执中。'
            if kind=='frame':
                budget=model['frame_budget_ns'];excess=max(Decimal(0),Decimal(end-begin)-budget)
                timing+=f'目标帧预算：{ms(budget)}；本帧超出：{ms(excess)}。预算依据：{references([review["profile_evidence_id"]])}'
            lines += [f'**{md(label)}**：{md(sample["reason"])}',timing,
                f'帧依据：{references([sample["frame_evidence_id"]])}','',
                f'| Event及直接父事件 | 执行线程/队列 | 完整耗时 | 区间内交集 | 占{denominator} | 占直接父事件完整耗时 | 自身耗时 | 运行时间 |',
                '|---|---|---:|---:|---:|---:|---:|---:|']
            manual += [f'## {md(label)}','',link,'']
            for eid in sample['event_ids']:
                ev=model['events'][eid];record=ev['record'];node=ev['node']
                if ev['interval'] is None:
                    lines.append(f'| {md(record["name"])} | {md(record["thread_name"])} | {md(record["timing_gap"])} | 不适用 | 不适用 | 不适用 | 未记录 | 未记录 |')
                    manual.append(f'- {md(record["name"])}：{references([record["evidence_id"]])}；{md(record["timing_gap"])}')
                    continue
                a,b,duration,gpu=ev['interval']
                overlap=max(0,min(b,end)-max(a,begin));parent=record.get('parent_event_id')
                parent_cost=model['events'][parent]['interval'][2] if parent else None
                name=record['name']+('（父：'+model['events'][parent]['record']['name']+'）' if parent else '（根事件）' if node.get('parent_ref') is None else '（祖先待补）')
                lines.append('| '+' | '.join([md(name),md(record['thread_name']),ms(duration),ms(overlap),percent(overlap,end-begin),
                    percent(duration,parent_cost),ms(node.get('self_time_ns')),ms(node.get('running_time_ns'))])+' |')
                manual += [f'- {md(record["thread_name"])} / {md(record["name"])}：{a}–{b} ns；{references([record["evidence_id"]])}']
            lines += ['', '本表每行是一条实际事件实例；聚合调用次数、单次成本分布未提供时不据此推算。毫秒保留六位小数，对应整数纳秒精度；占比显示两位小数，不改写原始Query值。占区间使用交集，跨区间事件另保留完整耗时。GPU片段不是整帧GPU耗时。','']
        if issue.get('comparison_gap'):lines += ['对照限制：'+md(issue['comparison_gap']),'']
        if issue.get('sample_gap'):lines += ['样本限制：'+md(issue['sample_gap']),'']
        if model['measurements'].get(iid):
            lines += ['### 调用次数、单次成本及其他Query指标','', '| 指标 | 原始值或单位换算 | 统计范围 | 含义与依据 |','|---|---|---|---|']
            for measurement,value in model['measurements'][iid]:
                formatted=ms(value) if measurement['quantity']=='duration_ns' else str(value)+{'count':' 次或项','bytes':' bytes','sampling_percent':'%（采样占比）'}[measurement['quantity']]
                lines.append(f'| {md(measurement["name"])} | {formatted} | {md(measurement["scope"])} | {md(measurement["interpretation"])}；{references([measurement["evidence_id"]])} |')
        if issue.get('relation_refs'):
            lines += ['', '### 跨线程或队列的依赖链','']
            for rid in issue['relation_refs']:
                relation=model['relations'][rid]
                left=model['events'][relation['from_event_id']]['record']['name'];right=model['events'][relation['to_event_id']]['record']['name']
                lines.append(f'- {md(left)} → {md(right)}：{md(relation["name"])}。{md(relation["meaning"])}；{references([relation["evidence_id"]])}')
        lines += ['### 如何发现','']
        for j,step in enumerate(issue['discovery_steps'],1):
            lines += [f'**步骤{j}：为什么查这里** — {md(step["question"])}',
                f'- 实际检查：{md(step["action"])}',f'- 查到什么：{md(step["observation"])}',
                f'- 对判断的影响：{md(step["judgment"])}',f'- 依据：{references(step["evidence_refs"])}','']
            if step.get('source_refs'):
                lines.append('对应源码：'+'、'.join(md(source_models[s]['record']['name']) for s in step['source_refs']))
        audit += [f'- {link}：发现步骤、样本、源码和反证均在正文该条目下。']
        lines += ['### 源码分析','',md(issue['source_applicability']),'']
        for sid in issue.get('source_refs',[]):
            source=source_models[sid]['record'];snippet=source_models[sid]['excerpt'];n=source_index[sid]
            lines += [f'**{md(source["name"])}** — [{md(source["path"])}:{source["line"]}](sources/source-{n}.txt)',
                f'函数：{md(source["symbol"])}；{LABELS[source["version_status"]]}。','', '```cpp',snippet,'```','',
                f'- 实现行为：{md(source["behavior"])}',f'- 执行条件：{md(source["conditions"])}',
                f'- 已有优化与约束：{md(source["existing_controls"])}',f'- 与录制证据的对应：{md(source["runtime_support"])}',
                f'- 依据：{references(source["evidence_refs"])}','']
        if issue.get('source_blocker'):
            blocker=issue['source_blocker'];lines+=['源码分析受阻：'+md(blocker['reason'])]+['- 已查：'+md(s) for s in blocker['searches']]+['下一步：'+md(blocker['next_action']),'']
        lines += ['### 反例与结论边界','']
        for counter in issue['counterevidence']:
            lines += ['- '+md(counter['observation'])+' → '+md(counter['effect'])+'；'+references(counter['evidence_refs'])]
        lines += ['- '+md(text) for text in issue['limitations']]
        lines += ['','### 下一步','']+['- '+md(text) for text in issue['next_actions']]
        if issue['analysis_state']=='blocked':todo += [link]+['- '+md(x) for x in issue['next_actions']]+['']
    assigned={cid for issue in issues for cid in issue['candidate_ids']}
    for cid in model['pending']:
        if cid not in assigned:todo.append('- '+md(views[cid]['name'])+'：尚未完成定向调查和源码分析。')
    if not model['pending']:todo.append('全部必查候选已完成深查记录；机制待验证事项仍见各问题下一步，不等于全部根因已确认。')
    todo += ['', '未入选候选保存在机器候选清单中，不逐条转换为用户待办。']
    docs={'Performance-Analysis-Report.md':'\n'.join(lines)+'\n','Analysis-Process-and-Query-Audit.md':'\n'.join(audit)+'\n'}
    index=['# 证据索引','', '以下链接使用可读名称；原始回执中的内部身份保留在机器数据中。','']
    for eid,n in ev_order.items():
        index += [f'<a id="query-{n}"></a>',f'## {md(labels[eid])}','',f'[原始请求](evidence/request-{n}.json) · [原始响应](evidence/response-{n}.json)','']
    docs['Evidence-Index.md']='\n'.join(index)
    for name,title,domains in SPECIALTIES:
        content=['# '+title,'']
        if name.startswith('08-'):content=manual
        elif name.startswith('09-'):content=todo
        elif name.startswith('01-'):
            content += [f'全部候选观测：{counts["candidate_total"]}；必查候选：{counts["required_total"]}；深查待完成：{counts["required_pending"]}。',
                '证据查询覆盖与源码深查完成分别核验；没有自动确认所有性能解释。']
            content += [md(note['description']) for note in review.get('quality_notes',[])]
        else:
            for i,issue in enumerate(issues,1):
                if domains & {views[c]['classification']['domain'] for c in issue['candidate_ids']}:
                    content.append(f'- [问题{i}：{md(issue["title"])}](Performance-Analysis-Report.md#issue-{i}) — {LABELS[issue["readiness"]]}')
            if len(content)==2:content.append('本专项没有已整理条目；不表示该域不存在开销。')
            if name.startswith('07-'):content.append('单次录制没有测得总体采集开销的对照差值。')
        docs[name]='\n'.join(content)+'\n'
    if analysis.get('report_versions',{}).get('analysis_workflow')=='2.2.0':
        from review_routing import render_routing
        docs['P1-Source-Review-Candidates.md']=render_routing(analysis)
        problems=['# Tracy 数据收集与 AI 性能分析问题汇总','','以下为证据质量与分析过程问题，不计入性能候选数量，也不使用性能 P 级。','','## 采集质量','']
        for note in review.get('quality_notes',[]):
            problems.append('- '+md(note['description']))
        problems+=['','采集质量原始记录完整保存在 analysis-result.json；具体质量限制见[录制质量与覆盖](01-Trace-Quality-and-Scan-Coverage.md)。','','## 其他已记录问题','']
        for issue in analysis.get('process_issues',[]):
            problems+=['### '+md(issue['name']),'','- 事实：'+md(issue['fact']),'- 影响范围：'+md(issue['impact_scope']),'- 对结论的影响：'+md(issue['consequence']),'- 下一步：'+md(issue['next_action']),'- 证据：'+references(issue['evidence_refs']),'']
        if not analysis.get('process_issues'):problems.append('未登记额外过程问题；这不表示全部运行原因已经解释。')
        problems+=['','[深查待办与证据缺口](09-Investigation-Backlog-and-Evidence-Gaps.md)']
        docs['Capture-and-Analysis-Issues.md']='\n'.join(problems)+'\n'

        docs['Performance-Analysis-Report.md']=docs['Performance-Analysis-Report.md'].replace('完整深查报告','本轮指定范围深查报告',1)
        docs['Performance-Analysis-Report.md']+='\n[P1 源码深查候选与测试人员决定](P1-Source-Review-Candidates.md)\n\n[采集与分析问题汇总](Capture-and-Analysis-Issues.md)\n\n完成仅指 新 P0、原有低级别必查项与测试人员已批准项；其余新 P1 没有声称源码深查完成。\n'
    return docs


def check_documents(docs,model):
    errors=[]
    internal_ids=set(model['classifications'])|set(model['review']['evidence_labels'])|{i['issue_id'] for i in model['review']['issues']}
    for name,text in docs.items():
        if OPAQUE.search(text):errors.append('unreadable technical identity in rendered document: '+name)
        for identity in internal_ids:
            if len(identity)>=4 and re.search(r'[:_-]',identity) and re.search(r'(?<!\w)'+re.escape(identity)+r'(?!\w)',text):
                errors.append('internal identity leaked into readable document: '+name)
    return errors


def build_review_report(args,analysis,candidates,evidence,model):
    output=args.output.resolve()
    if output.exists() and any(output.iterdir()):
        raise ValidationFailure(['new workflow requires an empty output directory; retain previous report as history'])
    docs=render_documents(analysis,model)
    if args.html:
        docs['visuals/index.html']=_html(docs['Performance-Analysis-Report.md'])
        if args.standalone:docs['visuals/standalone.html']=docs['visuals/index.html']
    elif args.standalone:raise ValidationFailure(['--standalone requires --html'])
    # Check every visible artifact before writing. IDs are never a label fallback.
    errors=check_documents(docs,model)
    if errors:raise ValidationFailure(errors)
    output.mkdir(parents=True,exist_ok=True)
    from build_report import copy_evidence_data
    copy_evidence_data(output,args.evidence,{e['evidence_id']:e for e in evidence['evidence']})
    from candidate_manifest import copy_candidate_manifest
    copy_candidate_manifest(args.candidates,output)
    write_json(output/'analysis-result.json',analysis);write_json(output/'evidence-manifest.json',evidence)
    for row in analysis.get('review_routing',{}).get('records',[]):
        if row.get('decision_record'):
            from investigation_receipts import _file
            decision=row['decision_record']; _file(args.evidence.parent,decision)
            destination=output/decision['path']; destination.parent.mkdir(parents=True,exist_ok=True)
            shutil.copyfile(args.evidence.parent/decision['path'],destination)
    (output/'evidence').mkdir(exist_ok=True)
    ev={item['evidence_id']:item for item in evidence['evidence']}
    for n,eid in enumerate(sorted(model['review']['evidence_labels']),1):
        raw=ev[eid].get('mcp_receipt',ev[eid])
        for kind in ('request','response'):
            shutil.copyfile(args.evidence.parent/raw[kind]['path'],output/'evidence'/f'{kind}-{n}.json')
    (output/'sources').mkdir(exist_ok=True)
    for n,sid in enumerate(sorted(model['sources']),1):
        source=model['sources'][sid]['record'];relative=Path(source['snapshot']['path'])
        destination=output/relative;destination.parent.mkdir(parents=True,exist_ok=True)
        shutil.copyfile(args.evidence.parent/relative,destination)
        shutil.copyfile(destination,output/'sources'/f'source-{n}.txt')
    for name,text in docs.items():
        path=output/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_text(text,encoding='utf-8')
    # Revalidation uses the copied evidence, not the authoring directory.
    errors,checked=validate_review(analysis,candidates,evidence,output)
    if errors:raise ValidationFailure(errors)
    write_json(output/'report-validation.json',status(analysis,checked))
    write_json(output/'manifest.json',{'schema_version':1,'workflow':analysis.get('report_versions',{}).get('analysis_workflow','2.1.0'),'report_builder':analysis['report_versions']['report_builder'],'template':analysis['report_versions']['template'],'artifacts':[
        {'path':p.relative_to(output).as_posix(),'sha256':hashlib.sha256(p.read_bytes()).hexdigest()}
        for p in sorted(output.rglob('*')) if p.is_file()]})
    print(str(output/'Performance-Analysis-Report.md'))
    return 0
