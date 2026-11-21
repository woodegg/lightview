# Ubuntu 26.04 compatibility runtime

Lightview 0.1.13 provides separate native x86_64 packages for Ubuntu 24.04 and
26.04. The 0.1.12 package requires ICU74 and FFmpeg 6 SONAMEs absent from a
normal Ubuntu 26.04 installation. Installing that archive on 26.04 is not a
supported upgrade path.

The 26.04 package retains the qualified JavaScriptCore and libsoup ELF bytes.
Its WebKitGTK 2.52.6 engine starts from the same six-patch source and adds the
child-launch and device-monitor corrections described below. Six pinned ABI
libraries are installed privately under
`/usr/local/lib/lightview-diagnostics/ubuntu26.04-compat-0.1.13/lib`:

- ICU74 (`libicudata`, `libicui18n`, `libicuuc`), package 74.2-1ubuntu3.1.
- libxml2.so.2, package 2.9.14+dfsg-1.3ubuntu3.10.
- Matching libxslt.so.1 and libexslt.so.0, package
  1.1.39-0exp1ubuntu0.24.04.3.

`compat-manifest.json` pins each real filename, SONAME, package version and
SHA256. Keep XML and XSLT together: the native XSLT library is built for the
new XML ABI and must not receive structures allocated by the old XML library.
System ICU78/XML libraries remain installed. Private libraries are selected
only for Lightview and its children; no system `ldconfig` entries or package
downgrades are used. These compatibility copies need their own security-update
review when Ubuntu publishes fixes; they do not follow system apt updates.

The decoder is built natively from Ubuntu `gst-libav1.0 1.28.2-1`, with the
ported metadata/allocation guards and system FFmpeg 8. See the
[decoder build and test instructions](../gst-libav-1.28.2/README.md) and
`native-decoder.json`. Do not copy the old FFmpeg stack into this prefix.
The native frontend and launcher use the shared release version header and a
platform-specific GStreamer registry. The optional procfs sandbox helper
continues dropping privileges before launching the browser.

## Child-process launch failure

Native 26.04 fault injection found an upstream fatal path: killing a new
sandboxed child before its PID-credentials handshake completed made
`Connection::remoteProcessID` call `g_error`, aborting the UI process. Apply
`../webkitgtk-2.52.6/child-launch-handshake-failure.patch` **after** the six common
WebKit patches. Failed credentials yield no PID, and the launcher rejects the
connection through `didFinishLaunchingProcess(0, { })`. WebProcessProxy's
existing failed-launch path then informs Lightview's bounded recovery state
machine. No unauthenticated connection is accepted and sandboxing remains on.

Build the engine with the common WebKit configuration, using the independent
prefix in `engine-launch-guard.json` for helper lookup and helper RPATHs.
The tested build incrementally recompiled ConnectionGLib, ProcessLauncherGLib
and the helper-path translation unit, relinked the engine with the qualified
preload/invalidation objects, and relinked its three process helpers. The
original engine/build tree was not overwritten. A full build of the supplied
patched source may have different debug bytes; exact-byte reproducibility is
not claimed. Use the unchanged JSC/injected bundle from the common base prefix.
The 24.04 package retains its original six-patch engine.

## GStreamer 1.28 device-monitor startup

The first native major-site run reset while leaving YouTube for Facebook,
with `gst_device_provider_stop` reporting an invalid provider followed by
`double free or corruption`. Cgroup OOM counters remained zero. GStreamer
1.28 starts device providers asynchronously; WebKit already waits for its
`DEVICE_MONITOR_STARTED` message when compiled against 1.28 headers, but the
retained 24.04 build omitted that branch at compile time.

Apply `../webkitgtk-2.52.6/gstreamer-runtime-device-monitor.patch` after the
child-launch patch. It chooses the existing upstream startup barrier using
`gst_check_version()` at runtime, preserving the 1.24 behavior on older runtimes.
The message's public enum value comes from the 1.28 header. It does not disable
capture APIs, plugins or media. The initial capture-only candidate still
crashed: WebAudio's output-channel
probe has a separate immediate start/enumerate/stop path. The final patch waits
in both paths, recompiling both capture-device-manager and audio-destination
translation units.

A standalone native control performs the old WebAudio sequence against stock
26.04 GStreamer. Under GDB it reports an invalid provider, then SIGSEGV on its
second iteration in `gst_device_provider_stop`, called by
`gst_device_monitor_stop`. Rebuilding the checked-in fixture also reproduces
an allocator abort through that same stop path. With the startup barrier,
all 200 cycles complete. The system library's cancellation path can free a started-provider
list also owned by the stopping thread when another provider fails startup.
Waiting for the documented completion message avoids that cancellation race.
No system GStreamer library is replaced. A passing short check does not prove
that every device-provider race is eliminated.

## Source and license cache

The binary packager expects the six private files and SONAME symlinks in the
prefix above, with package copyright notices under
`<SOURCE_CACHE>/compat-ubuntu26.04/licenses/`. Copy files from the exact Ubuntu
24.04 binary packages listed in the manifest, verifying their SHA256 values;
extract those packages into a staging directory, without installing them into
the 26.04 package database or adding 24.04 binary apt repositories.

Fetch the matching `icu`, `libxml2` and `libxslt` source packages using Ubuntu
source repositories. Put their `.dsc` files and all referenced archives under
`<SOURCE_CACHE>/compat-ubuntu26.04/sources/`. Put the native gst-libav `.dsc`
and referenced archives at the cache root, and its extracted source tree at
`<SOURCE_CACHE>/gst-libav1.0-1.28.2/`. Retain the common WebKit/libsoup cache
layout documented by the release packagers. All source descriptions are
checked against their SHA256 sections, including detached signatures, before
publication. Upstream/package license notices accompany the binary and
corresponding sources; they are not relicensed under Lightview's MIT license.

Run `install.sh --check` as an ordinary user before installation. Wrong OS or
architecture, corrupt payloads and missing ELF dependencies fail before any
installed files change. Use the matching archive and native distribution
packages to resolve errors; do not create fake SONAME symlinks.

The preceding engine/network combination passed the recorded Ubuntu 24.04 Normal-mode
24-hour workload. Ubuntu 26.04 receives its own functional and short major-site
regressions; the old result is not a separate 26.04 endurance qualification.

## Native device-monitor diagnostic

Run in a disposable Ubuntu 26.04 environment with the normal audio-provider
plugins and development packages. The control can crash intentionally; do not
run it in a production service. The barrier mode must complete all 200 cycles.
Provider availability and thread scheduling affect control reproduction.

```sh
cc -g -O0 tests/gstreamer_device_monitor.c -o /tmp/lightview-monitor-probe \
  $(pkg-config --cflags --libs gstreamer-1.0)
gdb --batch -ex run -ex 'thread apply all bt' --args /tmp/lightview-monitor-probe
/tmp/lightview-monitor-probe wait
```

Also run `BrowserIntegration.test_webaudio_output_device_probe` through the
real packaged browser. It creates and closes AudioContexts in Normal and Low
Memory modes, reads the output-channel limit, and rejects resets or generation
changes. The real XML/XSLT/Unicode regression covers the private ABI boundary.
