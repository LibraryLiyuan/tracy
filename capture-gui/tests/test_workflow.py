"""Runs the real controller with real Capture, converter, Query and Tracy producer."""
import json
import os
import pathlib
import subprocess
import sys
import time
import unittest

RUNNER, TOOLS, PRODUCER, OUTPUT = map(lambda x: pathlib.Path(x).resolve(), sys.argv[1:5])
del sys.argv[1:5]

class WorkflowTests(unittest.TestCase):
    def test_timer_capture_publishes_only_validated_snapshot(self):
        OUTPUT.mkdir(parents=True, exist_ok=True)
        ini = OUTPUT / 'requested.ini'
        ini.write_text('[Config]\nPreset=HighEvidence\n', encoding='utf-8')
        settings = {'output':str(OUTPUT),'tools':str(TOOLS),'ini':str(ini),
            'name':'controller-' + str(time.time_ns()), 'scene':'synthetic scene',
            'note':'timer integration', 'timed':True,'seconds':2, 'sound':False, 'port':38086}
        with (OUTPUT / 'producer.log').open('wb') as log:
            producer = subprocess.Popen([str(PRODUCER),'--duration-seconds','120'],stdout=log,stderr=log,
                env={**os.environ, 'TRACY_PORT':'38086'})
            try:
                settings['target_pid']=producer.pid
                config=OUTPUT/'run.json'
                config.write_text(json.dumps(settings), encoding='utf-8')
                result=subprocess.run([str(RUNNER),str(config)],capture_output=True,timeout=300)
                (OUTPUT/'runner.log').write_bytes(result.stdout+result.stderr)
                self.assertEqual(result.returncode,0,result.stdout.decode('utf-8',errors='replace')+result.stderr.decode('utf-8',errors='replace'))
                task=json.loads((OUTPUT/settings['name']/'task.json').read_text(encoding='utf-8'))
                self.assertEqual(task['state'],'complete')
                self.assertEqual(task['stop_reason'],'timer')
                self.assertTrue(pathlib.Path(task['stream']).is_file())
                self.assertTrue(pathlib.Path(task['final']).is_file())
                self.assertFalse(pathlib.Path(task['candidate']).exists())
                self.assertTrue(pathlib.Path(task['validation_evidence']).is_file())
                self.assertEqual(task['candidate_sha256'],task['validated_sha256'])
                mapping=pathlib.Path(task['stream']+'.snapshot-map').read_text(encoding='utf-8')
                self.assertIn('snapshot="'+pathlib.Path(task['final']).name+'"',mapping)
                self.assertIsNone(producer.poll(),'normal stop must not terminate target')
            finally:
                if producer.poll() is None:
                    producer.terminate()
                producer.wait(timeout=10)

if __name__=='__main__':unittest.main()
