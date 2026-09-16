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
        return root,task,root/(name+'.tracy'),root/(name+'.pending.tracy'),root/(name+'.tracy-stream')

    def run_recovery(self,root):
        config=root/'recover.json'
        config.write_text(json.dumps({'recover':str(root)}),encoding='utf-8')
        result=subprocess.run([str(RUNNER),str(config)],capture_output=True,timeout=120)
        (root/'recovery-runner.log').write_bytes(result.stdout+result.stderr)
        return result,json.loads((root/'task.json').read_text(encoding='utf-8'))

    def test_valid_candidate_retries_validation_without_reconverting(self):
        root,task,final,candidate,stream=self.fixture('candidate')
        final.rename(candidate)
        task['state']='failed'
        (root/'task.json').write_text(json.dumps(task),encoding='utf-8')
        prior=sum(p['role']=='converter' for p in task['processes'])
        result,after=self.run_recovery(root)
        self.assertEqual(result.returncode,0,result.stdout.decode('utf-8',errors='replace'))
        self.assertEqual(after['state'],'complete')
        self.assertEqual(sum(p['role']=='converter' for p in after['processes']),prior)
        self.assertEqual(hashlib.sha256(final.read_bytes()).hexdigest(),task['validated_sha256'])

    def test_changed_stream_invalidates_old_publication_evidence(self):
        root,task,final,candidate,stream=self.fixture('changed-stream')
        before=final.read_bytes()
        with stream.open('ab') as out:out.write(b'changed input')
        result,after=self.run_recovery(root)
        self.assertNotEqual(result.returncode,0)
        self.assertEqual(after['state'],'failed')
        self.assertEqual(final.read_bytes(),before)

    def test_tail_interruption_is_published_only_as_partial(self):
        root,task,final,candidate,stream=self.fixture('partial')
        final.unlink()
        content=stream.read_bytes()
        stream.write_bytes(content[:-80])
        for key in ['stream_sha256','candidate_sha256','validated_sha256']:task.pop(key,None)
        task['state']='interrupted'
        task['capture_exit_code']=130
        (root/'task.json').write_text(json.dumps(task),encoding='utf-8')
        result,after=self.run_recovery(root)
        self.assertEqual(result.returncode,0,result.stdout.decode('utf-8',errors='replace'))
        self.assertEqual(after['state'],'partial')
        self.assertEqual(after['completeness'],'RecoverablePrefix')
        self.assertTrue(final.exists())
        self.assertEqual(stream.read_bytes(),content[:-80])

if __name__=='__main__':unittest.main()
