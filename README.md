# ATL Shelf

ATL Shelf is a Plasma-friendly app library and setup assistant for Android APKs that run through the Android Translation Layer. It can prepare the ATL runtime, install apps from GitHub Releases or F-Droid (and manually from APKMirror/local APKs), create Plasma launchers, and check for updates.

## SSH / CLI control

`atl-shelf cli help` exposes the app manager over SSH with JSON output, stable exit codes, and no GUI dependency. Inspect/configure apps and the runtime, search/install/update/remove APKs, preview or run launch commands, stop apps, and read/follow logs. See the [CLI and agent guide](docs/CLI.md) for all commands and examples.

## Everyday use

The library is the home screen. Tap **Add app**, choose a source, then **Install app**. F-Droid starts with search; GitHub accepts a repository link and recommends a compatible release APK. App names are filled in automatically. APKMirror explicitly uses a browser download; local APKs are checked for device compatibility before installation.

Tap an installed app to **Open app**, **Update app**, manage daily updates or remove it. **Update apps** checks and installs updates for all GitHub/F-Droid apps with a per-app result. Technical launch controls are collapsed under **App settings**. **View launch log** keeps troubleshooting inside Shelf.

Downloads show transferred megabytes and can be canceled. Background and manual operations share a library lock, and library writes are atomic. Failed timer setup is shown as disabled automatic updates rather than a successful toggle. Runtime configuration lives under **Settings**, with a setup invitation on the empty/unconfigured home screen. App launches detect the current output resolution and rendering density, including launches from Plasma. The app settings show physical pixels; Shelf converts window dimensions to GTK logical coordinates and renders Android content at the display density. An explicit custom pixel width/height remains available under app settings.

The source-oriented library and installation flow are inspired by [Obtainium](https://obtainium.imranr.dev/).

Shelf uses one native window and one running GUI instance. Add/install, file selection, setup, progress and notices are pages in that window. Back/Cancel stays at the upper left; Escape and the system Back key use the same navigation. Scrollable pages and lists support finger gestures. Swiping a list does not activate its rows.

The physical panel size and the logical window size differ on scaled desktops. On the tested Nura device, KScreen reports a 1080×2280 panel at 2.65 desktop scaling; Qt reports 408×860 available logical pixels. Shelf reports 1080×2280 and sets `ATL_RENDER_SCALE=2.65`, while GTK receives the logical window dimensions. The mobile runtime patch wraps the activity stack in ATL’s pixel-coordinate root, so Android renders at native density without enlarging its controls. The usable app area excludes desktop panels and window decorations. Other displays are detected automatically; Qt screen information is used as a fallback outside Plasma.

## First-time runtime setup

Choose **Set up runtime…** in the app and select one of:

- **Build the mobile fork from mortelil on GitHub** downloads the matching `linux-mobile-experimental` ATL, bionic_translation, and art_standalone branches, installs the build dependencies through Alpine `apk` with a graphical Polkit prompt, and builds a private runtime under `~/.local/share/atl-shelf/runtime`.
- **Build from ATL GitLab** downloads the three upstream `master` source repositories and builds them into the same private runtime.
- **Install with Alpine APK** installs `android-translation-layer`, `bionic_translation`, and `art_standalone` from the configured Alpine repositories. APK resolves and installs their package dependencies.
- **Use an existing binary** points ATL Shelf at an ATL executable already on the device.

The runtime setup screen shows build output and shows the automatic display scale and lets you choose window mode, and optional access to `~/Pictures`. New app launchers inherit the runtime environment and working directory, plus the app's own data directory and resolution. Setup and source builds run inside ATL Shelf; they do not require a terminal. Source-built files stay in the app's private runtime prefix rather than replacing system libraries.

Building source requires Alpine edge/testing packages to be available. The GitHub fork includes the tested mobile build script and dependency lock. The GitLab option follows the upstream Alpine build recipe. Full source builds can take a while, particularly on a phone.

## Runtime updates

**Look for ATL updates** fetches the configured GitHub or GitLab repositories and rebuilds changed revisions. GitHub companion repositories follow ATL's dependency lock. Shelf includes a GPL-3.0 display compatibility patch for the GitHub mobile fork. The updater reverses only its recorded patch before fetching, reapplies the bundled patch, and includes its digest in the build identity. Additional edits to patched files stop the update. If an upstream revision no longer accepts the patch, updating stops with a diagnostic instead of building a runtime with broken scaling. Successful build revisions are recorded so an unchanged runtime is skipped; failed builds can be retried. Close Android apps before updating. Updates currently rebuild in place, so a failed compilation may require a successful retry before launching apps again.

Source edits stop an update. The two tracked Java files generated by aapt are backed up under `generated-backups` before checkout. The update dialog shows download and compiler output and supports canceling the build. A canceled source build can be resumed by running the update again. During initial setup, cancellation waits for a privileged package transaction to finish before stopping. Existing GitHub profiles and menu entries automatically receive the private runtime's correct prefix on the next Shelf startup. Launches from Shelf write `launch.log` in the app's data folder.

## Build

Requires Qt 6 Widgets and Network development files, CMake, and a C++17 compiler. Install KDE Frameworks 6 WindowSystem development files for Plasma/Wayland activation support (recommended). CMake detects it automatically; qmake detects Alpine’s `/usr/include/KF6/KWindowSystem`. Runtime dependencies for APK icon extraction are `aapt`, `unzip`, and Qt’s SVG image plugin. Plasma display detection uses `kscreen-doctor`. On Alpine these are provided by `android-build-tools`, `unzip`, `qt6-qtsvg`, and `kscreen`. GnuPG (`gpg`) is needed for F-Droid’s pinned signed-index verification.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

On Nura (Alpine edge), where qmake is available but CMake may not be installed:

```sh
mkdir -p build-nura && cd build-nura
qmake6 ../atl-shelf.pro
make -j2
```

Run `build/atl-shelf`. Use **Set up Android support** on first launch, then select **Add app**. The add dialog supports GitHub (fetches the latest release and ranks APKs for the current CPU), F-Droid search, APKMirror search, or a local APK. APKMirror search opens the selected release in a browser; download a compatible `.apk` there and import it in ATL Shelf. APKMirror downloads use an interstitial and have no stable public API, so those installs are manual and do not get daily updates. The activity is optional; new apps use the current screen dimensions. The GitHub release list also lets you choose an asset manually.

APK launcher icons are extracted on installation and update, with a background refresh for existing apps. Shelf supports raster icons and adaptive foreground/background icons, including vector paths, groups and gradients. Adaptive icons use a circular launcher mask; their own background colors are preserved. Unsupported drawable types fall back to the verified repository icon or a generic icon.

The app interface is in English. Open an app's details and choose **Remove app** to remove its Plasma launcher and update timer, then delete its APK and ATL app-data directory. ATL Shelf includes a scalable SVG application icon; `cmake --install` installs it into the hicolor icon theme along with the launcher entry.

Daily checks use a per-user systemd timer. The timer is enabled only when the checkbox is selected for a GitHub or F-Droid app. Downloads are staged before replacing the current APK, and the app's ATL data directory stays the same between versions. GitHub release digests are checked when provided. APKs with native libraries are checked against the host architecture before activation.

F-Droid search uses its public search endpoint. Installs and update checks verify the detached GPG signature on `entry.json` with F-Droid's pinned signing subkey, then verify the index-v2 SHA-256 listed in that signed entry before reading package versions or downloading APKs. APK hashes are checked against that verified index. The first F-Droid lookup downloads and caches the full repository index, so it can take some time and disk space. GnuPG must be installed on the host. The launch activity is entered manually. ATL Shelf is built for the host architecture, while ATL and APKs should match the target system.

## Validation

```sh
cmake -S . -B build -DATL_SHELF_TESTS=ON
cmake --build build
QT_QPA_PLATFORM=offscreen ctest --test-dir build --output-on-failure
python3 -m unittest discover -s tests -v
```

Interaction tests use temporary app data and fake runtime/system commands. They exercise phone-width navigation and a single native window, finger scrolling and tapping, single-instance activation, display-profile migration, current-screen menu launches, opening after saving, first-run setup visibility, source selection, local APK installation and menu creation, and download cancellation against a loopback HTTP server. They do not verify compatibility of third-party Android applications. The loopback test requires permission to bind a local socket.

## Known compatibility limits

Shelf installs APKs; it cannot guarantee that ATL implements every Android API an app uses. On the tested Nura device, Signal 8.30.3 reaches onboarding but crashes in ATL’s `android.text.Layout.getLineBottom()`. This is separate from Shelf startup and display scaling. Switching the active Secret Service between KDE Wallet and GNOME Keyring can also hide an existing app key; do not reset app data to work around this.

Existing or distribution-provided ATL binaries need equivalent native-density support for fractional scaling. The bundled patch is applied to Shelf-managed GitHub source builds.

## License

GPL-3.0-only; see [LICENSE](LICENSE). The runtime patch modifies code from [Android Translation Layer](https://gitlab.com/android_translation_layer/android_translation_layer) and the [mortelil mobile fork](https://github.com/mortelil/android_translation_layer). Android app icons remain the property of their respective authors and are extracted locally, not bundled with Shelf.
