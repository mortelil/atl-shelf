"""Exercise the real updater against local Git repositories; no network or compiler."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / 'data/runtime-setup.sh'
REPOS = ('android_translation_layer', 'bionic_translation', 'art_standalone')

class RuntimeUpdateTest(unittest.TestCase):
    def git(self, path, *args):
        return subprocess.check_output(['git', '-C', str(path), *args], stderr=subprocess.DEVNULL, text=True).strip()

    def commit(self, path):
        self.git(path, 'add', '.')
        self.git(path, '-c', 'user.name=Test', '-c', 'user.email=test@example.invalid', 'commit', '-m', 'fixture')
        return self.git(path, 'rev-parse', 'HEAD')

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        base = Path(self.tmp.name)
        self.root = base / 'runtime'
        self.workspace = self.root / 'workspace'
        self.workspace.mkdir(parents=True)
        self.remotes = {}
        pins = {}
        for repo in reversed(REPOS):
            remote = base / repo
            remote.mkdir()
            self.git(remote, 'init', '-b', 'linux-mobile-experimental')
            (remote / 'source').write_text('original')
            if repo == 'android_translation_layer':
                (remote / 'dependency-lock.json').write_text(json.dumps({'dependencies': pins}))
                mobile = remote / 'scripts/mobile'
                mobile.mkdir(parents=True)
                (mobile / 'build.sh').write_text('''#!/bin/sh
set -eu
root=$(dirname "$ATL_WORKSPACE")
echo build >> "$root/build-count"
test ! -f "$root/fail-build"
mkdir -p "$ATL_BUILD_DIR"
printf '#!/bin/sh\\nexit 0\\n' > "$ATL_BUILD_DIR/android-translation-layer"
chmod +x "$ATL_BUILD_DIR/android-translation-layer"
''')
                (remote / '.gitignore').write_text('build-mobile/\n')
                generated = remote / 'src/api-impl/com/android/internal'
                generated.mkdir(parents=True)
                (generated / 'R.java').write_text('/* AUTO-GENERATED FILE. DO NOT MODIFY. */\noriginal')
            pins[repo] = {'commit': self.commit(remote)}
            self.git(self.workspace, 'clone', str(remote), repo)
            self.remotes[repo] = remote

    def run_update(self, ok=True):
        result = subprocess.run(['sh', str(SCRIPT), 'github', str(self.root), '1', 'update', str(getattr(self, 'patch_file', '/dev/null'))], capture_output=True, text=True)
        self.assertEqual(result.returncode == 0, ok, result.stdout + result.stderr)
        return result.stdout

    def test_update_noop_pins_failure_retry_and_edits(self):
        self.run_update()
        self.assertIn('No compilation needed', self.run_update())
        self.assertEqual(len((self.root / 'build-count').read_text().splitlines()), 1)
        # A companion branch moving must not override ATL's pinned version.
        remote = self.remotes['bionic_translation']
        (remote / 'source').write_text('new companion')
        self.commit(remote)
        self.assertIn('No compilation needed', self.run_update())
        remote = self.remotes['android_translation_layer']
        (remote / 'source').write_text('new ATL')
        revision = self.commit(remote)
        (self.root / 'fail-build').touch()
        self.run_update(ok=False)
        self.assertNotIn(revision, (self.root / 'built-revisions').read_text())
        (self.root / 'fail-build').unlink()
        self.run_update()
        self.assertIn(revision, (self.root / 'built-revisions').read_text())
        (self.workspace / 'android_translation_layer/source').write_text('user edit')
        self.run_update(ok=False)
        self.assertEqual((self.workspace / 'android_translation_layer/source').read_text(), 'user edit')

    def test_generated_sources_are_backed_up(self):
        self.run_update()
        generated = self.workspace / 'android_translation_layer/src/api-impl/com/android/internal/R.java'
        content = '/* AUTO-GENERATED FILE. DO NOT MODIFY. */\nregenerated'
        generated.write_text(content)
        self.assertIn('No compilation needed', self.run_update())
        backups = list((self.root / 'generated-backups').glob('*/src/api-impl/com/android/internal/R.java'))
        self.assertEqual(len(backups), 1)
        self.assertEqual(backups[0].read_text(), content)
        self.assertTrue(generated.read_text().endswith('original'))

    def test_managed_runtime_patch_survives_updates_and_preserves_edits(self):
        repo = self.workspace / 'android_translation_layer'
        source = repo / 'source'
        source.write_text('density fix')
        self.patch_file = self.root / 'display.patch'
        self.patch_file.write_text(self.git(repo, 'diff') + '\n')
        self.git(repo, 'restore', 'source')
        self.run_update()
        self.assertEqual(source.read_text(), 'density fix')
        self.assertIn('No compilation needed', self.run_update())
        self.assertEqual(source.read_text(), 'density fix')
        source.write_text('user modified managed patch')
        self.run_update(ok=False)
        self.assertEqual(source.read_text(), 'user modified managed patch')

    def test_lock_and_network_failure(self):
        (self.root / '.build-lock').mkdir()
        self.run_update(ok=False)
        (self.root / '.build-lock').rmdir()
        repo = self.workspace / 'android_translation_layer'
        self.git(repo, 'remote', 'set-url', 'origin', str(self.root / 'missing'))
        self.run_update(ok=False)
        self.assertFalse((self.root / 'built-revisions').exists())
        self.assertFalse((self.root / '.build-lock').exists())

if __name__ == '__main__':
    unittest.main()
