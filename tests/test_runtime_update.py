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
# Match ATL's companion validator, including untracked files.
test -z "$(git -C "$ATL_WORKSPACE/art_standalone" status --porcelain)"
test "$ATL_ALLOW_DEPENDENCY_CHANGES" = 1
echo build >> "$root/build-count"
test ! -f "$root/fail-build"
mkdir -p "$ATL_PREFIX/share/art"
printf wolf > "$ATL_PREFIX/share/art/wolfssljni-hostdex.jar"
mkdir -p "$ATL_BUILD_DIR"
printf '#!/bin/sh\\nexit 0\\n' > "$ATL_BUILD_DIR/android-translation-layer"
chmod +x "$ATL_BUILD_DIR/android-translation-layer"
''')
                (mobile / 'build-art-runtime.sh').write_text('#!/bin/sh\nset -eu\nmkdir -p "$ATL_PREFIX/lib"\nprintf "libart.so\\nlibart-compiler.so\\nlibutils.so\\nlibandroidfw.so\\nlibbase.so\\n" > "$ATL_PREFIX/native-libraries.txt"\nprintf base > "$ATL_PREFIX/lib/libbase.so"\nprintf art > "$ATL_PREFIX/lib/libart.so"\nprintf compiler > "$ATL_PREFIX/lib/libart-compiler.so"\nprintf utils > "$ATL_PREFIX/lib/libutils.so"\nif test ! -f "$(dirname "$ATL_WORKSPACE")/omit-androidfw"; then printf framework > "$ATL_PREFIX/lib/libandroidfw.so"; fi\n')
                (mobile / 'build-core-java.sh').write_text('#!/bin/sh\nset -eu\ntest -s "$R8_JAR"\ntest ! -f "$(dirname "$ATL_WORKSPACE")/fail-core"\nmkdir -p "$ATL_PREFIX/share/art"\nprintf core > "$ATL_PREFIX/share/art/core-all-hostdex.jar"\n')
                (mobile / 'build-apk-verifier.sh').write_text('#!/bin/sh\nset -eu\nmkdir -p "$ATL_PREFIX/libexec/atl-apk-verifier"\nprintf \'#!/bin/sh\\nexit 0\\n\' > "$ATL_PREFIX/libexec/atl-apk-verifier/run"\nchmod +x "$ATL_PREFIX/libexec/atl-apk-verifier/run"\n')
                (remote / '.gitignore').write_text('build-mobile/\n')
                generated = remote / 'src/api-impl/com/android/internal'
                generated.mkdir(parents=True)
                (generated / 'R.java').write_text('/* AUTO-GENERATED FILE. DO NOT MODIFY. */\noriginal')
            pins[repo] = {'commit': self.commit(remote)}
            self.git(self.workspace, 'clone', str(remote), repo)
            self.remotes[repo] = remote

    def run_update(self, ok=True, action="update"):
        env = os.environ.copy()
        env['ATL_RUNTIME_TEST_SKIP_GSTREAMER'] = '1'
        result = subprocess.run(['sh', str(SCRIPT), 'github', str(self.root), '1', action, str(getattr(self, 'patch_file', '/dev/null'))], capture_output=True, text=True, env=env)
        self.assertEqual(result.returncode == 0, ok, result.stdout + result.stderr)
        return result.stdout

    def test_gstreamer_build_dependencies_are_installed_for_repository_runtimes(self):
        setup = SCRIPT.read_text()
        self.assertIn('pkg-config --exists gstreamer-app-1.0 gstreamer-video-1.0', setup)
        self.assertIn('gstreamer-dev gst-plugins-base-dev', setup)
        gui = (SCRIPT.parents[1] / 'src/main.cpp').read_text()
        cli = (SCRIPT.parents[1] / 'src/cli.h').read_text()
        for source in (gui, cli):
            self.assertIn('"gstreamer-dev"', source)
            self.assertIn('"gst-plugins-base-dev"', source)

    def test_update_latest_heads_failure_retry_and_edits(self):
        self.run_update()
        self.assertIn('No compilation needed', self.run_update())
        self.assertEqual(len((self.root / 'build-count').read_text().splitlines()), 1)
        # Each companion follows main even when ATL's lock still names an older commit.
        remote = self.remotes['bionic_translation']
        (remote / 'source').write_text('new companion')
        revision = self.commit(remote)
        self.run_update()
        self.assertIn(revision, (self.root / 'built-revisions').read_text())
        remote = self.remotes['art_standalone']
        (remote / 'source').write_text('new ART')
        revision = self.commit(remote)
        self.run_update()
        self.assertIn(revision, (self.root / 'built-revisions').read_text())
        self.assertIn('No compilation needed', self.run_update())
        remote = self.remotes['android_translation_layer']
        (remote / 'source').write_text('new ATL')
        revision = self.commit(remote)
        (self.root / 'fail-build').touch()
        self.run_update(ok=False)
        self.assertFalse((self.root / 'built-revisions').exists())
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

    def test_missing_androidfw_repairs_and_incomplete_success_is_rejected(self):
        self.run_update()
        library = self.root / 'prefix/lib/libandroidfw.so'
        self.assertTrue(library.exists())
        library.unlink()
        (self.root / 'omit-androidfw').touch()
        self.run_update(ok=False)
        self.assertFalse((self.root / 'built-revisions').exists())
        (self.root / 'omit-androidfw').unlink()
        self.run_update()
        self.assertTrue(library.exists())
        self.assertIn('No compilation needed', self.run_update())

    def test_missing_transitive_library_rebuilds(self):
        self.run_update()
        (self.root / 'prefix/lib/libbase.so').unlink()
        self.assertNotIn('No compilation needed', self.run_update())
        self.assertTrue((self.root / 'prefix/lib/libbase.so').exists())
        self.assertIn('No compilation needed', self.run_update())

    def test_old_recipe_metadata_does_not_skip_runtime_upgrade(self):
        self.run_update()
        metadata = self.root / 'built-revisions'
        # Old Shelf stored only the source revisions (and optional display patch).
        metadata.write_text('\n'.join(metadata.read_text().splitlines()[:-1]) + '\n')
        self.assertNotIn('No compilation needed', self.run_update())
        self.assertIn('No compilation needed', self.run_update())
        self.assertEqual(len((self.root / 'build-count').read_text().splitlines()), 2)

    def test_setup_retry_fetches_latest_heads_in_existing_checkouts(self):
        revisions = {}
        for name, remote in self.remotes.items():
            (remote / 'source').write_text('updated source for setup retry')
            revisions[name] = self.commit(remote)
        self.run_update(action='setup')
        for name, revision in revisions.items():
            self.assertEqual(self.git(self.workspace / name, 'rev-parse', 'HEAD'), revision)

    def test_existing_checkout_after_branch_rename(self):
        for repo in REPOS:
            self.git(self.workspace / repo, 'branch', '-m', 'linux-mobile-experimental')
        remote = self.remotes['android_translation_layer']
        (remote / 'source').write_text('new main revision')
        revision = self.commit(remote)
        self.run_update()
        self.assertEqual(self.git(self.workspace / 'android_translation_layer', 'rev-parse', 'HEAD'), revision)

    def test_art_build_outputs_are_preserved_and_only_locally_ignored(self):
        art = self.workspace / 'art_standalone'
        output = art / ('out-messenger-' + os.uname().machine)
        output.mkdir()
        library = output / 'libart.so'
        library.write_text('existing linked runtime library')
        exclude = art / '.git/info/exclude'
        exclude.write_text('# user exclusion\n/user-build/\n')
        self.run_update()
        self.assertIn('No compilation needed', self.run_update())
        self.assertEqual(library.read_text(), 'existing linked runtime library')
        self.assertIn('/user-build/', exclude.read_text())
        self.assertEqual(exclude.read_text().count('/' + output.name + '/'), 1)
        self.assertEqual(self.git(art, 'status', '--porcelain'), '')
        self.assertFalse((art / '.gitignore').exists())
        # An actual edit must still block updating, without being discarded.
        (art / 'source').write_text('user edit')
        self.run_update(ok=False)
        self.assertEqual((art / 'source').read_text(), 'user edit')
        self.git(art, 'restore', 'source')
        # Unknown untracked source files still reach the strict ATL validator.
        (art / 'new-source.cpp').write_text('user work')
        self.run_update(ok=False)
        self.assertEqual((art / 'new-source.cpp').read_text(), 'user work')

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

    def test_legacy_managed_patch_is_removed_without_discarding_user_edits(self):
        repo = self.workspace / 'android_translation_layer'
        source = repo / 'source'
        source.write_text('legacy density fix')
        marker = self.root / 'applied-shelf.patch'
        marker.write_text(self.git(repo, 'diff') + '\n')
        source.write_text('user modified managed patch')
        self.run_update(ok=False)
        self.assertEqual(source.read_text(), 'user modified managed patch')
        self.assertTrue(marker.exists())
        source.write_text('legacy density fix')
        self.run_update()
        self.assertEqual(source.read_text(), 'original')
        self.assertFalse(marker.exists())

    def test_upstream_display_changes_do_not_require_a_shelf_patch(self):
        remote = self.remotes['android_translation_layer']
        (remote / 'source').write_text('upstream display implementation')
        revision = self.commit(remote)
        self.patch_file = self.root / 'display.patch'
        self.patch_file.write_text('obsolete patch that cannot apply')
        self.run_update()
        self.assertIn(revision, (self.root / 'built-revisions').read_text())
        self.assertFalse((self.root / 'applied-shelf.patch').exists())

    def test_companion_edits_are_not_allowed_with_latest_heads(self):
        bionic = self.workspace / 'bionic_translation'
        (bionic / 'new-source.cpp').write_text('user work')
        self.run_update(ok=False)
        self.assertEqual((bionic / 'new-source.cpp').read_text(), 'user work')

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
