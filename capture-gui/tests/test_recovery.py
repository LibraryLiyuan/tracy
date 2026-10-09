"""Crash/publication recovery against a small real recorded fixture from test_workflow."""
import hashlib
import json
import pathlib
import shutil
import subprocess
import sys
import unittest
import time

RUNNER, SOURCE, OUTPUT = map(lambda x:pathlib.Path(x).resolve(),sys.argv[1:4])
del sys.argv[1:4]

class RecoveryTests(unittest.TestCase):
    def fixture(self,kind):
        root=OUTPUT/(kind+'-'+str(time.time_ns()))
        shutil.copytree(SOURCE,root)
        task=json.loads((root/'task.json').read_text(encoding='utf-8'))
        name=task['name']
        self.assertEqual(task['output_kind'],'store')
        stream=root/(name+'.tracy-stream')
        # The copied task refers to its own directory.
        for key in ('stream','final'):task[key]=str(stream)
        task['store']=str(root/(name+'.tracy-stream.store'))
        task['store_summary']=str(root/'store-summary.json')
        (root/'task.json').write_text(json.dumps(task),encoding='utf-8')
        return root,task,stream,root/(name+'.tracy-stream.store'),root/'store-summary.json'

    def run_recovery(self,root):
        config=root/'recover.json'
        config.write_text(json.dumps({'recover':str(root)}),encoding='utf-8')
        result=subprocess.run([str(RUNNER),str(config)],capture_output=True,timeout=300)
        (root/'recovery-runner.log').write_bytes(result.stdout+result.stderr)
        return result,json.loads((root/'task.json').read_text(encoding='utf-8'))

    def test_published_store_recovers_without_rebuilding(self):
        root,task,stream,store,summary=self.fixture('published')
        task['state']='failed'
        (root/'task.json').write_text(json.dumps(task),encoding='utf-8')
        before=(store/'manifest.json').read_bytes()
        result,after=self.run_recovery(root)
        self.assertEqual(result.returncode,0,result.stdout.decode('utf-8',errors='replace'))
        self.assertEqual(after['state'],'complete')
        self.assertEqual(after['output_kind'],'store')
        self.assertEqual((store/'manifest.json').read_bytes(),before)
        self.assertNotIn('converter',[p['role'] for p in after['processes']])

    def test_changed_stream_invalidates_old_publication_evidence(self):
        root,task,stream,store,summary=self.fixture('changed-stream')
        before=(store/'manifest.json').read_bytes()
        with stream.open('ab') as out:out.write(b'changed input')
        result,after=self.run_recovery(root)
        self.assertNotEqual(result.returncode,0)
        self.assertEqual(after['state'],'failed')
        self.assertEqual((store/'manifest.json').read_bytes(),before)

    def test_tail_interruption_is_published_only_as_partial(self):
        root,task,stream,store,summary=self.fixture('partial')
        content=stream.read_bytes()
        stream.write_bytes(content[:-80])
        for key in ['stream_sha256','quality','quality_passed']:task.pop(key,None)
        task['state']='interrupted'
        task['capture_exit_code']=130
        (root/'task.json').write_text(json.dumps(task),encoding='utf-8')
        result,after=self.run_recovery(root)
        self.assertEqual(result.returncode,0,result.stdout.decode('utf-8',errors='replace'))
        self.assertEqual(after['state'],'partial')
        self.assertEqual(after['completeness'],'RecoverablePrefix')
        self.assertEqual(stream.read_bytes(),content[:-80])
        # The store was rebuilt for the remaining prefix.
        rebuilt=json.loads(summary.read_text(encoding='utf-8'))
        self.assertEqual(rebuilt['source_sha256'],hashlib.sha256(content[:-80]).hexdigest())
        self.assertEqual(after['quality'],rebuilt)

if __name__=='__main__':unittest.main()
