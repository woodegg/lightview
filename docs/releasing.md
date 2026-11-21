# Formal releases

Lightview 0.1.13 ships the application and the private WebKitGTK, JavaScriptCore,
and libsoup runtime based on the passing Ubuntu 24.04 Normal-mode endurance run.
The 24.04 engine/decoder are retained; the 26.04 engine adds a startup-handshake
failure guard and its decoder is built natively for FFmpeg 8.
Building Lightview alone still uses distribution libraries. A binary-only
application update does not install the runtime fixes.

## Install the matching Ubuntu x86_64 release

Choose the archive matching `/etc/os-release`: `ubuntu24.04` or `ubuntu26.04`.
Both are dynamically linked. The 26.04 package includes private ICU74 and a
matching XML/XSLT pair; it uses system FFmpeg 8 instead of requiring the old
24.04 FFmpeg libraries. See [the compatibility notes](../runtime/ubuntu26.04/README.md).

Install the normal distribution dependencies first:

```sh
sudo apt-get install libwebkit2gtk-4.1-0 libjson-glib-1.0-0 libmanette-0.2-0 \
  gstreamer1.0-plugins-good gstreamer1.0-plugins-bad gstreamer1.0-libav \
  bubblewrap xdg-dbus-proxy
```

Download the matching tarball and its `.sha256` file into the same directory.
For Ubuntu 26.04, replace `ubuntu24.04` with `ubuntu26.04` below:

```sh
sha256sum -c lightview-0.1.13-ubuntu24.04-x86_64.tar.gz.sha256
tar -xzf lightview-0.1.13-ubuntu24.04-x86_64.tar.gz
cd lightview-0.1.13
./install.sh --check
sudo ./install.sh
lightview --version
lightview https://example.org
```

The installer installs under `/usr/local`. It selects private runtime files at
their compiled paths under `/usr/local/lib/lightview-diagnostics`; the historical
directory names are retained to preserve the exact qualified ELF bytes. It does
not replace distribution libraries, run `ldconfig`, copy a profile, or restart an
existing browser. Conflicting private runtime files cause installation to stop.
`DESTDIR=/absolute/staging/root ./install.sh` supports an unprivileged staging
install. The installed launcher refers to its real `/usr/local` paths, so run
staged packages inside an appropriately isolated filesystem view.

Only nested containers needing the procfs workaround should explicitly use
`sudo ./install.sh --sandbox-launcher`, then launch
`/usr/local/libexec/lightview-sandbox-launcher`. The helper is otherwise installed
as ordinary mode 0755. Its fixed root-owned browser target remains unchanged;
it drops privileges before the browser selects its root-owned private runtime.
The WebKit process sandbox remains enabled.

The application is MIT licensed. Bundled runtime components retain their
upstream licenses. `LICENSES/` and `THIRD-PARTY-NOTICES.txt` accompany the binary;
the corresponding upstream archives, patches and build notes are published as
`lightview-0.1.13-ubuntu24.04-runtime-source.tar.gz` or
`lightview-0.1.13-ubuntu26.04-runtime-source.tar.gz` alongside it.

## Maintainer procedure

1. Review the release train, worktree and confidential-data exclusions. Preserve
   the original failed and passing test evidence. Do not count recovered or
   joined attempts as continuous healthy uptime.
2. Set `src/lightview-version.h` and the changelog. For runtime changes, qualify the
   exact engine/network/decoder combination before release. Record Normal and
   Low Memory coverage separately.
3. Build with `make`, run `tests/stability_runner.py`, and run the complete real
   GTK/WebKit integration suite against the staged package in a clean private
   mount namespace. Verify all three process-role library mappings, sandbox
   identity, decoding, icons, version output and installer checksums. Staged
   private runtime mounts must hide host copies so fallback cannot mask omissions.
   Give every isolated D-Bus/Xvfb session separate mode-0700 `XDG_CACHE_HOME`
   and `XDG_RUNTIME_DIR` directories. Sharing the user's AT-SPI cache between
   concurrent isolated sessions can interrupt the accessibility test bus.
4. Use `package-release.py --stage-only` with a fresh output directory for
   pre-commit installation QA. Commit the reviewed release changes, then build
   fresh final package directories from the clean release commit. The scripts
   refuse to reuse a package tree or archive an uncommitted worktree:

   ```sh
   python3 tools/package-release.py --platform ubuntu24.04 --source-cache /path/to/qualified/source-cache \
     --output dist/release-0.1.13
   python3 tools/package-runtime-source.py --platform ubuntu24.04 --source-cache /path/to/qualified/source-cache \
     --output dist/release-0.1.13
   ```

Build the 26.04 application and runtime launcher natively on Ubuntu 26.04,
   then repeat both commands with `--platform ubuntu26.04` and a fresh output
   directory. The binary packager refuses a mismatched native build host. Its
   source cache must include the native decoder and pinned compatibility
   sources/licenses described in `runtime/ubuntu26.04/`.

5. Check archive manifests, checksums, dependency resolution, modes and symlinks.
   Stage/extract and exercise the actual release files. Include public,
   sanitized validation evidence; exclude profiles, cookies, private CSVs,
   control sockets, local browsing logs and credentials.
6. Push/merge the reviewed release changes. Tag the exact commit and create
   a GitHub release with binary, corresponding-source and checksum assets.
   Download the published assets, verify their hashes, and confirm the release
   points to that commit. Publishing does not deploy to desktops or containers.

The November 19 run completed 86,402.473 seconds with 11,957 healthy navigations
across seventeen sites, zero resets/rebuilds/timeouts/recycles/OOM events, and
no sustained material post-warmup PSS/FD/thread growth. It qualifies Normal mode
for this tested workload; it does not supply a separate Low Memory 24-hour run
or establish universal absence of leaks.

Release-package verification also covers preserving the GTK application name
through the native runtime launcher and running that launcher through the
optional setuid sandbox helper. An initial shell launcher failed the AT-SPI
application-name check. The first package also rewrote matching runtime files
during a concurrent reinstall, overlapping a SIGBUS in the active test browser.
The installer now leaves matching runtime files untouched. Its live-reinstall
check keeps the same PID, socket and generation while repeatedly evaluating
JavaScript during installation. These packaging issues were found before
publication and do not change the long-tested engine/network/decoder bytes.
