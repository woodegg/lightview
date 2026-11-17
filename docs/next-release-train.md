# Next Release Train

This document records accepted requirements for the next Lightview release.
Items remain requirements until implementation, validation, and release work
are completed.

Next train: 0.1.12 development, including LTV-017. Its full endurance
acceptance remains unmet; merging development work does not qualify a release.
The last formal release is 0.1.11, published on 2026-10-02 with LTV-016 and
the requirements released in 0.1.8 through 0.1.10.

## LTV-008 — Version information button

Status: released in 0.1.8.

Add a small version-information button to the browser toolbar. Activating the
button must display the running Lightview version and the linked WebKitGTK
version without navigating away from the current page.

Requirements:

- Use the same version source as the existing `lightview --version` command so
  the command line and graphical interface cannot report different versions.
- Display both the Lightview release version and WebKitGTK runtime version.
- Perform the query locally; the button must not require network access or
  contact GitHub or an update service.
- Keep the current page, history, profile, downloads, and automation socket
  unchanged when the button is used.
- Work in normal, private, and low-memory modes with negligible persistent
  memory cost.
- Provide an accessible label and tooltip suitable for XFCE and remote desktop
  use.

Acceptance:

- A real GTK integration test activates the button and verifies both reported
  versions.
- The reported Lightview version matches `lightview --version` from the same
  executable.
- Repeated activation does not create additional browser views or WebKit web
  processes and does not change the current URI.

## LTV-009 — Supervised WebKit recovery with a stable automation endpoint

Status: released in 0.1.8.

Keep the GTK window, browser toolbar, and automation server in the Lightview
main process while treating WebKit page processes as replaceable workers. A
page crash, memory-limit termination, JavaScript hang, or operator-requested
reset must not require the agent to discover a new Lightview instance or
control socket.

WebKitGTK controls its own multi-process layout, so this requirement does not
attempt to force the web process, network process, and optional GPU helpers
into one operating-system process. It instead supervises the replaceable
`WebKitWebView` and its page process from the stable Lightview main process.

Requirements:

- Keep the Lightview main-process PID, GTK window, toolbar, selected profile,
  and Unix control-socket path unchanged during WebKit recovery.
- Add a clearly labelled and accessible **Reset WebKit** toolbar action. The
  toolbar action and the automation `reset` command must use the same recovery
  state machine.
- Model recovery with at least `ready`, `resetting`, `recovering`, and `failed`
  states. Continue serving `status` while recovery is in progress.
- Implement a soft reset by terminating the current WebKit web process through
  the public WebKitGTK API and creating a fresh page process.
- Implement an escalated engine reset that destroys and recreates the
  `WebKitWebView` and its WebKit context after a soft reset fails or repeated
  terminations indicate that the wider engine is unhealthy.
- Automatically enter recovery after an unexpected web-process crash or
  memory-limit termination. Apply a cooldown and a bounded retry policy so a
  broken page cannot cause an uncontrolled restart loop.
- Configure an effective, documented low-memory termination threshold in
  addition to the existing advisory memory-pressure thresholds. Memory
  termination must be reported distinctly from an ordinary crash.
- Load `about:blank` after recovery by default. Preserve the last committed URI
  in status so an agent can explicitly decide whether to reopen it rather than
  automatically replaying a page that may have caused the failure.
- Preserve cookies and other persistent profile data across recovery. Document
  that DOM state, JavaScript objects, session-only page state, unsaved forms,
  media playback, and in-progress downloads may be lost.
- Cancel outstanding page-bound operations when recovery starts. Return a
  stable, machine-readable, retryable error for `eval`, `click`, `fill`, and
  navigation commands received while the engine is unavailable. Do not
  automatically replay state-changing automation commands.
- Keep the automation endpoint usable without changing connection information.
  Agents continue to use the same socket path; a request that was active when
  the page process stopped may fail and be retried after the engine becomes
  ready.
- Make `reset` completion observable. Either hold the reset response until the
  replacement page is ready or return a reset operation identifier that can be
  waited on without polling page JavaScript.
- Extend `status` with `engine_state`, `web_process_generation`, `reset_count`,
  `last_termination_reason`, `last_reset_at`, and the last committed URI. The
  generation must increase whenever page-bound state is replaced.
- Record backend recovery events with timestamps and termination reasons so a
  long-running automated session can be diagnosed without relying on the GUI.

Acceptance:

- A real GTK integration test starts an authenticated profile, records the
  main PID and socket path, performs a reset, and verifies that both identifiers
  remain unchanged while a new web-process generation becomes ready.
- The same test verifies that persistent cookies and local storage survive a
  reset while old DOM and JavaScript state do not.
- Simulated API termination, unexpected web-process termination, and the
  memory-limit reason all reach a usable `ready` state or a bounded, reported
  `failed` state without exiting the Lightview main process.
- The toolbar remains responsive during recovery and its reset action works in
  normal, private, and low-memory modes.
- Requests made during recovery receive the documented retryable response, and
  a subsequent command succeeds through the original socket after recovery.
- Repeated forced resets do not accumulate live WebKit views, page processes,
  control connections, or signal handlers.

## LTV-010 — In-app resource and running-mode telemetry

Status: released in 0.1.8.

Show the active running mode and current resource use inside the browser as
well as in the native window title so an operator can observe a long-running
automated browser without opening another monitoring interface.

Requirements:

- Show `Normal`, `Private`, `Low memory`, or `Private / Low memory` as
  appropriate for the running process.
- Add a compact toolbar label such as `MODE:LM, 265M, CPU 1.0%`. Use `N`, `P`,
  `LM`, and `P/LM` for normal, private, low-memory, and combined modes. It must
  remain fully visible when Matchbox or another window manager disables native
  title bars.
- Report RAM as the summed proportional set size (PSS) of the Lightview process
  tree, including active WebKit child processes, without double-counting all
  shared library pages as RSS would.
- Report aggregate process-tree CPU usage. Values may exceed 100 percent when
  multiple processes consume more than one logical CPU.
- Refresh the values every two seconds and avoid adding a helper process,
  background thread, or persistent WebKit object.
- Preserve the page title at the beginning of the native title, explicitly
  prefix its mode with `MODE:`, and keep both displays compact enough for XFCE,
  Matchbox, and remote desktop use.
- Expose the same measurements through `status` as `running_mode`,
  `ram_pss_mib`, `cpu_percent`, `resource_processes`, and `window_title`.

Acceptance:

- A real GTK integration test verifies that the title and in-app toolbar label
  include the correct normal, private, and low-memory mode labels plus RAM and
  CPU values.
- Reported RAM is positive while the browser is running, and resource sampling
  continues across both soft and hard WebKit resets.
- Closing Lightview removes the periodic sampler without leaving a monitoring
  process behind.

## LTV-011 — Visible reset notification and recovery-loop prevention

Status: released in 0.1.8.

Correct the UAT failure where CNN repeatedly exceeded the low-memory WebKit
kill threshold and appeared to reset without explaining the event to the
operator.

Requirements:

- Keep the low-memory pressure target at 384 MiB, but apply a 3072 MiB minimum
  last-resort kill threshold so large news and media pages are not killed at the
  previous 1536 MiB threshold.
- Detect repeated automatic recovery over a five-minute window and stop after
  two automatic retries rather than allowing a page just outside a short
  cooldown to reset indefinitely.
- Turn the address bar red for five seconds whenever manual or automatic WebKit
  recovery starts. A later reset restarts the five-second indication.
- Keep the notification inside the application so it remains visible when
  Matchbox runs with `-use_titlebar no`.
- Expose the notification state as `reset_flash_active` and continue reporting
  the reset reason and time through the existing status fields.

Acceptance:

- UAT logs identify CNN terminations as WebKit `memory-limit` events rather
  than Lightview main-process crashes.
- Integration tests verify the 3072 MiB low-memory kill threshold and an active
  reset indication after soft and hard resets.
- The Lightview PID, window, profile, and control socket remain unchanged while
  the address bar notification is shown.

## LTV-012 — Runtime low-memory mode switch

Status: released in 0.1.8.

Allow an operator or automation agent to switch between normal and low-memory
operation without replacing the Lightview main process or its connection
information.

Requirements:

- Add a **Low memory mode (LM)** checkbox to the version-information dialog and
  initialize it from the active mode.
- Changing the checkbox must update the 384/768 MiB default pressure target and
  WebRTC, WebGL, and accelerated-canvas settings. Preserve an explicitly supplied
  `--memory-limit` value across mode changes.
- Recreate the WebView and WebKit context on `about:blank` so all changed engine
  settings take effect while retaining the GTK window, profile, PID, and socket.
- Flash the address bar and record the mode change as a reset event.
- Provide the equivalent `mode` automation command and wait for the new WebKit
  generation before `lightviewctl mode` returns.

Acceptance:

- A real GTK integration test opens the version dialog, activates the mode
  control in both directions, and verifies its checked state, mode label,
  memory policy, and web-process generation.
- PID and socket inode remain unchanged across both mode changes.

## LTV-013 — Configurable WebKit memory kill protection

Status: released in 0.1.9.

Allow operators and automation agents to change or disable the WebKit
excessive-memory termination policy independently of Normal and Low Memory
modes. This addresses long-running infinite-scroll jobs that intentionally
retain more than the default 3072 MiB threshold.

Requirements:

- Add a **WebKit memory kill protection** checkbox, editable MiB threshold, and
  explicit apply action to the version-information dialog.
- Keep the default protection enabled at 3072 MiB for ordinary Normal and Low
  Memory launches.
- Set WebKit's documented kill threshold to zero when protection is disabled,
  while retaining its conservative and strict memory cleanup policies.
- Preserve the configured nonzero threshold while protection is disabled so it
  can be restored without re-entry.
- Rebuild the WebView and WebKit context on `about:blank` after applying an
  active policy change, while retaining the main PID, profile, and socket.
- Expose `memory_protection_enabled`, the effective threshold, and the retained
  configured threshold through `status`.
- Provide equivalent startup options and a `memory-protection` automation
  command that waits for the replacement WebKit generation.
- Explain in the dialog and documentation that disabling WebKit termination
  does not prevent an operating-system or container OOM kill.

Acceptance:

- Real GTK integration tests change the threshold, disable protection, and
  re-enable it while verifying UI state and status fields.
- The effective threshold is zero while disabled and the configured threshold
  is restored when re-enabled.
- The Lightview PID and control socket inode remain unchanged across all policy
  changes.

## LTV-014 — Idle WebKit hibernation and agent leases

Status: released in 0.1.10.

Reduce the cost of long-lived automation instances by destroying an idle
WebKit view and context while keeping the Lightview process, window, profile
lock, and automation socket available. Allow an agent to reserve an instance
independently of whether WebKit is awake.

Requirements:

- Hibernate WebKit after a configurable idle interval, defaulting to 3600
  seconds, and allow `0` to disable automatic hibernation.
- Do not hibernate while loading, playing audio, downloading, or serving a
  page-bound automation request.
- Keep `status`, the main PID, window, profile lock, and socket available while
  suspended. Wake on navigation and expose an explicit `wake` command.
- Add the same idle interval control to the Version window, startup options,
  and `lightviewctl`, with synchronized values and a fresh countdown after a
  change.
- Add expiring acquire, renew, and release agent leases. A lease must survive
  WebKit hibernation and recovery without itself keeping WebKit awake.
- Provide a repeatable process-tree PSS measurement tool for active,
  suspended, and resumed states.

Acceptance:

- Integration tests verify suspension, wake, persistent site data, stable PID
  and socket inode, lease retention, lease expiry, and runtime interval changes.
- A GTK accessibility test edits and applies the Version-window idle control
  without changing the WebKit generation.
- Local measurements confirm that the Web content process exits and total PSS
  falls while a blank instance is suspended.

## LTV-015 — Runtime persistent-profile selection

Status: released in 0.1.10.

Allow an operator or automation agent to move a running private or persistent
session to another persistent profile without changing connection information.

Requirements:

- Show the active profile and a standard folder chooser in the Version window;
  allow the chooser to create a new directory.
- Validate ownership and private permissions and acquire the new profile lock
  before destroying the current WebKit session.
- Rebuild WebKit on `about:blank` after a successful switch while preserving
  the Lightview PID, window, socket, and agent lease.
- Reject invalid or already locked profiles without changing the current page
  or profile, and keep the graphical chooser open for another selection.
- If the profile requested at startup is locked, open the same chooser and
  continue startup with a different selected profile. Cancel exits startup.
- Provide a matching `lightviewctl profile DIR` command.

Acceptance:

- Integration tests switch between isolated profiles and verify that each
  profile retains its own persistent cookies/site data.
- Tests verify stable PID, socket inode, and lease across switches and confirm
  that a lock failure leaves the current generation and storage intact.
- A real GTK chooser test starts with a locked profile, selects another folder,
  and reaches a ready browser using the selected profile.

## LTV-016 — Nested-container WebKit sandbox launcher

Status: released in 0.1.11.

Allow Lightview to retain WebKitGTK's bubblewrap sandbox inside unprivileged
LXC/Incus environments where container-specific submounts below `/proc` make a
nested, unprivileged procfs mount fail with `EPERM`.

Requirements:

- Keep ordinary desktop launches and installations unchanged.
- Provide a separate launcher that must be explicitly installed setuid root.
- Open a fixed Lightview executable before namespace setup and require it to be
  root-owned, executable, and not writable by its group or other users.
- Create a private mount namespace and prevent mount propagation before
  detaching any `/proc` submount.
- Detach only mounts strictly below `/proc`; never replace or unmount the
  container's own `/proc` mount.
- Drop the saved, effective, and real user and group IDs back to the caller,
  clear ambient capabilities, and enable `no_new_privs` before executing
  Lightview.
- Preserve Lightview command-line options, desktop environment, profile,
  control socket, and process supervision behavior.
- Do not disable WebKit's sandbox, grant capabilities to Lightview, or require
  a privileged container.

Acceptance:

- Without the launcher, reproduce bubblewrap's procfs `EPERM` in the affected
  unprivileged container.
- With the launcher, the same bubblewrap sandbox probe succeeds while running
  as the original unprivileged UID with zero effective capabilities.
- Verify that `/proc` submounts remain present in the container's original
  mount namespace after the launched process exits.
- Run the complete GTK/WebKit integration suite and verify the existing direct
  launch path remains unchanged.

## LTV-017 — Long-running navigation recovery

Status: in development.

The current engine candidate includes reviewable
[WebKitGTK source patches](../runtime/webkitgtk-2.52.6/README.md) for a detached
keepalive connection leak, a Linux `ETOOMANYREFS` send-queue stall, and segmented
media-loop seeking. Targeted regressions pass; the full 24-hour gate below is
still required. Building Lightview alone does not patch the distribution
WebKit runtime. Evidence and failed-run distinctions are maintained in
[the investigation](stability-investigation.md).

The next runtime iteration also includes a
[gst-libav frame-buffer correction](../runtime/gst-libav-1.24.1/README.md)
for the fatal video-metadata width/height invariant. The original decoder
aborts under the matching allocation fault; the candidate preserves all 288
decoded frames, dimensions, and checked pixels through resolution changes.
The first correction still exposed mismatched output metadata. A stricter
negative test then found a null-input-frame crash during output-error draining
in the second candidate. The third verifies matching output metadata, avoids
mapping incompatible direct-rendering destinations, and handles that drain
error without crashing. Both earlier candidates are withdrawn.
It passed six upstream suites, four browser media combinations, and all 42
browser integration checks before entering a separately timed endurance run.
The November 17 candidate additionally clears shared synchronous-message
scheduling entries and queued messages when an IPC connection is invalidated.
A deterministic native regression reproduces retention before the fix and
checks connection release and companion-message delivery after it. This targets
the observed UI queue accumulation; endurance qualification and Microsoft
document-stall investigation remain outstanding.
The V3 candidate completed a full 24-hour navigation run on November 10--11,
but failed acceptance because descriptor and thread counts grew materially.
The recorded evidence and distinction from a passing run are documented in
the investigation.

Keep an automation instance usable during repeated heavy cross-site navigation
without allowing retained WebKit allocations to reach the last-resort kill
threshold or misreporting a permanently failed engine as retryable.

Requirements:

- Clear the recovery-loop streak after the replacement blank engine reaches a
  completed ready state; continue bounding failures that occur during recovery.
- Return non-retryable `webkit_failed` for page commands while the engine is in
  `failed`, while keeping status and manual hard reset available on the same
  socket.
- Before a new navigation, optionally hard-recycle WebKit when total
  process-tree PSS has reached an explicitly configured maintenance threshold.
  Keep this policy disabled by default: WebKit's memory-pressure kill threshold
  applies per process and must not be used to derive an aggregate browser
  threshold. Preserve the main PID, profile, socket, and requested destination.
  Make the decision from a current sample, not the periodic UI sample.
- Include the WebKit network process in hard maintenance without replacing its
  data manager, so accumulated receive queues are released while profile and
  persistent website data remain available.
- Independently replace a growing NetworkProcess at navigation boundaries
  without replacing the WebView, changing its generation/reset count, flashing
  the reset warning, or changing the main PID/profile/socket. Preserve both
  persistent and session cookies, defer while downloads or page operations are
  active, expose the threshold/count, and allow the policy to be disabled.
- Expose the maintenance threshold and proactive recycle count through status.
- Stop stability testing with a failing exit status on permanent engine or
  control failure, a changed main PID, any load error, any page-operation
  timeout, any WebKit reset, or an incomplete wall-clock duration target.
  Report a strict machine-readable verdict separately from mere process
  survival.
- For runs of at least four hours, require enough complete repeated site rounds
  to evaluate post-warmup PSS, FD, and thread trends. Fail only when a robust
  positive slope is accompanied by a material early-to-late increase, avoiding
  false leak reports from a single heavy site or ordinary cache warmup.
- Cancel a page operation after its configurable timeout and return retryable
  `operation_timeout` without replacing a responsive engine. Release control
  admission slots independently of callbacks, reload the current document if
  WebKit does not acknowledge cancellation, and escalate to an engine reset
  only if the bounded reload fallback also cannot release the operation.
- Default the page-operation watchdog to 60 seconds and give the bundled client
  a five-second response margin, avoiding resets caused by transient long tasks
  on heavy pages while keeping genuinely wedged operations bounded.
- After a top-level document commits, stop a load that remains active for half
  the operation timeout (with a five-second minimum). Retain the current
  document and WebKit process, expose the stop count, and leave the existing
  operation watchdog as the fallback for a genuinely wedged main thread. A
  shorter stability-runner deadline must leave the load active and still
  verify the committed document through JavaScript before counting success.
  The next `open` asks WebKit to replace it
  directly, without an explicit stop followed by a separate remote load.
  Cancel the old document's load watchdog when accepting that replacement,
  and never let it stop an uncommitted main resource owned by the navigation
  watchdog.
- Supervise navigation before its first commit. If the previous page blocks a
  requested destination from committing, supersede the provisional request
  with a deterministic local document, wait briefly for late callbacks, and
  retry up to twice in the retained WebView. Do not issue `stop_loading()` and
  a replacement remote request in the same provisional state. If it still
  cannot commit, replace only the WebView/page process while retaining the
  context, network process, profile, main PID, control socket, and agent
  connection. Reset the full WebKit engine and replay the URL only if the fresh
  page process also fails. Expose a monotonic WebView-rebuild counter.
- Give the initial commit attempt the full configured control-timeout budget
  and each retained-process retry half that budget. Do not rebuild a healthy
  page process merely because a remote server takes 30--60 seconds to begin its
  response.
- Bound the local blank bootstrap used at startup and after a WebView rebuild.
  Dispatch the retained destination even if the blank document omits its finish
  callback, and do not charge blank-process startup time to the destination's
  navigation watchdog.
- Expose a monotonic committed-navigation sequence so `open --wait` cannot
  return against a stale completed document.
- Require `open --wait` to reach the requested site and path, and keep a late
  cross-site commit from cancelling the watchdog for the current destination.
- Record generation, committed-navigation sequence, loading state, and whether
  a top-level load began while a page operation was pending when its watchdog
  expires; do not include page URLs in this diagnostic.
- Exercise page probes through the browser's configured operation watchdog,
  with enough client margin to receive a structured timeout/reset response.
- Give navigation polling enough time for the complete bounded retained-view
  retry sequence, independently of the shorter post-commit load deadline used
  for continuously loading documents.
- Do not stop parser-blocking stylesheets or deferred scripts merely because
  the main response committed. Confirm DOMContentLoaded before stopping
  remaining resources, cancel stale internal probes on navigation/reset, and
  make forced resets terminate retired workers even if an asynchronous probe
  still holds the old view. Keep the shorter resource-observation budget
  independent of the bounded document-ready wait in the verifier.
- Require the duration runner to observe both a newer commit and the requested
  site's committed document before treating a navigation as complete. A local
  blank commit or prior site's late commit must not qualify merely because
  the current provisional address already matches the requested site. Apply
  the same check to `open --wait` and the final CSV verdict.
- Expose `committed_uri` for the current document independently of the
  last-visited address retained for recovery. Clear it when the engine is
  destroyed and record local recovery commits. Wake on a local bootstrap, then
  dispatch a requested remote URL under its navigation timeout.
- Normalize a JavaScript completion that races with WebKit termination to a
  retryable reset response, including when the invalid-result callback arrives
  before the process-termination signal.
- Allow proactive network-process maintenance with an empty cookie jar.
- When no DRM render node is accessible, select the non-GL GStreamer video sink
  before GTK/WebKit initialization so cross-site process teardown does not
  enter an unusable GL dispatch path. Do not force software rendering for the
  whole page; keep WebKit's on-demand composition policy, preserve the default
  path on hosts where the user can open a render node, and expose the selected
  path to automation.

Acceptance:

- Three automatic page-process failures that each recover successfully remain
  usable through the same PID and socket.
- A proactive recycle reaches the requested URL with a new generation through
  the same PID and socket, with a new network-process PID and preserved persistent
  cookies.
- A network-only recycle reaches the requested URL with a new network PID while
  keeping the WebProcess generation and reset count unchanged and preserving a
  session cookie.
- A local 24-hour `DISPLAY=:1` major-site rotation retains the same main PID
  and control socket, finishes with a live `ready` or `suspended` engine, and
  records zero full resets, WebView rebuilds, automation timeouts, destination
  mismatches, blank documents, unverified documents, or load errors. Exercise
  every configured site with readable document content; address-bar metadata
  alone cannot satisfy coverage. Stop a failed candidate after
  preserving its evidence rather than counting recovered failures as healthy
  uptime. Evaluate post-warmup PSS, FD, and thread trends over at least four
  hours and require no sustained material growth.
- Repeated page-operation timeouts do not accumulate control clients or page
  requests, and status remains reachable after more than 16 timed-out requests.
- A navigation requested while the old WebProcess main thread is wedged is
  automatically replayed after recovery and reaches the destination through
  the same main PID and socket.
- A provisional main resource whose first request exceeds the navigation
  budget succeeds on an in-place retry with the same generation, reset count,
  PID, profile, and socket. Three stalled attempts rebuild only the WebView/page
  process and reach the destination without changing reset count, PID, profile,
  socket, or agent connection. A destination that also stalls in the fresh page
  process reaches the full reset fallback without looping indefinitely.
- A destination that takes longer than the ten-second engine-startup deadline
  after hibernation still commits within its configured navigation budget,
  without a reset or false blank-document success.
- A committed document with a resource that never finishes is stopped within
  the load budget, remains scriptable, and does not change the WebKit
  generation or reset count.
- Replacing a continuously loading committed page near the end of its load
  budget with a slow main document does not let the old timer stop the new
  request, increment the load-stop count, or reset WebKit.
- A GPU-restricted-container rotation has no GL dispatch assertion or hidden
  WebKit subprocess crash, while a host with an accessible render node is not
  forced into software rendering.
