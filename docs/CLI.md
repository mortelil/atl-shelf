# SSH and agent control

Use `atl-shelf cli help` to discover commands, writable fields, and exit codes. CLI commands use **QCoreApplication** and work without DISPLAY, a Qt GUI plugin, or a running Shelf window. They operate on the same user's library as the GUI. Run as the phone's normal user, not root.

Every command returns one JSON object on stdout:

```json
{"schemaVersion":1,"ok":true,"result":{}}
```

Errors include `error` and may include `result` describing partial success. Download/build/app output goes to stderr. `--json` is accepted for explicitness; JSON is already the default. `logs --follow` returns newline-delimited JSON events after its initial tail. Do not infer success solely from a nonempty result: inspect `ok` and the exit status.

## Inspect and diagnose

```sh
atl-shelf cli status
atl-shelf cli doctor
atl-shelf cli list
atl-shelf cli show immich
atl-shelf cli settings
atl-shelf cli runtime show
atl-shelf cli launch immich --dry-run
atl-shelf cli logs immich --lines 200
atl-shelf cli logs immich --follow
```

`doctor` includes paths, installed apps, matching live processes, available tools, runtime configuration and display measurements. `settings` distinguishes stored settings from effective settings: Shelf supplies runtime paths and automatic rendering density. A launch dry run includes the executable, arguments, working directory, environment overrides, display profile and log path. The remaining environment is inherited; the launcher script can add its runtime library paths.

Shelf discovers the logged-in user's session bus and Wayland socket under `/run/user/<uid>` when those variables are absent. On Plasma it reads the output mode and fractional scale from KScreen. If KScreen is unavailable over SSH, it uses the last saved display profile, then a 960×540/1× fallback. The display result identifies its source (`kscreen`, `qt`, `saved`, or `fallback`). Thus SSH launches use the same native-density defaults as GUI launches. An app still requires a running graphical session; library/configuration/log commands do not. Explicit session variables take precedence.

## Launch and stop

```sh
atl-shelf cli launch immich
atl-shelf cli stop immich
atl-shelf cli launch immich --wait
atl-shelf cli launch immich --wait --env '{"G_MESSAGES_DEBUG":"all"}'
atl-shelf cli launch immich --dry-run --activity com.example.MainActivity
atl-shelf cli stop immich --force
```

A detached launch returns its PID, indicating only that the process started. `--wait` records the launch log, streams output to stderr, and returns the child's exit status inside the JSON result. Ctrl+C/TERM stops the waited-for process group and returns 130. Already-running apps are not launched twice. Stop targets same-user processes whose command line contains this app's exact managed APK path and the configured runtime/launcher. It tries TERM first; `--force` allows KILL after three seconds.

`--env` and `--activity` apply to this launch only. Environment values must be strings; null unsets a variable. A transient `ATL_RENDER_SCALE` override is allowed for debugging, while saved/default launches continue to use automatic screen density. Such an override changes rendering behavior and does not change the physical display measurements shown in the result.

## Install, update and remove

```sh
atl-shelf cli search --source fdroid --query immich
atl-shelf cli search --source github --query owner/repository
atl-shelf cli search --source apkmirror --query signal
atl-shelf cli install --source fdroid --value app.alextran.immich --id immich --name Immich --daily
atl-shelf cli install --source github --value owner/repository --name Example
atl-shelf cli install --source local --value /home/user/Downloads/example.apk --id example
atl-shelf cli install --source apkmirror --value /home/user/Downloads/signal.apk --name Signal --source-url https://www.apkmirror.com/
atl-shelf cli check-updates --all
atl-shelf cli update --all
atl-shelf cli update immich
atl-shelf cli replace example --apk /home/user/Downloads/new-version.apk
atl-shelf cli icons --all
atl-shelf cli refresh
atl-shelf cli clear-data example
atl-shelf cli remove example --keep-data
atl-shelf cli remove immich
```

GitHub picks the compatible release asset. F-Droid uses the same signed-index/hash checks as the GUI. APKMirror search is supported, but APK downloads must be imported from a local file; there is no automatic APKMirror updater. CLI installs default to manual updates unless `--daily` is given. Daily updates require GitHub/F-Droid and a systemd user manager.

`check-updates` checks version metadata without installing an APK. Bulk checks/updates skip manual sources and return a result for each app. `replace` replaces only the APK, preserves app data and source settings, and clears stale version metadata. Stop running apps before replacing, updating or removing them. `icons` refreshes APK launcher icons; `refresh` regenerates menu entries.

`clear-data ID` immediately clears the managed APK’s private directory (`app.apk_`) and local `.cache`, preserving the APK, icon, library settings, logs and menu entry. Close the app first. Custom data roots are refused to protect shared data. Shared media and system keyring entries are not deleted. There is no CLI confirmation; the GUI asks before clearing.

`remove` immediately removes the library record, update timer, menu entry and managed app directory; there is no interactive prompt. `--keep-data` leaves the app directory. Existing IDs and orphaned data directories are never silently reused. The GUI refreshes its library when CLI writes arrive; return to the library to refresh an open details page.

## Configuration

```sh
atl-shelf cli configure immich --set '{"fitScreen":true,"daily":false}'
atl-shelf cli configure example --set '{"launchEnv":{"G_MESSAGES_DEBUG":"all"}}'
atl-shelf cli configure example --set '{"launchEnv":{"G_MESSAGES_DEBUG":null}}'
atl-shelf cli settings --set '{"runtimeEnv":{"ATL_DISABLE_FULLSCREEN":"1"}}'
atl-shelf cli settings --file /home/user/runtime-settings.json
printf '%s' '{"name":"My app"}' | atl-shelf cli configure example --file -
```

Objects are validated partial updates. Environment objects merge by key; null deletes a key. IDs, versions, icon paths and other generated metadata are read-only. Unknown options/fields and invalid JSON are errors. In particular, unsupported flags are not ignored: `remove ID --dry-run` fails without removing anything. Width/height are physical pixels and take effect with `fitScreen:false`.

Writes use the library lock shared with the GUI and daily updater. Reads remain available while a build is running. Corrupt library/configuration files cause an error instead of being silently replaced. Direct manual file edits bypass these protections.

## Runtime administration

```sh
atl-shelf cli runtime setup --source existing --binary /path/to/android-translation-layer --launcher /path/to/run.sh --working-directory /path/to/build
atl-shelf cli runtime setup --source github --jobs 2
atl-shelf cli runtime setup --source gitlab --jobs 2
atl-shelf cli runtime update --jobs 2
# With an interactive SSH TTY for the administrator password:
atl-shelf cli runtime setup --source github --install-deps --jobs 2
atl-shelf cli runtime setup --source apk
```

Source builds reuse Shelf's managed runtime setup/update script, dependency pins and native-density patch. Close all Android apps first. Source setup assumes build dependencies are installed unless `--install-deps` is passed. Package setup always invokes Alpine apk. Privileged package installation uses `doas` under a non-root account; use `ssh -t` for its password prompt. Source builds run as the normal user. Cancellation waits for a package transaction to finish before returning; source build process groups can be interrupted immediately.

Build output is saved under the runtime directory (`cli-runtime.log` or `cli-dependencies.log`). A failed build leaves settings unchanged, but builds are in place and may need a successful retry before apps can start. Compatibility of individual Android apps remains an ATL concern.

## Exit codes

| Code | Meaning |
|---|---|
| 0 | Success |
| 2 | Invalid command, option or configuration |
| 3 | App or log not found |
| 4 | Busy: concurrent writer or app still running |
| 5 | Operation failed; inspect result for partial changes |
| 6 | App/build/package subprocess failed; result contains its exit code |
| 130 | Interrupted |
