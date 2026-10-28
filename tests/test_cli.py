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
        self.runtime = self.exe('fake-atl', '#!/bin/sh\nif [ "$2" = --install-internal ]; then mkdir -p "$ANDROID_APP_DATA_DIR/_installed_apks_"; cp "$1" "$ANDROID_APP_DATA_DIR/_installed_apks_/test.fixture.apk"; exit $?; fi\necho "scale=$ATL_RENDER_SCALE"\necho "$@"\nexit 0\n')
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

    def preparation_runtime(self, body):
        original = self.runtime.read_text()
        self.runtime.write_text(original.replace('#!/bin/sh\n', '#!/bin/sh\n' +
            'if [ "$2" = --help ]; then echo "Usage: --prepare APK"; exit 0; fi\n' +
            'if [ "$2" = --prepare ]; then\n' + body + '\nexit 0; fi\n', 1))

    def test_preparation_final_path_environment_and_replacement(self):
        self.preparation_runtime('test -f "$1" || exit 3\n'
            'printf "%s\\n" "$1" "$ANDROID_APP_DATA_DIR" "$CUSTOM_PREP" "$PWD" >> "$HOME/prepared"\n'
            'echo "ATL PREPARE OK: $1"')
        self.call('settings', '--set', json.dumps({'runtimeEnv': {'CUSTOM_PREP': 'same-environment'}}))
        entry = self.install()
        self.assertEqual(entry['startupPreparation']['status'], 'ready')
        captured = (self.base / 'prepared').read_text().splitlines()
        launch = self.call('launch', 'sample', '--dry-run')[0]
        self.assertEqual(captured[:3], [entry['registeredApkPath'],
                         launch['environment']['ANDROID_APP_DATA_DIR'], 'same-environment'])
        self.assertEqual(captured[3], str(self.bin))
        entry = self.call('replace', 'sample', '--apk', str(self.apk))[0]
        self.assertEqual(entry['startupPreparation']['status'], 'ready')
        self.assertEqual(len((self.base / 'prepared').read_text().splitlines()), 8)
        # Switching/reconfiguring the runtime also prepares existing apps.
        self.call('runtime', 'setup', '--source', 'existing', '--binary', str(self.runtime))
        self.assertEqual(len((self.base / 'prepared').read_text().splitlines()), 12)

    def test_preparation_accepts_art_log_terminator(self):
        self.preparation_runtime('printf "\\000ATL PREPARE OK: %s\\n" "$1"')
        self.assertEqual(self.install()['startupPreparation']['status'], 'ready')

    def test_preparation_requires_success_marker_and_preserves_install(self):
        self.preparation_runtime('echo no-marker')
        entry = self.install()
        self.assertEqual(entry['startupPreparation']['status'], 'failed')
        self.assertTrue(Path(entry['registeredApkPath']).is_file())
        self.assertIn('no-marker', Path(entry['startupPreparation']['logPath']).read_text())
        self.runtime.write_text(self.runtime.read_text().replace('echo no-marker', 'echo "ATL PREPARE OK: $1"; exit 9'))
        entry = self.call('replace', 'sample', '--apk', str(self.apk))[0]
        self.assertEqual(entry['startupPreparation']['status'], 'failed')
        self.assertTrue(Path(entry['registeredApkPath']).is_file())

    def test_cancel_preparation_keeps_install_and_stops_child(self):
        self.preparation_runtime('sleep 60 &\nchild=$!\necho "$child" > "$HOME/prepare-child"\nwait "$child"')
        process = subprocess.Popen([str(BINARY), 'cli', 'install', '--source', 'local',
            '--value', str(self.apk), '--id', 'sample'], env=self.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.addCleanup(lambda: process.kill() if process.poll() is None else None)
        marker = self.base / 'prepare-child'
        deadline = time.monotonic() + 10
        while not marker.exists() and time.monotonic() < deadline:
            time.sleep(.02)
        self.assertTrue(marker.exists())
        process.send_signal(signal.SIGINT)
        stdout, stderr = process.communicate(timeout=10)
        result = json.loads(stdout)
        self.assertTrue(result['ok'], stderr)  # Installation already committed.
        self.assertEqual(result['result']['startupPreparation']['status'], 'canceled')
        self.assertTrue(Path(result['result']['registeredApkPath']).is_file())
        child = Path('/proc') / marker.read_text().strip() / 'stat'
        deadline = time.monotonic() + 3
        while child.exists() and child.read_text().split()[2] != 'Z' and time.monotonic() < deadline:
            time.sleep(.02)
        self.assertTrue(not child.exists() or child.read_text().split()[2] == 'Z')

    def test_old_runtime_remains_installable(self):
        entry = self.install()
        self.assertEqual(entry['startupPreparation']['status'], 'unsupported')
        self.assertTrue(Path(entry['registeredApkPath']).is_file())

    def test_github_runtime_artifacts_supply_environment_and_preserve_overrides(self):
        self.install()
        self.call('settings', '--set', '{"runtimeType":"github"}')
        env = self.call('launch', 'sample', '--dry-run')[0]['environment']
        self.assertNotIn('ATL_CORE_JAR', env)
        prefix = self.base / 'data/atl-shelf/runtime/github/prefix'
        for name in ('lib/libart.so', 'share/art/core-all-hostdex.jar', 'libexec/atl-apk-verifier/run'):
            path = prefix / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('fixture')
            path.chmod(0o755)
        env = self.call('launch', 'sample', '--dry-run')[0]['environment']
        self.assertEqual(env['ATL_CORE_JAR'], str(prefix / 'share/art/core-all-hostdex.jar'))
        self.assertEqual(env['ATL_APK_VERIFIER'], str(prefix / 'libexec/atl-apk-verifier/run'))
        self.assertEqual(env['LD_PRELOAD'], '/usr/lib/libjemalloc.so.2')
        self.call('settings', '--set', '{"runtimeEnv":{"ATL_CORE_JAR":"/custom/core.jar","LD_PRELOAD":"/custom/allocator.so"}}')
        env = self.call('launch', 'sample', '--dry-run')[0]['environment']
        self.assertEqual(env['ATL_CORE_JAR'], '/custom/core.jar')
        self.assertEqual(env['LD_PRELOAD'], '/custom/allocator.so')
        self.call('settings', '--set', '{"runtimeType":"existing","runtimeEnv":{"ATL_CORE_JAR":null,"LD_PRELOAD":null}}')
        env = self.call('launch', 'sample', '--dry-run')[0]['environment']
        self.assertNotIn('ATL_CORE_JAR', env)
        self.assertNotIn('LD_PRELOAD', env)

    def test_local_apk_metadata_and_replacement(self):
        def badging(version, code):
            self.exe('aapt', "#!/bin/sh\nprintf \"package: name='test.fixture' versionCode='" + code + "' versionName='" + version + "'\\n\"\n")
        badging('1.18.2', '4294967296')
        entry = self.install('metadata', '--name', 'My custom name')
        self.assertEqual(entry['version'], '1.18.2')
        self.assertEqual(entry['versionCode'], '4294967296')
        self.assertEqual(entry['packageId'], 'test.fixture')
        private = Path(entry['dataDirectory']) / 'app.apk_'
        private.mkdir(exist_ok=True)
        (private / 'synthetic.txt').write_text('preserve me')
        badging('2.0', '4294967297')
        entry = self.call('replace', 'metadata', '--apk', str(self.apk))[0]
        self.assertEqual(entry['version'], '2.0')
        self.assertEqual(entry['versionCode'], '4294967297')
        self.assertEqual(entry['name'], 'My custom name')
        self.assertEqual((private / 'synthetic.txt').read_text(), 'preserve me')
        self.assertEqual(self.call('show', 'metadata')[0]['version'], '2.0')
        self.assertEqual(self.call('list')[0][0]['versionCode'], '4294967297')
        badging('', '3')
        self.assertEqual(self.call('replace', 'metadata', '--apk', str(self.apk))[0]['version'], '')
        self.exe('aapt', '#!/bin/sh\necho malformed\n')
        entry = self.call('replace', 'metadata', '--apk', str(self.apk))[0]
        self.assertEqual(entry['version'], '')
        self.assertIn('warning', entry['installedApk'])
        self.apk.write_bytes(b'not an apk')
        self.call('replace', 'metadata', '--apk', str(self.apk), code=5)

    def test_metadata_import_keeps_architecture_rejection(self):
        wrong = 'x86_64' if os.uname().machine == 'aarch64' else 'arm64-v8a'
        with zipfile.ZipFile(self.apk, 'w') as apk:
            apk.writestr('AndroidManifest.xml', '<manifest/>')
            apk.writestr('lib/' + wrong + '/libexample.so', 'fixture')
        self.call('install', '--source', 'local', '--value', str(self.apk), '--id', 'wrong', code=5)
        self.assertEqual(self.call('list')[0], [])

    def test_internal_install_and_failed_replace_preserve_data(self):
        entry = self.install()
        base = Path(entry['dataDirectory'])
        registered = Path(entry['registeredApkPath'])
        self.assertEqual(registered.read_bytes(), self.apk.read_bytes())
        self.assertEqual(entry['internalPackageId'], 'test.fixture')
        self.assertFalse(list(base.glob('.install-*')))
        private = base / 'app.apk_'
        private.mkdir(exist_ok=True)
        (private / 'keep').write_text('existing user data')
        before = Path(entry['apkPath']).read_bytes()
        library = (self.root / 'apps.json').read_bytes()
        for script in ('exit 12', 'exit 0', 'mkdir -p "$ANDROID_APP_DATA_DIR/_installed_apks_"; echo corrupt > "$ANDROID_APP_DATA_DIR/_installed_apks_/test.fixture.apk"'):
            self.runtime.write_text('#!/bin/sh\n' + script + '\n')
            self.call('replace', 'sample', '--apk', str(self.apk), code=5)
            self.assertEqual(Path(entry['apkPath']).read_bytes(), before)
            self.assertEqual(registered.read_bytes(), before)
            self.assertEqual((private / 'keep').read_text(), 'existing user data')
            self.assertEqual((self.root / 'apps.json').read_bytes(), library)
            self.assertFalse(list(base.glob('.install-*')))
        self.call('clear-data', 'sample')
        self.assertTrue(registered.exists())
        self.call('remove', 'sample')
        self.assertFalse(registered.exists())

    def test_apps_share_profile_without_sharing_private_data(self):
        first = self.install('first')
        self.runtime.write_text(self.runtime.read_text().replace('test.fixture', 'test.second'))
        second = self.install('second')
        self.assertEqual(first['profileDirectory'], second['profileDirectory'])
        for entry in (first, second):
            plan = self.call('launch', entry['id'], '--dry-run')[0]
            self.assertEqual(plan['environment']['ANDROID_APP_DATA_DIR'], first['profileDirectory'])
            self.assertEqual(plan['arguments'][0], entry['registeredApkPath'])
            self.call('launch', entry['id'], '--wait')
            private = Path(entry['dataDirectory']) / 'app.apk_'
            (private / 'sentinel').write_text(entry['id'])
            alias = Path(entry['profileDirectory']) / (entry['internalPackageId'] + '.apk_')
            self.assertEqual((alias / 'sentinel').read_text(), entry['id'])
        self.call('clear-data', 'first')
        self.call('remove', 'first')
        self.assertTrue(Path(second['registeredApkPath']).exists())
        self.assertEqual((Path(second['dataDirectory']) / 'app.apk_/sentinel').read_text(), 'second')
        self.call('install', '--source', 'local', '--value', str(self.apk), '--id', 'duplicate', code=5)
        self.assertTrue(Path(second['registeredApkPath']).exists())
        self.call('remove', 'second')
        self.assertFalse(Path(second['registeredApkPath']).exists())

    def test_legacy_install_moves_into_shared_profile_on_launch(self):
        entry = self.install()
        Path(entry['registeredApkPath']).unlink()
        db = self.root / 'apps.json'
        entries = json.loads(db.read_text())
        del entries[0]['internalPackageId']
        db.write_text(json.dumps(entries))
        private = Path(entry['dataDirectory']) / 'app.apk_'
        private.mkdir(exist_ok=True)
        (private / 'sentinel').write_text('preserved')
        self.call('launch', 'sample', '--wait')
        self.assertTrue(Path(entry['registeredApkPath']).exists())
        self.assertEqual((private / 'sentinel').read_text(), 'preserved')
        self.assertEqual(json.loads(db.read_text())[0]['internalPackageId'], 'test.fixture')

    def test_failed_internal_install_can_be_retried(self):
        original = self.runtime.read_text()
        self.runtime.write_text('#!/bin/sh\necho install-failure\nexit 1\n')
        self.call('install', '--source', 'local', '--value', str(self.apk), '--id', 'retry', code=5)
        self.assertFalse((self.root / 'apps/retry').exists())
        self.assertIn('install-failure', (self.root / 'logs/install-retry.log').read_text())
        self.runtime.write_text(original)
        self.assertEqual(self.install('retry')['internalPackageId'], 'test.fixture')

    def test_cancel_internal_install_removes_temporary_state(self):
        marker = self.base / 'installer-started'
        self.runtime.write_text(f'#!{sys.executable}\nfrom pathlib import Path\nimport time\nPath({str(marker)!r}).touch()\ntime.sleep(60)\n')
        process = subprocess.Popen([str(BINARY), 'cli', 'install', '--source', 'local', '--value', str(self.apk), '--id', 'cancel-install'], env=self.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.addCleanup(lambda: process.poll() is None and process.kill())
        deadline = time.monotonic()+5
        while not marker.exists() and time.monotonic()<deadline: time.sleep(.02)
        self.assertTrue(marker.exists())
        process.send_signal(signal.SIGINT)
        out, err = process.communicate(timeout=5)
        self.assertEqual(process.returncode,130,out+err)
        self.assertFalse((self.root / 'apps/cancel-install').exists())

    def test_doctor_reports_missing_private_androidfw(self):
        self.call('settings', '--set', '{"runtimeType":"github"}')
        libraries = self.call('doctor')[0]['runtimeLibraries']
        fw = next(lib for lib in libraries if lib['library'] == 'libandroidfw.so')
        self.assertFalse(fw['present'])
        self.assertEqual(fw['selection'], 'not-observed')
        path = Path(fw['expectedPath'])
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text('fixture')
        fw = next(lib for lib in self.call('doctor')[0]['runtimeLibraries'] if lib['library'] == 'libandroidfw.so')
        self.assertTrue(fw['present'])
        self.assertEqual(fw['observed'], [])

    def test_clear_private_data_preserves_install_and_other_files(self):
        entry = self.install()
        root = Path(entry['dataDirectory'])
        private = root / 'app.apk_'
        private.mkdir(exist_ok=True)
        (private / 'database').write_text('private')
        cache = root / '.cache'
        cache.mkdir()
        (cache / 'cached').write_text('cache')
        (root / 'launch.log').write_text('diagnostic')
        library = (self.root / 'apps.json').read_bytes()
        self.call('clear-data', 'sample', '--dry-run', code=2)
        self.assertTrue(private.exists())
        self.call('clear-data', 'sample')
        self.assertEqual(list(private.iterdir()), [])
        self.assertFalse(cache.exists())
        self.assertTrue(Path(entry['apkPath']).exists())
        self.assertTrue((root / 'launch.log').exists())
        self.assertEqual((self.root / 'apps.json').read_bytes(), library)
        self.call('clear-data', 'sample')
        outside = self.base / 'shared'
        outside.mkdir()
        (outside / 'keep').write_text('keep')
        private.rmdir()
        private.symlink_to(outside, target_is_directory=True)
        self.call('clear-data', 'sample')
        self.assertTrue((outside / 'keep').exists())
        self.call('configure', 'sample', '--set', json.dumps({'launchEnv': {'ANDROID_APP_DATA_DIR': str(outside)}}))
        self.call('clear-data', 'sample', code=5)
        self.assertTrue((outside / 'keep').exists())

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
        self.call('clear-data', 'sample', code=5)
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
