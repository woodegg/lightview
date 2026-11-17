# Experimental WebKitGTK runtime

These source patches are the runtime candidate for LTV-017. Lightview still
links dynamically to WebKitGTK; building or installing Lightview alone does
not apply these fixes to the distribution's engine. This candidate is not a
released runtime and has not yet passed the required 24-hour endurance gate.

The patches address three independently reproduced problems:

| Patch | Behavior |
| --- | --- |
| `disable-segmented-loop-seeking.patch` | Use ordinary end-of-stream looping instead of segmented seeking, which stalled the local H.264/AAC loop fixture. Playback and explicit seeking remain enabled. |
| `soup-detached-keepalive-cleanup.patch` | Release a detached keepalive loader after Soup ignores its completed response, allowing the old WebProcess connection and receive thread to be destroyed. |
| `glib-ipc-inflight-retry.patch` | Preserve an IPC message after Linux `ETOOMANYREFS` and retry asynchronously after 10 ms. The socket can remain writable during this error, so a writable monitor alone is insufficient. |
| `ipc-send-failure-diagnostics.patch` | Log existing fatal send/allocation returns without logging message contents, URLs, cookies, or request bodies. |

The IPC fix does not raise file-descriptor limits. The cleanup fix does not
restart NetworkProcess. The loop fix does not disable media, Media Source,
JavaScript, WebGL, or the process sandbox.

## Source and configuration

The tested source is Ubuntu's `webkit2gtk_2.52.6-0ubuntu0.24.04.1.dsc`, from
the [Ubuntu source archive](https://archive.ubuntu.com/ubuntu/pool/main/w/webkit2gtk/).
Its `webkit2gtk_2.52.6.orig.tar.xz` SHA-256 is
`179a2ea3f8f6edd4be7f31fdc55afc57bd0729f1fba648c61d4181539ac116fc`.
Extract the source package with `dpkg-source -x` so the distribution patches
are applied, then apply the four patches in this order:

1. `disable-segmented-loop-seeking.patch`
2. `ipc-send-failure-diagnostics.patch`
3. `soup-detached-keepalive-cleanup.patch`
4. `glib-ipc-inflight-retry.patch`

Each patch uses paths relative to the extracted source root and applies with
`patch -p1`. Keep that patched source and the build configuration with any
distributed runtime. WebKit's source retains its upstream license terms;
Lightview's MIT license does not relicense WebKit.

Building requires WebKit's Ubuntu build dependencies, CMake, Ninja, Clang 18,
LLD 18, Ruby, and gettext. Installing dependencies requires network access and
root privileges. On a host running endurance jobs, use `NEEDRESTART_MODE=l`
for package installation so service restarts do not interrupt the experiment.

Configure an independent build directory and an independent install prefix.
The tested compiler options are `-O2 -DNDEBUG -DG_DISABLE_CAST_CHECKS -g1
-fno-omit-frame-pointer` for both C and C++, Clang 18, LLD 18, Release, and no
LTO. The relevant CMake settings are:

```text
PORT=GTK
USE_GTK4=OFF
CMAKE_INSTALL_LIBDIR=lib
CMAKE_BUILD_WITH_INSTALL_RPATH=ON
CMAKE_INSTALL_RPATH=<independent-prefix>/lib
ENABLE_BUBBLEWRAP_SANDBOX=ON
ENABLE_VIDEO=ON
ENABLE_WEB_AUDIO=ON
ENABLE_MEDIA_SOURCE=ON
ENABLE_WEBGL=ON
ENABLE_MINIBROWSER=ON
ENABLE_DOCUMENTATION=OFF
ENABLE_INTROSPECTION=OFF
USE_AVIF=OFF
USE_JPEGXL=OFF
USE_GSTREAMER_TRANSCODER=OFF
ENABLE_SPEECH_SYNTHESIS=OFF
```

The last four settings follow this Ubuntu release's package configuration.
WebRTC and encrypted-media implementations retain the GTK port's default
disabled build settings; the experiment does not claim DRM or conferencing
coverage. System GStreamer 1.24.2 is used. Build with `cmake --build` and install
only into the independent prefix with `cmake --install`. Do not replace system
libraries or run a global `ldconfig` for this experiment.

For an ordinary unprivileged launch, select the runtime by setting
`LD_LIBRARY_PATH=<independent-prefix>/lib` before invoking the tested Lightview
binary. Verify `/proc/<pid>/maps` for the browser and both WebKit process types;
the reported WebKit version alone cannot distinguish patched 2.52.6 from the
distribution build. The setuid nested-container launcher discards loader
environment variables. It must not be weakened to load arbitrary user-selected
libraries; the local experiment instead uses an existing clean mount namespace
and drops privileges before selecting its runtime.

## Validation recorded on 2026-10-26

The source-built engine SHA-256 is
`50cbf91f28f3addf24a7459771a41e5a3c426445976a90b914eff9537862f971`.
This identifies this local build, not a reproducible-build guarantee.

- Under real kernel descriptor pressure, the distribution runtime lost IPC
  progress and reset. The candidate delivered all four 32 KiB response bodies
  and a synchronous cookie reply after pressure was released, in 2.014 seconds
  with tracing and 2.029 seconds without tracing; resets and timeouts were zero.
  Pressure was confined to a dedicated test UID with no other workloads.
- After three sandboxed WebViews issued twelve held beacons and were destroyed,
  the distribution runtime retained four receive queues. The candidate returned
  to one UI connection queue within two seconds of releasing all responses;
  NetworkProcess PID stayed unchanged and all WebProcesses were gone.
- HTTP-backed H.264/AAC playback, seeking, and looping passed for inline and
  standalone media with both default playbin and playbin3.

These are regression results. See [the investigation](../../docs/stability-investigation.md)
for the distinction between a passing fixture and a healthy endurance run.

The follow-up head-only-document investigation also evaluates a separately
built [libsoup 3.8.1 network runtime](../libsoup-3.8.1/README.md). That comparison
does not change these four WebKit source patches or establish a passing
24-hour result.
