# Changelog

## 0.1.11 - 2026-10-02

### Added

- Add an optional, narrowly scoped setuid launcher for unprivileged LXC/Incus
  containers where WebKit's bubblewrap sandbox cannot mount a fresh procfs over
  container-specific `/proc` submounts.
- Create a private mount namespace, detach only mounts below `/proc`, drop all
  user and group privilege, enable `no_new_privs`, and then execute a fixed,
  root-owned Lightview binary. The container's mount namespace is unchanged.
- Add an explicit `install-sandbox-launcher` target. Normal builds and installs
  leave the launcher non-setuid and continue to start Lightview directly.

## 0.1.10 - 2026-09-24

### Added

- Add opt-in idle WebKit hibernation while keeping the Lightview window, PID,
  profile, and automation socket alive. Opening a URL wakes WebKit on demand.
- Add expiring agent leases independent of short-lived control connections.
  Lease renewal and status checks do not prevent hibernation.
- Add an isolated process-tree PSS measurement script for the blank-page
  active, suspended, and resumed states.
- Default idle hibernation to 60 minutes, with configurable startup and
  `lightviewctl hibernate-after` runtime intervals.
- Add a profile folder chooser to the version window and a matching
  `lightviewctl profile DIR` command. Switching profiles rebuilds WebKit while
  preserving the Lightview process, control socket, and agent lease.
- When a startup profile is already locked, open the same folder chooser to
  select or create another profile. Invalid selections keep the chooser open.
- Add an idle-hibernation interval control to the Version window, synchronized
  with the startup setting and `lightviewctl hibernate-after`.

## 0.1.9 - 2026-09-18

### Added

- Add runtime controls for WebKit's excessive-memory kill policy. The version
  dialog can enable or disable the protection and set its MiB threshold, while
  `lightviewctl memory-protection` exposes the same operation to agents without
  changing the Lightview PID, profile, or control socket.
- Add `--memory-kill-threshold` and `--disable-memory-kill` startup options and
  expose the effective and configured policies through `status`.

## 0.1.8 - 2026-09-18

### Added

- Add an accessible toolbar button that shows the running Lightview and
  WebKitGTK versions from the same source as `lightview --version`.
- Keep the GTK window and automation socket available while supervising WebKit
  recovery. Expose engine state, web-process generation, reset history, last
  termination reason, and the last committed URI through `status`.
- Add a toolbar WebKit reset action, synchronous `lightviewctl reset`, and
  `reset --hard` for rebuilding the WebView and WebKit context without changing
  the Lightview PID or socket path.
- Recover automatically from page-process crashes and WebKit memory-limit
  terminations, with bounded retries and backend event logging.
- Show compact telemetry such as `MODE:LM, 265M, CPU 1.0%` in the toolbar,
  mirror it in the native title, and expose the same process-tree PSS and CPU
  values through `status`. The toolbar remains visible when Matchbox disables
  window title bars.

### Changed

- Configure WebKit's last-resort kill threshold at four times the selected
  per-process memory-pressure target, with a 3072 MiB minimum that keeps heavy
  news and media pages from being terminated prematurely.
- Return machine-readable retry information when a page operation is cancelled
  by recovery or arrives while the engine is unavailable.
- Detect repeated automatic recovery over a five-minute window instead of
  thirty seconds so a page cannot evade the recovery-loop guard.
- Turn the address bar red for five seconds after every manual or automatic
  WebKit reset and expose the active indication through `status`.
- Add a low-memory checkbox to the version dialog and a matching
  `lightviewctl mode` command. Mode changes rebuild WebKit while preserving the
  main window, profile, PID, and automation socket.

## 0.1.7 - 2026-09-17

### Fixed

- Keep images enabled in low-memory mode. `--no-images` remains available as an
  explicit opt-in for workloads where the usability tradeoff is acceptable.
- Save website downloads automatically to the standard Downloads directory,
  use conflict-free filenames, and report progress, completion, or failure in
  the status bar without opening a destination chooser.

## 0.1.6 - 2026-09-16

### Fixed

- Default to WebKitGTK's shared-memory renderer. This also prevents the null
  accelerated-backing-store crash in containers where no renderer override was
  inherited and the default DMABUF path could not use a DRM device.

## 0.1.5 - 2026-09-16

### Fixed

- Translate the unsafe `WEBKIT_DISABLE_DMABUF_RENDERER=1` legacy override to
  WebKitGTK's shared-memory renderer. WebKitGTK 2.52 can otherwise dereference
  a null accelerated backing store when a YouTube video creates a composited
  layer.

## 0.1.4 - 2026-09-16

### Fixed

- Route trusted YouTube link clicks through a native UI-process message before
  starting navigation. This avoids the delayed WebKitGTK fault that could still
  follow repeated Guide navigation with the 0.1.3 page-side workaround.

## 0.1.3 - 2026-09-16

### Fixed

- Keep WebKit's context sandbox state consistent with the required
  `WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1` override on restricted
  containers. This prevents the delayed native crash after YouTube navigation.

## 0.1.2 - 2026-09-16

### Fixed

- Route trusted YouTube link clicks through programmatic navigation to avoid a
  WebKitGTK 2.52 native crash when choosing entries from YouTube's Guide.

## 0.1.1 - 2026-09-16

### Fixed

- Keep media playback, MediaSource, encrypted media, and WebAudio enabled in
  low-memory mode. This prevents a WebKitGTK crash when opening and playing
  media on sites such as YouTube Music.
- Include YouTube Music in the multi-site stability test and verify the media
  APIs required by playback in the low-memory integration test.

## 0.1.0 - 2026-09-16

Initial release of Lightview, a small GTK 3 and WebKitGTK browser for Linux.

### Features

- Single-view browser with a minimal address and navigation toolbar.
- Modern JavaScript support through WebKitGTK 4.1.
- Persistent and private browsing profiles.
- Local JSON automation socket with navigation, JavaScript evaluation, DOM
  interaction, status, process reset, and shutdown commands.
- Default memory-pressure policy plus optional `--low-memory`, `--no-images`,
  and custom `--memory-limit` modes.
- Embedded application icon and XFCE desktop launcher integration.
- Process-tree RSS/PSS reporting and repeatable multi-site stability testing.

### Known limitations

- One browser view; tabs, popup windows, downloads UI, bookmarks, and a password
  manager are not implemented.
- The Linux x86_64 archive is dynamically linked and requires compatible GTK 3,
  WebKitGTK 4.1, JSON-GLib, and related runtime libraries.
- The 384 MiB low-memory setting is a per-process pressure target rather than a
  total browser memory cap. Heavy sites can still use substantially more memory.
