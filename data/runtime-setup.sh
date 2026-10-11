#!/bin/sh
# Builds a private ATL runtime without installing source-built files into /usr.
set -eu

mode=$1
root=$2
jobs=$3
workspace="$root/workspace"
prefix="$root/prefix"
build="$workspace/android_translation_layer/build-mobile"

action=${4:-setup}
patch_file=${5:-$(dirname "$0")/atl-native-density.patch}
applied_patch="$root/applied-shelf.patch"
mkdir -p "$root"
if ! mkdir "$root/.build-lock" 2>/dev/null; then
    echo "Another runtime build is running. Wait for it to finish." >&2
    exit 1
fi
trap 'rm -rf "$root/.build-lock"' EXIT
trap 'exit 130' INT TERM
mkdir -p "$workspace"
case "$mode" in
    github) branch=main; remote=https://github.com/mortelil ;;
    gitlab) branch=master; remote=https://gitlab.com/android_translation_layer ;;
    *) echo "Unknown runtime source: $mode" >&2; exit 2 ;;
esac
# Reverse only the exact managed patch; refuse to overwrite additional edits.
if [ -s "$applied_patch" ]; then
    git -C "$workspace/android_translation_layer" apply --reverse --check "$applied_patch" || {
        echo "Shelf runtime patch was edited. Preserve those edits before updating." >&2; exit 1;
    }
    git -C "$workspace/android_translation_layer" apply --reverse "$applied_patch"
    rm "$applied_patch"
fi
# Resolve all revisions before changing any checkout. Refuse to overwrite edits.
for repo in android_translation_layer bionic_translation art_standalone; do
    dir="$workspace/$repo"
    if ! git -C "$dir" rev-parse HEAD >/dev/null 2>&1; then
        if [ -d "$dir" ]; then
            echo "Incomplete checkout at $dir. Move it aside and retry." >&2
            exit 1
        fi
        git clone --depth 1 --branch "$branch" "$remote/$repo.git" "$dir"
    fi
    # The mobile ART builder keeps compiler-specific outputs here, and links
    # installed libraries into that tree. Keep it intact across updates. Its
    # dependency checker includes untracked files, unlike our tracked-edit check.
    # Use a checkout-local exclusion, never modify upstream .gitignore or disable
    # dependency validation. Tracked edits remain visible even under this path.
    if [ "$mode" = github ] && [ "$repo" = art_standalone ]; then
        case $(uname -m) in
            aarch64|x86_64) art_output="/out-messenger-$(uname -m)/" ;;
            *) echo "Unsupported mobile ART architecture" >&2; exit 1 ;;
        esac
        exclude="$(git -C "$dir" rev-parse --absolute-git-dir)/info/exclude"
        mkdir -p "$(dirname "$exclude")"
        if ! grep -Fqx "$art_output" "$exclude" 2>/dev/null; then
            printf '\n# ATL Shelf: private ART build outputs (retain linked runtime libraries)\n%s\n' "$art_output" >> "$exclude"
        fi
    fi
    # aapt rewrites these tracked generated files during ordinary builds.
    # Preserve their contents before restoring them for checkout.
    if [ "$repo" = android_translation_layer ]; then
        for generated in src/api-impl/com/android/internal/Manifest.java src/api-impl/com/android/internal/R.java; do
            if ! git -C "$dir" diff --quiet HEAD -- "$generated"; then
                if ! head -1 "$dir/$generated" | grep -q 'AUTO-GENERATED FILE'; then
                    echo "Unexpected edit in $generated; update stopped." >&2
                    exit 1
                fi
                backup="$root/generated-backups/$(date +%s)-$$"
                mkdir -p "$backup/$(dirname "$generated")"
                cp "$dir/$generated" "$backup/$generated"
                git -C "$dir" restore --source=HEAD --staged --worktree -- "$generated"
                echo "Saved generated file to $backup/$generated"
            fi
        done
    fi
    untracked=no
    if [ "$mode" = github ] && [ "$repo" = art_standalone ]; then untracked=normal; fi
    if [ -n "$(git -C "$dir" status --porcelain --untracked-files="$untracked")" ]; then
        echo "Local changes in $dir. Commit or move them before updating." >&2
        exit 1
    fi
    if [ "$action" = update ]; then
        echo "Checking $repo ($branch)…"
        git -C "$dir" fetch --depth 1 origin "$branch"
        git -C "$dir" rev-parse FETCH_HEAD > "$root/.build-lock/$repo"
    else
        git -C "$dir" rev-parse HEAD > "$root/.build-lock/$repo"
    fi
done
# The mobile fork specifies exact companion commits, which can lag their branches.
if [ "$mode" = github ]; then
    git -C "$workspace/android_translation_layer" show "$(cat "$root/.build-lock/android_translation_layer"):dependency-lock.json" > "$root/.build-lock/dependencies.json"
    for repo in bionic_translation art_standalone; do
        revision=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["dependencies"][sys.argv[2]]["commit"])' "$root/.build-lock/dependencies.json" "$repo")
        case "$revision" in *[!0-9a-f]*|'') echo "Invalid dependency revision" >&2; exit 1 ;; esac
        if ! git -C "$workspace/$repo" cat-file -e "$revision^{commit}" 2>/dev/null; then
            git -C "$workspace/$repo" fetch --depth 1 origin "$revision"
        fi
        echo "$revision" > "$root/.build-lock/$repo"
    done
fi
for repo in android_translation_layer bionic_translation art_standalone; do
    echo "$repo $(cat "$root/.build-lock/$repo")"
done > "$root/.build-lock/revisions"
for repo in android_translation_layer bionic_translation art_standalone; do
    git -C "$workspace/$repo" checkout --detach "$(cat "$root/.build-lock/$repo")"
done
# An upstream commit can retain every patch addition while changing surrounding
# context. Reverse-apply alone then wrongly labels the integrated fix incompatible.
# Verify all added lines, including duplicate occurrences, in every affected file.
patch_additions_present() {
    python3 - "$patch_file" "$workspace/android_translation_layer" <<'PYTHON'
import collections
import pathlib
import sys
patch, root = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
required = {}
path = None
in_hunk = False
for line in patch.read_text().splitlines():
    if line.startswith('diff --git '):
        path, in_hunk = None, False
    elif line.startswith('+++ b/'):
        path = line[6:]
    elif line.startswith('@@ '):
        in_hunk = True
    elif in_hunk and path and line.startswith('+') and line[1:].strip():
        required.setdefault(path, collections.Counter())[line[1:].strip()] += 1
if not required:
    sys.exit(1)
for name, additions in required.items():
    try:
        actual = collections.Counter(line.strip() for line in (root / name).read_text().splitlines())
    except (OSError, UnicodeError):
        sys.exit(1)
    if additions - actual:
        sys.exit(1)
PYTHON
}
if [ "$mode" = github ] && [ -s "$patch_file" ]; then
    if git -C "$workspace/android_translation_layer" apply --check "$patch_file" 2>/dev/null; then
        git -C "$workspace/android_translation_layer" apply "$patch_file"
        cp "$patch_file" "$applied_patch"
    elif git -C "$workspace/android_translation_layer" apply --reverse --check "$patch_file" 2>/dev/null || patch_additions_present; then
        echo "ATL already includes the Shelf display patch; using upstream implementation."
    else
        echo "This ATL revision needs a newer Shelf display patch. Runtime was not rebuilt." >&2
        exit 1
    fi
    sha256sum "$patch_file" | cut -d ' ' -f 1 >> "$root/.build-lock/revisions"
fi
# A setup recipe change must rebuild even when the three Git revisions match.
sha256sum "$0" | cut -d ' ' -f 1 >> "$root/.build-lock/revisions"
runtime_ready=true
if [ "$mode" = github ]; then
    for artifact in "$prefix/lib/libart.so" "$prefix/lib/libart-compiler.so" "$prefix/share/art/core-all-hostdex.jar" "$prefix/libexec/atl-apk-verifier/run"; do
        [ -s "$artifact" ] || runtime_ready=false
    done
fi
if [ "$action" = update ] && [ "$runtime_ready" = true ] && [ -x "$build/android-translation-layer" ] && cmp -s "$root/.build-lock/revisions" "$root/built-revisions"; then
    echo "ATL is already up to date. No compilation needed."
    exit 0
fi
echo "Building runtime…"
if [ "$mode" = github ]; then
    export ATL_WORKSPACE="$workspace" ATL_PREFIX="$prefix" ATL_BUILD_DIR="$build" JOBS="$jobs"
    mobile="$workspace/android_translation_layer/scripts/mobile"
    sh "$mobile/build.sh"
    sh "$mobile/build-art-runtime.sh"
    mkdir -p "$root/downloads"
    r8="$root/downloads/r8-8.3.37.jar"
    if [ ! -f "$r8" ]; then
        curl -fL --connect-timeout 15 --max-time 300 \
            https://storage.googleapis.com/r8-releases/raw/8.3.37/r8.jar -o "$r8.part"
        mv "$r8.part" "$r8"
    fi
    # ATL validates the pinned R8 and apksig checksums before use.
    R8_JAR="$r8" sh "$mobile/build-core-java.sh"
    sh "$mobile/build-apk-verifier.sh"
	elif [ "$mode" = gitlab ]; then
	bionic="$workspace/bionic_translation"
	art="$workspace/art_standalone"
	 atl="$workspace/android_translation_layer"
	mkdir -p "$prefix/lib/pkgconfig"
	export PKG_CONFIG_PATH="$prefix/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
	export CFLAGS="-I$prefix/include${CFLAGS:+ $CFLAGS}"
	export CXXFLAGS="-I$prefix/include${CXXFLAGS:+ $CXXFLAGS}"
	export LDFLAGS="-L$prefix/lib -Wl,-rpath,$prefix/lib${LDFLAGS:+ $LDFLAGS}"
	if [ -f "$bionic/build-atl-shelf/build.ninja" ]; then
		meson setup --reconfigure "$bionic/build-atl-shelf" "$bionic" --prefix="$prefix" --libdir=lib --buildtype=release
	else
		meson setup "$bionic/build-atl-shelf" "$bionic" --prefix="$prefix" --libdir=lib --buildtype=release
	fi
	meson compile -C "$bionic/build-atl-shelf" -j "$jobs"
	meson install -C "$bionic/build-atl-shelf"
	make -C "$art" -j "$jobs" ____PREFIX="$prefix" ____LIBDIR=lib
	make -C "$art" -j "$jobs" ____PREFIX="$prefix" ____LIBDIR=lib install
	if [ -f "$build/build.ninja" ]; then
		meson setup --reconfigure "$build" "$atl" --prefix="$prefix" --libdir=lib --buildtype=release
	else
		meson setup "$build" "$atl" --prefix="$prefix" --libdir=lib --buildtype=release
	fi
	meson compile -C "$build" -j "$jobs"
	meson install -C "$build"
	cat > "$root/atl-launcher.sh" <<EOF
#!/bin/sh
set -eu
export RUN_FROM_BUILDDIR=1
export LD_LIBRARY_PATH="$build:$prefix/lib:/usr/lib/art:/usr/lib/java/dex/art/natives:/usr/lib/java/dex/art:/usr/lib\${LD_LIBRARY_PATH:+:\$LD_LIBRARY_PATH}"
export BIONIC_LD_LIBRARY_PATH="$prefix/lib\${BIONIC_LD_LIBRARY_PATH:+:\$BIONIC_LD_LIBRARY_PATH}"
cd "$build"
exec "$build/android-translation-layer" "\$@"
EOF
	chmod 755 "$root/atl-launcher.sh"
else
	echo "Unknown runtime source: $mode" >&2
	exit 2
fi

test -x "$workspace/android_translation_layer/build-mobile/android-translation-layer" || \
	test -x "$workspace/android_translation_layer/builddir/android-translation-layer" || \
	test -x "$prefix/bin/android-translation-layer"
cp "$root/.build-lock/revisions" "$root/built-revisions"
echo "Runtime build completed."
