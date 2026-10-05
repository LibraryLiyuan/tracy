"""Runs the real controller with real Capture, converter, Query and Tracy producer."""
import json
import os
import pathlib
import socket
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
                self.assertEqual(task['conversion_backend'], 'bounded-export')
                converter = next(p for p in task['processes'] if p['role'] == 'converter')
                self.assertEqual(pathlib.Path(converter['exe']).name, 'tracy-stream-export.exe')
                self.assertTrue(pathlib.Path(task['stream']).is_file())
                self.assertTrue(pathlib.Path(task['final']).is_file())
                self.assertFalse(pathlib.Path(task['candidate']).exists())
                self.assertTrue(pathlib.Path(task['validation_evidence']).is_file())
                self.assertEqual(task['candidate_sha256'],task['validated_sha256'])
                # The store followed the recording and the export reused it.
                self.assertEqual(task['store_follow_exit_code'], 0)
                self.assertTrue((pathlib.Path(task['stream'] + '.store') / 'manifest.json').is_file())
                mapping=pathlib.Path(task['stream']+'.snapshot-map').read_text(encoding='utf-8')
                self.assertIn('snapshot="'+pathlib.Path(task['final']).name+'"',mapping)
                self.assertIsNone(producer.poll(),'normal stop must not terminate target')
                task['settings']['tools'] = str(OUTPUT / 'removed-custom-toolchain')
                task['settings']['tools_custom'] = True
                task_file = OUTPUT / settings['name'] / 'task.json'
                task_file.write_text(json.dumps(task), encoding='utf-8')
                recovery_config = OUTPUT / 'recover-with-current-tools.json'
                recovery_config.write_text(json.dumps({'recover': str(task_file.parent),
                    'tools': str(TOOLS), 'tools_custom': True}), encoding='utf-8')
                recovered = subprocess.run([str(RUNNER), str(recovery_config)], capture_output=True, timeout=60)
                (OUTPUT / 'recover-with-current-tools.log').write_bytes(recovered.stdout + recovered.stderr)
                self.assertEqual(recovered.returncode, 0, recovered.stderr.decode('utf-8', errors='replace'))
                self.assertEqual(json.loads(task_file.read_text(encoding='utf-8'))['state'], 'complete')
            finally:
                if producer.poll() is None:
                    producer.terminate()
                producer.wait(timeout=10)

    def test_long_capture_publishes_journal_and_event_store(self):
        OUTPUT.mkdir(parents=True, exist_ok=True)
        with socket.socket() as port_probe:
            port_probe.bind(('127.0.0.1', 0))
            port = port_probe.getsockname()[1]
        name = 'store-mode-' + str(time.time_ns())
        ini = OUTPUT / (name + '.ini')
        ini.write_text('[Config]\nPreset=HighEvidence\n', encoding='utf-8')
        # store_minutes 0 publishes every recording the way long ones are.
        settings = {'output': str(OUTPUT), 'tools': str(TOOLS), 'ini': str(ini), 'name': name,
                    'timed': True, 'seconds': 3, 'store_minutes': 0, 'sound': False, 'port': port}
        with (OUTPUT / (name + '-producer.log')).open('wb') as log:
            producer = subprocess.Popen([str(PRODUCER), '--duration-seconds', '120'], stdout=log, stderr=log,
                                        env={**os.environ, 'TRACY_PORT': str(port)})
            try:
                settings['target_pid'] = producer.pid
                config = OUTPUT / (name + '.json')
                config.write_text(json.dumps(settings), encoding='utf-8')
                result = subprocess.run([str(RUNNER), str(config)], capture_output=True, timeout=300)
                (OUTPUT / (name + '-runner.log')).write_bytes(result.stdout + result.stderr)
                self.assertEqual(result.returncode, 0, result.stdout.decode('utf-8', errors='replace') +
                                 result.stderr.decode('utf-8', errors='replace'))
                task_file = OUTPUT / name / 'task.json'
                task = json.loads(task_file.read_text(encoding='utf-8'))
                self.assertEqual(task['state'], 'complete')
                self.assertEqual(task['stop_reason'], 'timer')
                self.assertEqual(task['output_kind'], 'store')
                self.assertEqual(task['final'], task['stream'])
                self.assertEqual(task['store_follow_exit_code'], 0)
                roles = [p['role'] for p in task['processes']]
                self.assertIn('store', roles)
                self.assertNotIn('converter', roles)
                for process in task['processes']:
                    self.assertEqual(pathlib.Path(process['exe']).parent, TOOLS)
                stream = pathlib.Path(task['stream'])
                store = pathlib.Path(task['store'])
                self.assertEqual(store, pathlib.Path(task['stream'] + '.store'))
                for part in ('manifest.json', 'events.sqlite', 'events.blocks'):
                    self.assertTrue((store / part).is_file(), part)
                self.assertEqual(list(stream.parent.glob(stream.name + '.store.building-*')), [])
                self.assertFalse(stream.with_suffix('.tracy').exists())
                self.assertFalse(pathlib.Path(task['candidate']).exists())
                summary = json.loads(pathlib.Path(task['store_summary']).read_text(encoding='utf-8'))
                self.assertEqual(summary['source_sha256'], task['stream_sha256'])
                self.assertGreater(summary['counts']['events'], 0)
                self.assertGreater(summary['counts']['cpu_zones'], 0)
                self.assertEqual(task['quality'], summary)
                self.assertEqual(task['quality_passed'], summary['valid'])
                # Saving is timed by step, and in total from the stop request.
                timings = task['processing_seconds']
                for key in ('drain', 'journal_scan', 'stream_identity', 'store_finish', 'save_total'):
                    self.assertGreaterEqual(timings[key], 0, key)
                self.assertGreaterEqual(timings['save_total'], timings['store_finish'])
                # The published store opens and verifies against its journal.
                reopened = subprocess.run([str(TOOLS / 'tracy-stream-export.exe'), str(stream), '--store-only'],
                                          capture_output=True, timeout=120)
                self.assertEqual(reopened.returncode, 0, reopened.stderr.decode('utf-8', errors='replace'))
                info = json.loads(reopened.stdout.decode('utf-8').strip().splitlines()[-1])
                self.assertEqual(info['source_sha256'], summary['source_sha256'])
                self.assertEqual(info['data_sha256'], summary['data_sha256'])
                self.assertIsNone(producer.poll(), 'normal stop must not terminate target')
                # Recovery of a published store task keeps it a store task.
                recovery_config = OUTPUT / (name + '-recover.json')
                recovery_config.write_text(json.dumps({'recover': str(task_file.parent), 'tools': str(TOOLS),
                                                       'tools_custom': True}), encoding='utf-8')
                recovered = subprocess.run([str(RUNNER), str(recovery_config)], capture_output=True, timeout=120)
                (OUTPUT / (name + '-recover.log')).write_bytes(recovered.stdout + recovered.stderr)
                self.assertEqual(recovered.returncode, 0, recovered.stderr.decode('utf-8', errors='replace'))
                task = json.loads(task_file.read_text(encoding='utf-8'))
                self.assertEqual(task['state'], 'complete')
                self.assertEqual(task['output_kind'], 'store')
                self.assertEqual(task['quality']['data_sha256'], summary['data_sha256'])
            finally:
                if producer.poll() is None:
                    producer.terminate()
                producer.wait(timeout=10)

    def test_untimed_capture_survives_missing_status_after_sixty_seconds(self):
        OUTPUT.mkdir(parents=True, exist_ok=True)
        with socket.socket() as port_probe:
            port_probe.bind(('127.0.0.1', 0))
            port = port_probe.getsockname()[1]
        name = 'missing-status-' + str(time.time_ns())
        task_root = OUTPUT / name
        ini = OUTPUT / (name + '.ini')
        ini.write_text('[Config]\nPreset=HighEvidence\n', encoding='utf-8')
        cancel = OUTPUT / (name + '.cancel')
        settings = {'output': str(OUTPUT), 'tools': str(OUTPUT / 'stale-tools'),
                    'ini': str(ini), 'name': name, 'timed': False, 'seconds': 1,
                    'manual_stop_after_ms': 75000, 'cancel_file': str(cancel),
                    'sound': False, 'port': port}
        # The runner is installed with the bundle, just like the GUI. The legacy
        # persisted tool directory must migrate to those matching executables.
        self.assertEqual(RUNNER.parent, TOOLS)
        with (OUTPUT / (name + '-producer.log')).open('wb') as producer_log, \
             (OUTPUT / (name + '-runner.log')).open('wb') as runner_log:
            producer = subprocess.Popen([str(PRODUCER), '--duration-seconds', '180'],
                stdout=producer_log, stderr=producer_log, env={**os.environ, 'TRACY_PORT': str(port)})
            runner = None
            try:
                settings['target_pid'] = producer.pid
                config = OUTPUT / (name + '.json')
                config.write_text(json.dumps(settings), encoding='utf-8')
                runner = subprocess.Popen([str(RUNNER), str(config)], stdout=runner_log, stderr=runner_log)
                status = task_root / 'capture-status.json'
                deadline = time.monotonic() + 150
                while time.monotonic() < deadline:
                    self.assertIsNone(runner.poll(), 'workflow ended before fault injection')
                    try:
                        current = json.loads(status.read_text(encoding='utf-8'))
                    except (OSError, ValueError):
                        current = {}
                    if current.get('elapsed_ms', 0) >= 65000:
                        break
                    time.sleep(.1)
                else:
                    self.fail('recording did not reach the 65-second fault-injection window')
                injected = 0
                until = time.monotonic() + 2
                while time.monotonic() < until:
                    try:
                        status.write_text('{', encoding='utf-8')
                        injected += 1
                    except OSError:
                        pass
                    self.assertFalse((task_root / 'capture.stop').exists(),
                                     'transient status failure requested an early stop')
                    time.sleep(.1)
                self.assertGreater(injected, 0)
                self.assertEqual(runner.wait(timeout=300), 0)
                task = json.loads((task_root / 'task.json').read_text(encoding='utf-8'))
                self.assertEqual(task['state'], 'complete')
                self.assertEqual(task['stop_reason'], 'user')
                self.assertGreaterEqual(task['elapsed_ms'], 75000)
                self.assertEqual(task['conversion_backend'], 'bounded-export')
                for process in task['processes']:
                    self.assertEqual(pathlib.Path(process['exe']).parent, TOOLS)
                self.assertTrue(pathlib.Path(task['final']).is_file())
                self.assertIsNone(producer.poll(), 'normal stop terminated the target')
            finally:
                if runner is not None and runner.poll() is None:
                    cancel.touch()
                    try:
                        runner.wait(timeout=15)
                    except subprocess.TimeoutExpired:
                        runner.kill()
                        runner.wait(timeout=10)
                if producer.poll() is None:
                    producer.terminate()
                producer.wait(timeout=10)

if __name__=='__main__':unittest.main()
