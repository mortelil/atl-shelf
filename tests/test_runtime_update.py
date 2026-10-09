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
        (self.root / 'downloads').mkdir()
        (self.root / 'downloads/r8-8.3.37.jar').write_text('fixture; no network')
        self.remotes = {}
        pins = {}
        for repo in reversed(REPOS):
            remote = base / repo
            remote.mkdir()
            self.git(remote, 'init', '-b', 'main')
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
                (mobile / 'build-art-runtime.sh').write_text('#!/bin/sh\nset -eu\nmkdir -p "$ATL_PREFIX/lib"\nprintf art > "$ATL_PREFIX/lib/libart.so"\nprintf compiler > "$ATL_PREFIX/lib/libart-compiler.so"\n')
                (mobile / 'build-core-java.sh').write_text('#!/bin/sh\nset -eu\ntest -s "$R8_JAR"\ntest ! -f "$(dirname "$ATL_WORKSPACE")/fail-core"\nmkdir -p "$ATL_PREFIX/share/art"\nprintf core > "$ATL_PREFIX/share/art/core-all-hostdex.jar"\n')
                (mobile / 'build-apk-verifier.sh').write_text('#!/bin/sh\nset -eu\nmkdir -p "$ATL_PREFIX/libexec/atl-apk-verifier"\nprintf \'#!/bin/sh\\nexit 0\\n\' > "$ATL_PREFIX/libexec/atl-apk-verifier/run"\nchmod +x "$ATL_PREFIX/libexec/atl-apk-verifier/run"\n')
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

    def test_missing_runtime_artifact_rebuilds_and_failed_core_is_not_published(self):
        self.run_update()
        core = self.root / 'prefix/share/art/core-all-hostdex.jar'
        core.unlink()
        (self.root / 'fail-core').touch()
        self.run_update(ok=False)
        self.assertFalse(core.exists())
        (self.root / 'fail-core').unlink()
        self.run_update()
        self.assertTrue(core.exists())
        self.assertIn('No compilation needed', self.run_update())
        self.assertEqual(len((self.root / 'build-count').read_text().splitlines()), 3)

    def test_old_recipe_metadata_does_not_skip_runtime_upgrade(self):
        self.run_update()
        metadata = self.root / 'built-revisions'
        # Old Shelf stored only the source revisions (and optional display patch).
        metadata.write_text('\n'.join(metadata.read_text().splitlines()[:-1]) + '\n')
        self.assertNotIn('No compilation needed', self.run_update())
        self.assertIn('No compilation needed', self.run_update())
        self.assertEqual(len((self.root / 'build-count').read_text().splitlines()), 2)

    def test_existing_checkout_after_branch_rename(self):
        for repo in REPOS:
            self.git(self.workspace / repo, 'branch', '-m', 'linux-mobile-experimental')
        remote = self.remotes['android_translation_layer']
        (remote / 'source').write_text('new main revision')
        revision = self.commit(remote)
        self.run_update()
        self.assertEqual(self.git(self.workspace / 'android_translation_layer', 'rev-parse', 'HEAD'), revision)

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
