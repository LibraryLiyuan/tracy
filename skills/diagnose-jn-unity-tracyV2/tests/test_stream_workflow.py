import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1]/'scripts/stream_workflow.py'
SPEC = importlib.util.spec_from_file_location('stream_workflow', SCRIPT)
workflow = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(workflow)


class StreamWorkflowTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)/'native'
        self.root.mkdir()

    def tearDown(self):
        self.temp.cleanup()

    def put(self, name, value, ndjson=False):
        text = ''.join(json.dumps(x)+'\n' for x in value) if ndjson else json.dumps(value)
        (self.root/name).write_text(text, encoding='utf-8')

    def fixture(self, count=2, windows=True):
        frames = []
        for i in range(count):
            frames.append({'index':i,'ref':f'frame:{i}','candidate_id':f'frame:{i}',
                'start_ns':str(i*40_000_000),'end_ns':str((i+1)*40_000_000),
                'duration_ns':'40000000','gpu_union_ns':'20000000','gpu_copy_union_ns':'0',
                'selected':True,'severe':True,'reasons':['strict_frame_miss','severe_frame','gpu_window_budget']})
        fast = [{k:v for k,v in f.items() if k in ['index','start_ns','end_ns','duration_ns']} | {'quality':'complete'} for f in frames]
        fast.append({'index':count,'start_ns':str(count*40_000_000),'end_ns':None,'quality':'open_tail'})
        self.put('frames.ndjson',frames,True); self.put('fast-frames.ndjson',fast,True)
        self.put('candidates.ndjson',frames,True); self.put('native-candidates.ndjson',[],True)
        self.put('fast-summary.json',{'complete_frames':count,'open_tail':1})
        ws = [{'start_ns':frames[0]['start_ns'],'end_ns':frames[-1]['end_ns']}]
        self.put('triage.json',{'frame_set':{'complete_frames':count},'native_candidates_written':0,
                 'selection':{'candidate_frames':count,'severe_frames':count,
                 'reason_counts':{'strict_frame_miss':count,'severe_frame':count,'gpu_window_budget':count}},
                 'recommended_windows':ws})
        if windows:
            self.put('window-0-cpu.json',{}); self.put('window-0-gpu.json',{})
        self.rehash()

    def rehash(self, requests=None):
        files = [{'path':p.name,'bytes':p.stat().st_size,'sha256':workflow.sha(p)} for p in self.root.iterdir() if p.name!='manifest.json']
        self.put('manifest.json',{'complete':True,'cpu_gpu_scanned':True,'recorded_candidate_enumeration_complete':True,
                 'source':{'schema':17,'revision':7,'source_sha256':'fingerprint'},'files':files,'requests':requests or []})

    def test_complete_frames_exclude_explicit_open_tail(self):
        self.fixture()
        result = workflow.audit(self.root)
        self.assertEqual(result['frames_verified'],2)
        self.assertEqual(result['excluded_open_tail_verified'],1)
        self.assertTrue(result['severe_window_coverage_complete'])

    def test_missing_window_is_not_claimed_covered(self):
        self.fixture(windows=False)
        result = workflow.audit(self.root)
        self.assertEqual(result['severe_without_cpu_gpu_window'],[0,1])
        self.assertFalse(result['severe_window_coverage_complete'])

    def test_more_than_64_severe_frames_are_batched_not_dropped(self):
        self.fixture(count=130,windows=False)
        result = workflow.evidence_plans(self.root,Path(self.temp.name)/'plans')
        self.assertEqual(result['windows'],130)
        self.assertEqual(len(result['plans']),3)
        emitted = [w['frame_index'] for name in result['plans'] for w in workflow.read(Path(self.temp.name)/'plans'/name)['windows']]
        self.assertEqual(emitted,list(range(130)))

    def test_tampered_receipt_rejected(self):
        self.fixture()
        with (self.root/'frames.ndjson').open('a') as f: f.write(' ')
        with self.assertRaisesRegex(ValueError,'checksum'): workflow.audit(self.root)

    def test_duplicate_candidate_rejected_even_with_updated_hash(self):
        self.fixture()
        candidates = list(workflow.rows(self.root/'candidates.ndjson'))
        self.put('candidates.ndjson',[candidates[0],candidates[0]],True);self.rehash()
        with self.assertRaisesRegex(ValueError,'identities/reasons'): workflow.audit(self.root)

    def test_partial_query_rejected(self):
        self.fixture()
        self.put('request-0.json',{'request':{'method':'trace.info'},'response':{'ok':True,'partial':True}})
        self.rehash(requests=[{'index':0}])
        with self.assertRaisesRegex(ValueError,'partial'): workflow.audit(self.root)

    def test_capture_directory_ambiguity_rejected(self):
        directory = Path(self.temp.name)/'capture';directory.mkdir()
        (directory/'a.tracy-stream').touch();(directory/'b.tracy-stream').touch()
        with self.assertRaisesRegex(ValueError,'exactly one'):workflow.capture_path(directory)

    def test_existing_outputs_are_not_overwritten(self):
        path = Path(self.temp.name)/'output.json'
        workflow.write_new(path,{'first':True})
        with self.assertRaises(FileExistsError):workflow.write_new(path,{'second':True})
        self.assertEqual(workflow.read(path),{'first':True})


if __name__ == '__main__':
    unittest.main()
