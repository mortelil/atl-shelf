"""Exercise the shipped executable over a headless CLI, with isolated user data."""
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest
import zipfile

BINARY = Path(os.environ.get('SHELF_BINARY', Path(__file__).resolve().parents[1] / 'build/atl-shelf')).resolve()

class CliTest(unittest.TestCase):
    def setUp(self):
        if not BINARY.is_file():
            self.skipTest('Build atl-shelf or set SHELF_BINARY first')
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.base = Path(self.tmp.name)
        self.bin = self.base / 'bin'
        self.bin.mkdir()
        self.env = dict(os.environ, HOME=str(self.base), XDG_DATA_HOME=str(self.base / 'data'),
                        XDG_CONFIG_HOME=str(self.base / 'config'), QT_QPA_PLATFORM='no-gui-plugin',
                        PATH=str(self.bin) + ':' + os.environ['PATH'])
        self.env.pop('DISPLAY', None)
        self.env.pop('WAYLAND_DISPLAY', None)
        self.root = self.base / 'data/ATL Shelf/atl-shelf'
        self.exe('systemctl', '#!/bin/sh\necho systemctl-output-must-not-pollute-json\nexit 0\n')
        self.exe('aapt', '#!/bin/sh\nexit 1\n')
        self.exe('kscreen-doctor', '#!/bin/sh\ncat <<\'SCREEN\'\n' + json.dumps({'outputs': [{
            'enabled': True, 'connected': True, 'name': 'DSI-1', 'scale': 2.65,
            'currentModeId': '1', 'rotation': 1,
            'modes': [{'id': '1', 'size': {'width': 1080, 'height': 2280}}]}]}) + '\nSCREEN\n')
        self.runtime = self.exe('fake-atl', '#!/bin/sh\necho "scale=$ATL_RENDER_SCALE"\necho "$@"\nexit 0\n')
        self.apk = self.base / 'Example.apk'
        with zipfile.ZipFile(self.apk, 'w') as apk:
            apk.writestr('AndroidManifest.xml', '<manifest package="test.fixture"/>')
        self.call('runtime', 'setup', '--source', 'existing', '--binary', str(self.runtime))

    def exe(self, name, text):
        path = self.bin / name
        path.write_text(text)
        path.chmod(0o755)
        return path

    def call(self, *args, code=0, input=None):
        result = subprocess.run([str(BINARY), 'cli', *args], env=self.env, input=input,
                                capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, code, result.stdout + result.stderr)
        value = json.loads(result.stdout)  # Exactly one JSON document, no child output.
        self.assertEqual(value['schemaVersion'], 1)
        self.assertEqual(value['ok'], code == 0)
        return value.get('result'), result

    def install(self, id='sample', *extra):
        return self.call('install', '--source', 'local', '--value', str(self.apk), '--id', id, *extra)[0]

    def test_headless_lifecycle_and_physical_display(self):
        entry = self.install()
        self.assertEqual(entry['id'], 'sample')
        self.assertTrue(Path(entry['apkPath']).is_file())
        status, _ = self.call('doctor')
        self.assertEqual(status['display']['width'], 1080)
        self.assertEqual(status['display']['logicalWidth'], 408)
        self.assertEqual(status['display']['logicalHeight'], 860)
        plan, _ = self.call('launch', 'sample', '--dry-run')
        self.assertEqual(plan['environment']['ATL_RENDER_SCALE'], '2.65')
        self.assertEqual(plan['arguments'][-4:], ['-w', '408', '-h', '860'])
        result, command = self.call('launch', 'sample', '--wait')
        self.assertEqual(result['exitCode'], 0)
        self.assertIn('scale=2.65', command.stderr)
        logs, _ = self.call('logs', 'sample', '--lines', '10')
        self.assertIn('scale=2.65', logs['text'])
        self.call('remove', 'sample')
        self.assertFalse(Path(entry['dataDirectory']).exists())
        self.assertEqual(self.call('list')[0], [])
        self.assertFalse((self.base / 'data/applications/atl-shelf-sample.desktop').exists())

    def test_saved_display_fallback(self):
        self.install()
        path = self.root / 'settings.json'
        value = json.loads(path.read_text())
        value.update(screenWidth=1080, screenHeight=2280)
        value['runtimeEnv']['ATL_RENDER_SCALE'] = '2.65'
        path.write_text(json.dumps(value))
        self.exe('kscreen-doctor', '#!/bin/sh\nexit 1\n')
        plan, _ = self.call('launch', 'sample', '--dry-run')
        self.assertEqual(plan['display']['source'], 'saved')
        self.assertEqual(plan['environment']['ATL_RENDER_SCALE'], '2.65')
        self.assertEqual(plan['arguments'][-4:], ['-w', '408', '-h', '860'])

    def test_validated_configuration_and_unsafe_option_rejection(self):
        entry = self.install()
        result, _ = self.call('configure', 'sample', '--file', '-', input=json.dumps({
            'name': 'Renamed app', 'fitScreen': False, 'width': 530, 'height': 1060,
            'launchEnv': {'TEST_VALUE': 'hello'}}))
        self.assertEqual(result['name'], 'Renamed app')
        plan, _ = self.call('launch', 'sample', '--dry-run')
        self.assertEqual(plan['arguments'][-4:], ['-w', '200', '-h', '400'])
        self.assertEqual(plan['environment']['TEST_VALUE'], 'hello')
        transient, _ = self.call('launch', 'sample', '--dry-run', '--env', '{"ATL_RENDER_SCALE":"1","TEST_VALUE":null}', '--activity', 'example.Main')
        self.assertEqual(transient['environment']['ATL_RENDER_SCALE'], '1')
        self.assertIsNone(transient['environment']['TEST_VALUE'])
        self.assertIn('example.Main', transient['arguments'])
        self.assertEqual(self.call('show', 'sample')[0]['launchEnv']['TEST_VALUE'], 'hello')
        self.call('configure', 'sample', '--set', '{"id":"../../other"}', code=2)
        self.call('configure', 'sample', '--set', '{"width":0}', code=2)
        self.call('configure', 'sample', '--set', '{"daily":true}', code=2)
        self.call('remove', 'sample', '--dry-run', code=2)
        self.call('remove', 'sample', '--force', code=2)
        self.call('remove', '../sample', code=2)
        self.assertTrue(Path(entry['apkPath']).exists())
        self.call('settings', '--set', '{"runtimeEnv":{"TEST_RUNTIME":"works"}}')
        self.assertEqual(self.call('settings')[0]['stored']['runtimeEnv']['TEST_RUNTIME'], 'works')
        self.call('configure', 'sample', '--set', '{"launchEnv":{"TEST_VALUE":null}}')
        self.assertNotIn('TEST_VALUE', self.call('show', 'sample')[0]['launchEnv'])

    def test_replace_preserves_data_and_keep_data_removal(self):
        entry = self.install()
        sentinel = Path(entry['dataDirectory']) / 'userdata'
        sentinel.write_text('keep this')
        self.call('replace', 'sample', '--apk', str(self.apk))
        self.assertEqual(sentinel.read_text(), 'keep this')
        self.call('remove', 'sample', '--keep-data')
        self.assertEqual(sentinel.read_text(), 'keep this')
        self.call('install', '--source', 'local', '--value', str(self.apk), '--id', 'sample', code=2)

    def test_failure_exit_and_process_control(self):
        self.install()
        self.runtime.write_text('#!/bin/sh\necho simulated-crash\nexit 23\n')
        result, _ = self.call('launch', 'sample', '--wait', code=6)
        self.assertEqual(result['exitCode'], 23)
        self.runtime.write_text(f'#!{sys.executable}\nimport time\ntime.sleep(60)\n')
        result, _ = self.call('launch', 'sample')
        pid = result['pid']
        self.addCleanup(lambda: self.cleanup_pid(pid))
        deadline = time.monotonic() + 3
        while pid not in self.call('show', 'sample')[0]['processes'] and time.monotonic() < deadline:
            time.sleep(.05)
        self.assertIn(pid, self.call('show', 'sample')[0]['processes'])
        self.call('launch', 'sample', code=4)
        self.call('remove', 'sample', code=4)
        self.call('replace', 'sample', '--apk', str(self.apk), code=4)
        self.call('stop', 'sample')
        self.assertEqual(self.call('show', 'sample')[0]['processes'], [])

    def test_wait_cancellation_stops_child(self):
        self.install()
        self.runtime.write_text(f'#!{sys.executable}\nimport time\ntime.sleep(60)\n')
        worker = subprocess.Popen([str(BINARY), 'cli', 'launch', 'sample', '--wait'], env=self.env,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.addCleanup(lambda: worker.poll() is None and worker.kill())
        deadline = time.monotonic() + 5
        while not self.call('show', 'sample')[0]['processes'] and time.monotonic() < deadline:
            time.sleep(.01)
        worker.send_signal(signal.SIGINT)
        stdout, stderr = worker.communicate(timeout=10)
        self.assertEqual(worker.returncode, 130, stdout + stderr)
        self.assertFalse(json.loads(stdout)['ok'])
        self.assertEqual(self.call('show', 'sample')[0]['processes'], [])

    @staticmethod
    def cleanup_pid(pid):
        try:
            os.kill(pid, signal.SIGTERM)
        except ProcessLookupError:
            pass

    def test_library_lock_and_corrupt_json_are_not_overwritten(self):
        self.install()
        self.exe('aapt', '#!/bin/sh\nsleep 1\nexit 1\n')
        worker = subprocess.Popen([str(BINARY), 'cli', 'icons', 'sample'], env=self.env,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.addCleanup(lambda: worker.poll() is None and worker.kill())
        deadline = time.monotonic() + 5
        while not (self.root / 'operations.lock').exists() and time.monotonic() < deadline:
            time.sleep(.01)
        self.call('configure', 'sample', '--set', '{"name":"Busy"}', code=4)
        worker.communicate(timeout=10)
        (self.root / 'apps.json').write_text('broken-json')
        self.call('install', '--source', 'local', '--value', str(self.apk), code=5)
        self.assertEqual((self.root / 'apps.json').read_text(), 'broken-json')

    def test_runtime_setup_preserves_explicit_window_preferences(self):
        self.call('settings', '--set', '{"runtimeEnv":{"ATL_DISABLE_FULLSCREEN":"0","TEST_RUNTIME":"preserve"}}')
        self.call('runtime', 'setup', '--source', 'existing', '--binary', str(self.runtime))
        env = self.call('settings')[0]['stored']['runtimeEnv']
        self.assertEqual(env['ATL_DISABLE_FULLSCREEN'], '0')
        self.assertEqual(env['TEST_RUNTIME'], 'preserve')

    def test_timers_and_manual_sources(self):
        self.install()
        self.call('configure', 'sample', '--set', '{"source":"github","github":"example/repo","daily":true}')
        self.assertTrue((self.base / '.config/systemd/user/atl-shelf-sample.timer').exists())
        self.call('configure', 'sample', '--set', '{"daily":false,"source":"local"}')
        self.assertFalse((self.base / '.config/systemd/user/atl-shelf-sample.timer').exists())
        results, _ = self.call('update', '--all')
        self.assertTrue(results[0]['skipped'])
        self.call('check-updates', 'sample', code=5)
        self.call('show', 'missing', code=3)
        self.call('help')

if __name__ == '__main__':
    unittest.main()
