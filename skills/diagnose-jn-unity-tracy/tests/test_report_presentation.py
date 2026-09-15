import copy
from pathlib import Path
import sys
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'scripts'))
from performance_review import validate_review
from performance_report import render_documents, _issue_frame_order, _routing_frame_order
from tests.test_deep_workflow import DeepWorkflowTests

class PresentationTests(unittest.TestCase):
    def test_numeric_frame_order_and_consistent_links(self):
        with tempfile.TemporaryDirectory() as tmp:
            a,c,e=DeepWorkflowTests().fixture(Path(tmp))
            errors,m=validate_review(a,c,e,Path(tmp));self.assertFalse(errors)
            base=copy.deepcopy(m['review']['issues'][0]);sm=next(iter(m['samples'].values()))
            issues=[]
            for key,index,scope in [('later',10,'Frames'),('other',1,'Player.Frame'),('earlier',2,'Frames')]:
                i=copy.deepcopy(base);i['issue_id']=key;i['title']=key
                sample=i['samples'][0];sample['sample_id']=key;sample['frame_name']=scope
                item=copy.deepcopy(sm);item['frame']['index']=index;item['frame']['frame_set_name']=scope
                m['samples'][(key,key)]=item;issues.append(i)
            m['review']['issues']=issues
            docs=render_documents(a,m);text=docs['Performance-Analysis-Report.md']
            self.assertLess(text.index('## 问题1：earlier'),text.index('## 问题2：later'))
            self.assertIn('## 问题3：other',text)
            self.assertIn('[问题1：earlier](Performance-Analysis-Report.md#issue-1)',docs['08-Profiler-Manual-Verification-Guide.md'])
            self.assertEqual(['later','other','earlier'],[i['title'] for i in issues])

    def test_discovery_stays_in_data_but_not_reading_documents(self):
        with tempfile.TemporaryDirectory() as tmp:
            a,c,e=DeepWorkflowTests().fixture(Path(tmp))
            errors,m=validate_review(a,c,e,Path(tmp));self.assertFalse(errors)
            docs=render_documents(a,m)
            for text in docs.values():
                self.assertNotIn('### 如何发现',text)
                self.assertNotIn('**步骤1：为什么查这里**',text)
            self.assertTrue(a['performance_review']['issues'][0]['discovery_steps'])
            self.assertIn('### 源码分析',docs['Performance-Analysis-Report.md'])

    def test_normal_counterexample_does_not_change_primary_frame(self):
        issue={'issue_id':'one','samples':[{'sample_id':'normal','role':'normal','frame_name':'Frames'},
                                         {'sample_id':'target','role':'peak','frame_name':'Frames'}]}
        model={'samples':{('one','normal'):{'kind':'frame','frame':{'index':1},'begin':10},
                          ('one','target'):{'kind':'frame','frame':{'index':10},'begin':100}}}
        self.assertEqual(10,_issue_frame_order(issue,model)[2])

    def test_candidate_windows_sort_numerically_without_mixing_job_ids(self):
        model={'candidate_facts':{
            'late':{'domain':'cpu','frame_scope':'captured-set','representative_frames':['10']},
            'early':{'domain':'cpu','frame_scope':'captured-set','representative_frames':['2']},
            'window':{'domain':'cpu','frame_scope':'captured-set','representative_frames':['15'],'local_evidence':{'frame_begin':'1'}},
            'job':{'domain':'job','representative_frames':['0']}}}
        rows=[{'candidate_id':k} for k in ['late','job','early','window']]
        ordered=sorted(rows,key=lambda r:_routing_frame_order(r,model,{'captured-set':'Frames'}))
        self.assertEqual(['window','early','late','job'],[r['candidate_id'] for r in ordered])

if __name__=='__main__':unittest.main()
