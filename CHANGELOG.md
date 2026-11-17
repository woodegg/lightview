# Changelog

## Unreleased

### Added

- Add an experimental libsoup HTTP/2 response-header guard with one bounded
  bodyless GET/HEAD retry and immediate cancellation wakeup. It coalesces
  eligible waiters once a connection is retired and preserves
  active responses and non-replayable requests. Its single retry uses HTTP/1.1
  with a separate header-wait bound; normal requests retain HTTP/2.
  Restore pending asynchronous HTTP/1.1 reads across pause/resume and preserve
  cancellation while paused. Portable transport and native pause regressions
  accompany the patch; full endurance is pending.
- Add optional origin-only subresource lifecycle tracing for document-loading
  investigations, independent of the page's bounded Resource Timing buffer.
- Add an experimental WebKit IPC invalidation cleanup for closed connections
  retained in shared synchronous-message scheduling state, with a deterministic
  native lifetime regression and companion-message/reentrant-dispatch coverage.
  This runtime correction still requires the full endurance acceptance gate.
- Add an isolated gst-libav 1.24.1 correction for the decoded-frame metadata
  assertion seen in endurance crashes, with a deterministic aborting control
  and pixel-verified resolution-change regression. The candidate reallocates
  mismatched frames and normalizes fresh output metadata to the negotiated
  picture, rejecting incompatible copy destinations; it still
  requires independent endurance qualification.
- Preserve an experimental WebKitGTK 2.52.6 source-patch set and build notes
  for detached keepalive cleanup, Linux IPC descriptor-pressure retries, and
  ordinary media looping. This is an isolated runtime candidate, separate
  from the distribution engine and pending the 24-hour endurance gate.
- Document an independent libsoup 3.8.1 candidate and its regression coverage
  for investigating slow or stalled resource streams; keep it separate from
  the installed distribution network library and normal Lightview install.
- Add opt-in origin-only navigation diagnostics and a page-process
  responsiveness field, allowing a stalled request to be distinguished from
  an unresponsive process or blocking script dialog without logging URL paths
  or query data.
- Add optional independent NetworkProcess maintenance at navigation boundaries.
  It bounds long-running connection, thread, FD, and receive-buffer retention
  without replacing the WebView or changing its generation/reset count. It is
  disabled by default after major-site testing showed 128 MiB causes excessive
  churn.
- Bound WebKit 2.52's retained IPC receive-queue threads with a separate,
  configurable NetworkProcess thread threshold. The 512-thread default keeps
  maintenance infrequent while preventing unbounded thread growth during
  continuous cross-site automation.
- Give initial top-level navigation the full configured control-timeout budget
  before retrying, preventing slow 30--60 second responses from causing an
  unnecessary WebView rebuild.
- Retry an uncommitted destination twice in the retained WebView before
  replacing its page process, absorbing consecutive transient network stalls
  without changing the main PID, socket, profile, or WebView generation.
- Proactively rebuild WebKit before a new navigation when process-tree PSS
  reaches a configurable threshold, while preserving the profile, main PID,
  control socket, and requested destination.
- Add duration-based stability testing with timestamps and proactive-recycle
  telemetry.
- Add a post-warmup resource-trend gate across complete repeated site rounds,
  requiring both robust positive slope and material PSS/FD/thread growth before
  diagnosing a sustained leak.
- Check continuing top-level loads after half the page-operation timeout
  (at least five seconds), stopping remaining resources after DOMContentLoaded
  without replacing WebKit.
  Expose the active watchdog and cumulative stop count through automation.

### Fixed

- Preserve parser-blocking stylesheets and deferred scripts when a response
  has committed but DOMContentLoaded has not completed. Cancel internal load
  probes on navigation and explicitly terminate retired workers during forced
  engine resets so a pending probe cannot retain a wedged process.
- Let the stability verifier wait for document parsing within the existing
  control budget, independently of the shorter resource-observation budget.
  A document still loading at the deadline remains a failed verification.
- Require readable document content for stability-test success and site
  coverage, including while subresources continue loading. Reject skipped
  probes and titled empty bodies, and preserve existing CSV evidence if a
  driver is restarted.
- Keep standalone audio/video navigation pending through WebKit's media-loader
  handoff instead of reporting its preliminary `WILL_HANDLE_LOAD` notification
  as a page failure. This lets `open --wait` reach the real media document.
- Replace a continuously loading document directly through WebKit's navigation
  API, without a separate pre-navigation stop that can strand the replacement
  in provisional network state.
- Require `open --wait` and the stability runner to observe the requested
  document's committed address. A local blank commit with the destination in
  the address bar no longer becomes a falsely successful navigation. Reject
  such rows independently in the final CSV verdict.
- Expose the current document's `committed_uri` independently of the retained
  last-visited address, including local recovery documents and suspended engines.
  Wake through a local bootstrap before dispatching a remote URL, so slow
  destinations do not consume the shorter engine-startup timeout.
- Retry a provisional navigation from a deterministic local document before
  sending the remote destination again, retaining the WebView, process
  generation, profile, main PID, socket, and agent connection during the
  bounded in-place recovery attempt.
- Let the stability runner observe the complete bounded navigation-retry window
  independently of its shorter post-commit load deadline, avoiding a false
  navigation failure while the browser is still performing an in-place retry.
- After two bounded retained-WebView retries, replace only the WebView/page
  process and retry while retaining the context, network process, profile, main
  PID, socket, and agent connection. Reset the full WebKit engine only when the
  fresh page process also cannot commit. This avoids replacing wider browser
  state for an intermittent WebKit provisional-navigation stall after leaving
  a continuously loading site.
- Give startup and navigation-recovery blank documents a bounded dispatch
  fallback, and start the destination watchdog only when its request is really
  issued. A blank document that never reports `LOAD_FINISHED` can no longer
  consume the destination's budget and cause a false full-engine reset.
- Stop a strict duration-based stability run immediately after preserving the
  evidence row for a WebKit reset, automation timeout, load error, or changed
  main PID, instead of wasting the remainder of a known-failed 24-hour run.
- Cancel the previous document's load watchdog as soon as a new `open` is
  accepted, and never let the load watchdog stop an uncommitted main resource.
  This prevents a continuously loading page's stale timer from cancelling the
  replacement navigation and forcing a later `navigation-timeout` reset.
- Make the stability runner wait through the browser's navigation-recovery
  window before moving to another site. Leave the old load active after the
  shorter resource-loading deadline for the next `open` to replace through
  WebKit's atomic document replacement; a separate pre-navigation stop can
  strand late callbacks and block the replacement.
- Cancel a timed-out page automation request before considering an engine
  reset. Cooperative WebKit cancellation now returns retryable
  `operation_timeout` while preserving the page generation. If WebKit does not
  acknowledge cancellation, reload the current document to release its script
  context; only an operation that remains wedged after that fallback resets
  WebKit.
- Expose a monotonic automation-timeout counter and make the strict stability
  verdict reject any increment. Avoid manufacturing those timeouts by probing
  over-budget endlessly loading pages from UI-process state while retaining DOM
  automation coverage for normally completed pages.
- Queue an automation URL received before the WebView's implicit initial blank
  document finishes, preventing the first external navigation from remaining
  permanently provisional when an agent connects immediately at startup.
  Seed new WebViews with an explicit local blank document before exposing the
  control socket, rather than relying on WebKit's non-signalling implicit state.
- Select Mesa software rendering automatically when no DRM render node is
  accessible, avoiding long or stuck EGL cold-start attempts while retaining
  WebKit's sandbox and shared-memory renderer.
- Disable aggregate-PSS WebKit maintenance by default. WebKit applies its
  memory-pressure and kill thresholds per process, so deriving a 2304 MiB
  process-tree threshold caused healthy multi-process browsing to rebuild the
  engine every few navigations. Keep the total-PSS recycle threshold available
  as an explicit operator option.
- Tear down the live WebView and briefly drain WebKit shutdown callbacks before
  process exit, reducing abrupt helper-process exits and profile write races.
- Clear the automatic-recovery failure streak after a replacement WebKit engine
  becomes ready, so independent successful recoveries do not eventually disable
  the browser.
- Report a permanent engine failure as non-retryable `webkit_failed` instead of
  telling automation to retry forever.
- Make the stability runner fail on a permanent engine/control failure, a
  changed main PID, any recorded load error, any WebKit reset, or an incomplete
  duration target. Report strict `passed`/`failure_reasons` separately from the
  weaker final-process `survived` state.
- Release a timed-out automation connection from client accounting immediately
  and reset a WebKit process whose page-bound operation does not return, avoiding
  permanent control-socket exhaustion.
- Replace WebKit's long-lived network process during a proactive engine
  recycle, releasing accumulated receive queues and file descriptors while
  preserving the profile, persistent website data, main PID, and socket.
- Make `open --wait` wait for a newly committed navigation sequence instead of
  accepting the previous document's already-complete ready state.
- Make `open --wait` verify the requested site and path, and prevent a stale
  cross-site commit from disarming the active navigation watchdog.
- Distinguish a page-operation timeout from a top-level navigation race in
  backend diagnostics without logging potentially sensitive page URLs.
- Align stability-runner page probes with the browser's configured operation
  timeout, avoiding false failures from an arbitrary five-second client limit.
- Require the stability runner's navigation poll to reach the requested site,
  so a previous site's late commit cannot be mistaken for completion of the
  current request.
- Sample current process-tree PSS at each top-level navigation instead of using
  a potentially stale two-second telemetry sample for proactive maintenance.
- Return retryable `webkit_reset` when a JavaScript callback races ahead of a
  WebKit process-termination signal, rather than leaking `INVALID_RESULT` as an
  automation failure.
- Complete proactive maintenance when a new profile has no cookies instead of
  waiting on WebKit's non-null cookie replacement API.
- Select WebKit's non-GL media sink when no DRM render node is accessible,
  avoiding GL dispatch failures while old cross-site web processes are
  destroyed in GPU-restricted containers without forcing slow software
  rendering for the whole page.
- Allow 60 seconds by default for page-bound automation and 65 seconds in the
  matching command-line client, so transient long tasks on heavy pages do not
  cause an unnecessary WebKit reset while genuinely wedged operations remain
  bounded.
- Supervise the interval between a navigation request and its first commit. If
  a busy page prevents the next URL from committing, reset WebKit and replay
  the requested destination through the same PID, profile, and socket.
- Stop a committed page's remaining load before it can consume the full
  page-operation watchdog, allowing automation to continue on the retained
  document instead of escalating a slow resource into a WebKit reset.
- Record each stability row's final reset, recycle, and load-stop counters after
  its page probe completes, so watchdog activity during a busy probe is
  attributed to that navigation instead of falling between CSV rows.
- Record DOM element, text, and HTML sizes during major-site probes and fail an
  actually empty document explicitly, without mistaking a late page title for
  a blank page.

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
