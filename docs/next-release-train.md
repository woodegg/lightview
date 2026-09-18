# Next Release Train

This document records accepted requirements for the next Lightview release.
Items remain requirements until implementation, validation, and release work
are completed.

Release status: completed and released as 0.1.9 on 2026-09-18. The final scope
adds LTV-013 to the requirements released in 0.1.8.

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
