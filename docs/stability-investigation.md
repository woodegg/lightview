# Endurance investigation

LTV-017 remains in development. A recovered navigation, a surviving main
process, or a passing short diagnostic does not satisfy the 24-hour acceptance
criteria in [the release train](next-release-train.md#ltv-017--long-running-navigation-recovery).

## Evidence collected on 2026-10-25

The formal candidate `20261025-130714` failed after 1,971.85 seconds and 306
recorded navigations. Leaving Cloudflare for CNN required a WebView rebuild
after two retained-view retries. There were no full resets or automation
timeouts in that run. The strict runner correctly rejected the rebuild.

The tested runtime was WebKitGTK 2.52.6
(`2.52.6-0ubuntu0.24.04.1`) and Mesa 25.2.8. Three later diagnostics preserved
the unresponsive processes long enough to inspect their stacks:

| Diagnostic | Configuration | Failure observation |
| --- | --- | --- |
| `20261025-135054` | Original 16-bit X11 display | Cloudflare to CNN, round 1; synchronous `DomCookiesForHost` IPC wait |
| `20261025-135510` | Separate 24-bit Xvfb display | Cloudflare to CNN, round 10; synchronous `BlobSize` IPC wait |
| `20261025-140828` | Original display, Skia CPU painting | Cloudflare to CNN, round 2; synchronous `BlobSize` IPC wait |

These are failed diagnostic runs, not successful endurance candidates. Their
300-second operation timeout was used to preserve the fault for inspection;
it is not a proposed default. Neither the display change nor the Skia CPU
painting change prevented the failure. WebGL and WebGL2 readback worked in
the CPU-painting diagnostic, but that configuration is not a stability fix.

Ubuntu debug symbols resolved the WebProcess main-thread wait to
`IPC::Connection::waitForSyncReply()`. The initial unsymbolized graphics stacks
did not establish a graphics deadlock: the actual main-thread waits were for
NetworkProcess replies.

In the 24-bit diagnostic, the NetworkProcess connection remained valid and
connected, but its outgoing queue stopped advancing. Inspection found 654
queued messages, including 597 resource-data messages and the synchronous
reply the WebProcess needed. The connection's pending-output flag was false.
Waking the receive queue's eventfd did not release the queue. A diagnostic call
to `sendOutgoingMessages()` on that connection's own receive thread drained
the queue; CNN committed and a subsequent JavaScript probe reported a complete
document. Main PID, WebView generation, and reset counters were unchanged.

This establishes a stalled outgoing IPC queue as the immediate failure
mechanism. A later diagnostic identified the send error described below.
Private C++ calls,
debugger intervention, and manually waking eventfds are diagnostic techniques,
not supported recovery mechanisms to ship in Lightview.

The follow-up diagnostic `20261025-142708` recorded NetworkProcess `sendmsg`
failures from startup while retaining the full 17-site workload; tracking was
later extended to shared-memory allocation syscalls. It stalled during round
6 when leaving Amazon for GitHub. No tracked kernel operation had failed, and
this time the NetworkProcess outgoing queues were empty. Symbolized stacks
identified a separate media failure: the WebProcess main thread waited in
`gst_element_send_event()` through `MediaPlayerPrivateGStreamer::doSeek()`.
The mutex owner was a media pool thread waiting in `gst_pad_pause_task()` for
the source streaming thread. Host TIDs and namespace TIDs must be mapped when
inspecting mutex owners in WebKit's sandbox. The runtime used GStreamer
1.24.2. A separate newer patch-level media runtime was subsequently tested
without replacing system libraries.

The next implementation decisions must account for both observed mechanisms.
Do not treat longer timeouts, blank-page retries, or another renderer setting
as a verified fix for either failure.

Three local pressure controls each completed ten rounds of overlapping
resource downloads, 1,000 cookie read/write pairs, and 200 Blob beacons per
round. Controls included brief receiver suspension and reducing the IPC send
buffer to 8 KiB. The small-buffer control exercised kernel `EAGAIN` responses
and recovered without a reset or WebView rebuild. Ordinary backpressure alone
therefore does not reproduce the observed stall in this fixture. These short
controls do not replace the live-site endurance test.

The GStreamer 1.24.13 comparison `20261025-145444` ran on the original
16-bit display with the WebKit sandbox enabled. Process mappings confirm the
separate media runtime is loaded; WebGL/WebGL2 pixel readback and H.264, VP9,
and MSE capability checks passed. It still stalled during round 22 when
leaving Cloudflare for CNN, about 39 minutes after startup. Two retries and
a WebView rebuild followed; the runner rejected it after about 49 minutes
and 374 recorded navigations. The intervening recovery wait is not healthy
uptime. Updating GStreamer alone did not resolve live-site instability.

A controlled six-second H.264/AAC fixture exposed a reproducible loop/seek
failure: after seeking to 3 seconds and then 5.8 seconds, playback stopped
advancing near the end despite `paused=false` and `ended=false`. The same
page reproduced this in the distribution's MiniBrowser, independently of
Lightview's automation protocol. Both GStreamer 1.24.2 and 1.24.13 exhibited
the failure. This local playback stall has not yet been proved identical to
the live site's blocking media seek or IPC queue stall.

An initial seek while paused makes WebKit disable its segmented-loop
optimization for that player. With that setup the fixture looped normally.
A separate, isolated binary probe made only
`MediaPlayerPrivateGStreamer::isSeamlessSeekingEnabled()` return false;
ordinary HTML media looping, playback, and seeking then passed without the
paused-seek setup. This is a useful causal lead, not a release artifact or
an endurance result. The equivalent C++ patch is now included in the
source-built candidate described below; broader verification is still
required before shipping it. System WebKit libraries remain unchanged.

The probe also passed standalone media, inline media with playbin3,
standalone media with playbin3, and inline media with GStreamer 1.24.13.
These remain short local checks, not endurance evidence.

Diagnostic `20261025-155358` evaluated the probe against the full 17-site
workload on display `:1`, using the original GStreamer runtime. Installing
build dependencies invoked the host's `needrestart`, which restarted the
driver at 16:06:39 EDT while leaving the browser PID unchanged. Its original
CSV was truncated by the restarted driver; the append-only driver log and
observer log remain. That workload cannot supply continuous-run evidence.
Later dependency installations use `NEEDRESTART_MODE=l`, and the runner now
refuses to overwrite an existing CSV.

A separate phase in the same diagnostic used pinned tool snapshots and
required JavaScript document probes even while resources remained loading.
It stalled on Cloudflare in round 2. At 20:13:39 UTC the automatic stack
capture found the WebProcess waiting for `DomCookiesForHost`; a later
read-only inspection found 264 queued NetworkProcess messages, no pending
output, and no pending functions in that connection's work-queue run loop.
The browser reported the WebProcess unresponsive with one outstanding page
operation. The UI was subsequently frozen and the diagnostic drivers stopped
to preserve the fault before watchdog recovery. The frozen instance is not
an ongoing endurance candidate. Disabling segmented looping did not fix
the IPC stall.

The same NetworkProcess retained 33 invalid, disconnected IPC connections,
each with one strong reference and its receive-queue thread, in addition to
two active connections. Their socket-monitor callbacks and queued run-loop
functions were empty. A sampled reference was traced to an older
`NetworkConnectionToWebProcess` object. This narrows the lifetime investigation
but does not yet distinguish temporary retained network work from a leak.

Stock-runtime lifecycle control `gi-keepalive-20261025-163706` kept its GTK UI
responsive and sandbox enabled while creating and destroying three WebViews
in one ephemeral context. Each view issued four local beacons whose responses
were held until all views were destroyed. Even 37 seconds after all twelve
responses were released, three disconnected IPC connections and their receive
threads remained, with one strong reference per connection. HTTP FDs returned
from 30 to 18. This reproduces retention without Lightview's reset machinery
or an unresponsive UI. Matching control `gi-keepalive-20261025-163841`
performed the same three view lifecycles without beacons and retained zero
disconnected connections, both immediately and after 37 seconds.

Source inspection found a matching cleanup gap: a detached keepalive loader
returns `PolicyAction::Ignore` after receiving its response, whereas Soup's
ignore branch only clears the request and supplies no later completion
callback. The loader can therefore remain in the session's keepalive set,
retaining its old `NetworkConnectionToWebProcess`, IPC connection, and worker
thread. The isolated source build now includes a Soup-only candidate change
to clean up that completed detached loader explicitly. Its expected effect
is that the beacon fixture also returns to zero disconnected connections.
The matching fixture now verifies that effect on the source build, as
recorded below. Live-site endurance evidence is still required. This cleanup candidate
does not itself establish a fix for the stalled outgoing IPC queue.

The stricter lifecycle regression `gi-keepalive-20261025-165739` additionally
counted all twelve response headers written by the fixture and all receive
threads after the three WebViews were gone. On the original runtime it exited
with a failure: four receive queues remained instead of the one UI connection
queue. Its result explicitly reports `cleanup_passed=false`. The same check
also runs against the source-built candidate; it can assess thread cleanup
without using hard-coded private layouts for the new binary.

An isolated build from the exact Ubuntu 2.52.6 source package has completed.
It retains the distribution's relevant GTK3/Soup3 features and sandbox,
includes the loop, detached-loader cleanup, and IPC retry candidates, and adds diagnostics
only at existing IPC send, attachment-limit, and shared-memory error returns.
Those diagnostics do not
change recovery or queue behavior. The build is not installed over the
distribution runtime and is not a formal release.

Diagnostic `20261025-164835` repeated the full workload with
diagnostic interposition on GSocket sends, shared-memory truncation/mapping,
and WebKit descriptor duplication. It records failed return values and call
sites, without message contents or URLs. Local controls confirmed identical
return values for normal sends, backpressure followed by successful retry,
closed sockets, and failed descriptor duplication; errno checks also covered
failed truncation and mapping. The tracer is loaded in the browser and both
backend process types with the WebKit sandbox enabled. An additional
error-only debugger breakpoint covers the normally unlogged attachment-limit
and out-of-line-body failure paths. This is timing-instrumented diagnostic
evidence, not a formal endurance candidate.

A separate standalone-MP4 check exposed an application error: WebKit's normal
`WEBKIT_PLUGIN_ERROR_WILL_HANDLE_LOAD` media-document handoff was reported as
a failed navigation. A 1.6 KiB generated MP4 fixture reproduced the failure
through `open --wait`; ignoring only that preliminary notification made the
regression pass while retaining navigation supervision. This fixes an
automation error, not the IPC or seek deadlocks described above.
The application change passed 15 unit tests and 36 integration tests.

The endurance verifier itself was strengthened after inspection found that
UI-only samples could count as successful navigation without a DOM probe.
It now requires readable, populated documents, verifies coverage of every
configured site, rejects titled empty bodies, and creates CSV files
exclusively. The updated verifier passed 22 unit checks and two real-browser
fixtures: a continuing resource load with usable DOM and a titled empty
document that must fail. Earlier UI-only samples cannot establish content
health or complete site coverage under this stronger gate.

Raw logs, CSVs, symbolized stacks, and diagnostic intervention records are
stored outside the repository under each run ID. Diagnostic profiles contain
test browsing data and must not be committed. A new formal 24-hour run is
required after a verified behavioral fix.

## Confirmed IPC error and source-runtime validation

The instrumented 17-site run `20261025-164835` captured the missing cause:
NetworkProcess `g_socket_send_message()` returned Linux errno 109,
`ETOOMANYREFS`, at 17:06:26 EDT. GIO classified it as a generic failure rather
than `WOULD_BLOCK`. The WebProcess was waiting for a synchronous
`DomCookiesForHost` reply; the generic failure path did not preserve the
current message or schedule a retry, leaving subsequent replies stranded.
The run ended after 1,393.129 seconds with one reset and one automation
timeout. Its 152 recorded documents include only 151 healthy navigations;
the recovered final document must not count as healthy uptime. The fault
began before the diagnostic's 300-second recovery wait.

This limit concerns file descriptors sent through Unix sockets but not yet
received. It is accounted per UID against the sender's soft `RLIMIT_NOFILE`,
which was 1024 here; it is not the ordinary open-FD count. A separate real
kernel fixture confirmed both the error and the socket remaining writable.
After the receiver drained the descriptors, sending succeeded. Increasing
the limit would mask this trigger without repairing the lost queue progress.

The candidate now preserves the pending message and schedules a 10 ms
connection-queue retry for this Linux error. Ordinary backpressure still
uses the existing writable monitor. All three behavioral fixes and the
error-only diagnostics are reviewable under
[runtime/webkitgtk-2.52.6](../runtime/webkitgtk-2.52.6/README.md).

The full source build completed and was installed in an isolated prefix on
2026-10-26. The source-built engine SHA-256 is
`50cbf91f28f3addf24a7459771a41e5a3c426445976a90b914eff9537862f971`.
The installed file matches the completed build. No system engine or normal
desktop executable was replaced.

| Regression | Distribution runtime | Source-built candidate |
| --- | --- | --- |
| Real kernel IPC pressure, 1,040 queued FD references under an isolated UID | Reset after pressure was released; failed | All four 32 KiB bodies and cookie reply correct; 2.014 s with tracing and 2.029 s without tracing; zero resets/timeouts |
| Destroy three WebViews with twelve held beacons, then release responses | Four receive queues remain after 37 s; failed | One UI queue within 2 s, unchanged NetworkProcess PID, all responses written and WebProcesses gone; passed |
| HTTP H.264/AAC play, seek, loop; inline/standalone, default/playbin3 | Original loop fixture stalls near its end | All four combinations passed using system GStreamer 1.24.2 |

The same candidate passed all 22 verifier unit checks. The real-browser API
suite `source-runtime-qa/20261026-134733` passed 37 of 38 checks initially;
the accessibility check could not locate the application because the QA
launch explicitly set `NO_AT_BRIDGE=1`. Removing that test-environment setting
made the remaining check pass in 1.002 seconds. The original failure and
follow-up logs are preserved. This was a harness configuration correction,
not a runtime or application fix. All checks used WebKit's sandbox.

These results establish specific regression improvements. They do not
establish a healthy 24-hour run or prove the absence of other leaks or stalls.

Candidate `stability-source-24h/20261026-135313` began at 13:53 EDT on
2026-10-26, on display `:1` in Normal mode, with the source-built engine mapped
in the browser and both backend process types. The driver uses all seventeen
configured sites, a 60-second operation watchdog, DOM probes, and the strict
24-hour and post-warmup resource gates. Browser, source, patches, and tools
are pinned; no interposer, debugger, or private binary patch is used. Initial
WebGL, WebGL2, H.264, VP9, and Media Source capability checks passed. This is
a failed run, as recorded below.

The driver stopped at 16:09 EDT after 8,173.151 seconds (about 2 hours
16 minutes), with 1,421 healthy navigations out of 1,422 recorded attempts.
The main PID stayed unchanged; resets, WebView rebuilds, automation timeouts,
navigation retries, and network/aggregate maintenance recycles were all zero.
All seventeen sites were exercised. In round 84, Microsoft's document had
committed and supplied a title, but its DOM remained `loading`, with 15,721
characters of HTML and no body. The strict verifier rejected that document.
The browser's 30-second load watchdog subsequently stopped the load. A later
read-only probe still found no body, so this cannot be dismissed as a passing
page or counted as continuing healthy navigation while the process stayed idle.

Across 83 complete rounds, post-warmup baseline PSS increased by about 12 MiB
and median PSS by about 5 MiB; FD and thread medians each increased by 5.5.
Those changes did not exceed the resource-growth thresholds, but only two
hours of post-warmup evidence were available, below the required four hours.
The run therefore neither passes the 24-hour gate nor excludes further leaks.
The terminal driver summary and independent completion audit are preserved
under this run ID. Its duration is currently the longest recorded continuous
major-site test under the strict document-verification gate.

## Follow-up: protecting document parsing

The captured Microsoft navigation had received its main response by 355 ms
(348,449 decoded bytes), and the recorded stylesheets completed in under a
second. That snapshot does not identify what subsequently held its parser.
It does establish that a committed response and a responsive JavaScript engine
are insufficient evidence of usable page content. The old 30-second load stop
left that document with no body.

A local fixture independently reproduced a harmful watchdog behavior: an
eight-second stylesheet blocks an inline head script, while the test browser's
five-second resource watchdog cancels the load before any body is parsed.
The old executable timed out waiting for the otherwise valid document.
This demonstrates an application bug; it does not prove that the live site's
parser was blocked by the same resource.

The watchdog now checks in an isolated JavaScript world that the current
document has a body and DOMContentLoaded has completed before stopping
remaining resources. An inconclusive check defers the stop. Each check has
a five-second cancellation timer; navigation, reset, and teardown cancel the
old check and invalidate its epoch, preventing an old callback from stopping
a replacement document. Forced reset/profile teardown explicitly terminates
the retired WebProcess: destroying only its GTK widget can leave a wedged
worker retained by a pending asynchronous JavaScript task. The new regression
initially caught that worker-retention case during implementation; it now
verifies the retired worker is gone.

The verifier also distinguishes its short resource-observation deadline from
document readiness. A still-parsing document can become ready within the
existing configured control budget from commit. A document that remains
loading at that deadline, a titled empty document, or any recovered timeout
still fails. Earlier failed runs are not reclassified as passing.

Candidate application SHA-256
`1439948cf749e467265b395a3f654cf864ba0dcc34687d6dfffd3ecc1b66b56a`
passed 25 verifier tests and all 42 sandboxed real-browser integration tests
in `source-runtime-qa/20261026-192150`. This includes delayed head parsing,
deferred-script completion, cancellation of a wedged internal probe during a
forced reset, bounded parser waiting in the verifier, and the existing
resource-stop, navigation, profile, hibernation, media, and download checks.
The integration suite completed in 229.744 seconds. The four WebKit patches
and source-built engine SHA remain unchanged.

The failed idle instance was retired after preserving its evidence and before
copying its dedicated test profile. A new independently timed candidate,
`stability-source-24h/20261026-192631`, started at 19:26 EDT on 2026-10-26 with
PID 2735273 on display `:1`. It uses Normal mode, all seventeen sites, the
60-second control budget, the 12-second resource observation, sandboxing,
pinned artifacts, read-only monitoring, and independent completion auditing.
It failed after 224.869 seconds: 27 healthy navigations and a second-round
Microsoft document that stayed unready through the 60-second verification
budget. Reset, rebuild, timeout, and load-stop counters were all zero.
Thus the demonstrated watchdog fix does not by itself fix the live stall.

That document completed DOMContentLoaded naturally at 610,957 ms and its load
at 611,937 ms. The diagnostic CSS fetch and syscall tracing happened later;
the attempted CSS-link replacement modified zero links because the page had
already recovered. Those interventions cannot explain its recovery. The
later trace's absence of IPC errors also cannot exclude an earlier IPC stall.
Read-only GDB inspections examined the UI stack and the typed `Browser`
variable; no inferior functions were called. The failed idle instance was
subsequently retired, preserving the original failed verdict.

## Network-library comparison

The next experiment compares the installed libsoup `3.4.4-5ubuntu0.9` with
an independently built 3.8.1, motivated by upstream HTTP/2 response-stream
and stall fixes. Both 30-minute diagnostic rotations passed the same full
seventeen-site workload: 233 healthy navigations with 3.4.4 and 232 with
3.8.1, zero resets, rebuilds, and automation timeouts in each. Their IPC-return
tracing makes them diagnostic evidence, and their short duration does not
establish that the upgrade fixes the Microsoft fault.

The GSSAPI-enabled 3.8.1 candidate, build instructions, source checksum, and
verification scope are recorded in
[the network-runtime notes](../runtime/libsoup-3.8.1/README.md). That combined
runtime passed 25 verifier checks, three repeated downloads, all 42 integration
checks, 119 selected upstream network subtests, four media combinations,
the strict keepalive lifecycle regression, and real kernel descriptor-pressure
recovery. Fifty synthetic recovery-failure/hard-reset cycles also passed.

The verification work preserved several unsuccessful test attempts. A download
had passed its data checks but raced temporary HOME/cache cleanup; the fixture
now selects its download directory through XDG configuration. A 320 MiB
allocation did not always push process-tree PSS above its 512 MiB maintenance
threshold, so both trigger fixtures now allocate 640 MiB directly. One
synthetic fault-injection attempt reported connection refused without saving
the main exit code; the exact cause remains unclassified and was not reproduced
by subsequent targeted repeats and the fifty-cycle stress run. The harness
now records unexpected early main exits and the injected process PID.
An initial media helper invocation also exceeded the Unix socket path limit;
all four checks passed with a shorter diagnostic output path. These corrections
do not turn any failed endurance candidate into a passing run.

The 24-hour goal remains unmet. A fresh uninstrumented full-site run is required
for the combined candidate; no system runtime or official desktop executable
has been replaced.

Candidate `stability-source-24h/20261026-205559` started a fresh uninstrumented
24-hour rotation at 20:55 EDT on 2026-10-26, PID 2889252, display `:1`, Normal
mode. It combines application `1439948c…`, source WebKit engine `50cbf91f…`,
and the GSSAPI-enabled libsoup build `10a83d9b…`. All seventeen sites remain
configured, with the 60-second control budget and 12-second resource
observation. Sandbox, image, media, and graphics capabilities remain enabled.
The independent audit additionally requires unchanged libsoup and WebKit
hashes, the original control-socket inode, and zero maintenance recycles.
It failed after 11,392.989 seconds, including the final failure-detection wait:
1,965 healthy documents out of 1,966 attempts, all seventeen sites, and 115
complete rounds. In round 116, Microsoft again remained `loading` with an
empty body throughout the 60-second verification budget. Reset, rebuild,
automation-timeout, and maintenance-recycle counters stayed zero. This run
therefore also demonstrates that the libsoup upgrade does not eliminate the
head-only stall. Its 2.787 hours of post-warmup resource evidence remain below
the required four hours; no absence-of-leaks conclusion follows.

## Resumed testing on 2026-10-28

The failed candidate's original CSV, audit, and manifest were preserved with
checksums before retiring its idle instance. A fresh run,
`stability-source-24h/20261028-143737`, started at 14:37 EDT on display `:1`,
Normal mode, with the same qualified application, source engine, network
library, seventeen sites, and strict gates. All seventeen sites completed
their first rotation. This is ongoing testing, not a 24-hour pass or a new
application fix.

Test supervision now runs in the independent system service
`lightview-endurance-supervised-20261028.service`. Its local session directory
is `~/.local/state/lightview/endurance-supervised/20261028-resumed/`.
`status.json` records current progress, `events.jsonl` records lifecycle
events, and `history.json` retains completed attempts. On failure, it preserves
the verdict, captures document readiness, stylesheet availability, resource
timing, and process memory, retires that test instance, and starts a fresh
independently timed attempt after a delay. A failure remains failed, and
durations are never combined. This mechanism continues test execution;
it does not autonomously change application code or establish a root cause.

The observer also collects process/thread evidence for committed loads that
remain pending, rather than monitoring only provisional navigation. Neither
the live driver nor its readiness/time limits were relaxed. No debugger or
interposed library is attached. The supervisor validates each new namespace
holder PID instead of retaining a stale PID across an environment restart.
The verifier's 25 unit checks passed again. Supervisor checks rejected short,
failed, interrupted, and incomplete verdicts; an isolated lifecycle trial
advanced through two distinct simulated failures while preserving both.
That trial did not touch a real browser or count as endurance evidence.

The first resumed attempt reproduced the fault in round two after 225.045
seconds: 27 healthy documents out of 28 attempts, without a reset, rebuild,
or automation timeout. Its post-verdict capture confirms that the main HTML
finished receiving at 331 ms while DOMContentLoaded remained zero and the
body remained absent. The fourth stylesheet, `clientlib-uhf…min.css`, had no
sheet or completed resource entry. CPU samples mainly show software-rendering
threads, rather than proving a JavaScript or network deadlock. Separate curl
requests made after retiring the failed instance received that stylesheet
with HTTP 200 over both HTTP/2 and HTTP/1.1 in less than 0.2 seconds; these
requests cannot establish what happened on the browser's original connection.

Supervision preserved and retired this real failed attempt, then scheduled
another independently timed attempt. For the next comparison, the launcher
starts an empty dedicated persistent test profile instead of copying the
retired profile, to separate inherited cache/cookie state from the runtime.
The previous launch script and the configuration change are retained in the
supervision session. Application, libraries, site coverage, sandbox, timeouts,
and pass gates remain unchanged. A fresh profile is an experimental condition,
not a demonstrated fix.

## Endurance results reviewed on 2026-11-03

The initial review recovered 96 completed, independently audited attempts.
One additional attempt completed during monitoring repair, bringing the
reviewed total to 97. None passed the 24-hour gate. They contain 65,201
navigation attempts, with 65,104 healthy document validations and one terminal
failure in each attempt. The high individual-page success count does not
establish continuous stability. The application and candidate runtime were
not automatically modified by the test supervisor.

Primary terminal failures were 83 unverified documents, seven load errors,
and seven WebKit resets. Separately, 83 attempts ended while visiting Microsoft;
the site count and primary-failure count describe different classifications.
Fresh profiles therefore did not eliminate the Microsoft problem. Other
terminal sites were Reddit, Apple, Wikipedia, Google, Bing, Amazon, and
Facebook.

The longest attempt, `20261101-063244`, lasted 61,970.843 seconds (about
17 hours 12 minutes 51 seconds), with 8,404 healthy documents before a terminal
Reddit failure. Its browser log records a GStreamer libav assertion in
`gst_ffmpegviddec_video_frame`: video metadata width differed from the decoded
video-info width. WebKit reported a crashed web process and recovered it,
which correctly failed the run. The next-longest attempt, `20261030-043409`,
lasted 57,532.857 seconds (about 15 hours 58 minutes 53 seconds) before another
unverified Microsoft document. Neither attempt is a healthy-duration pass.

Six of the seven WebKit resets followed a process crash; one followed an
automation timeout. Four crash logs contain the same libav width assertion.
The two Apple crash logs do not establish that assertion as their cause.
No OOM cause is established by these findings.

Ten attempts supplied at least four post-warmup hours of comparable resource
data. Five triggered sustained thread growth, and two of those also triggered
file-descriptor growth. No evaluable attempt triggered the configured PSS
growth criteria. This does not establish absence of memory leaks, and the
observed thread/descriptor accumulation already violates the resource gate.

The accumulation is concentrated in the UI process. In the longest attempt,
comparable non-Microsoft early/late page samples had median UI thread counts
of 27 versus 103 and descriptor counts of 44.5 versus 120. Network threads
remained 19 versus 19, and web-process threads 43 versus 42. The last captured
UI thread census included 93 `ReceiveQueue` threads. The second-longest
attempt had 96 such threads and similar UI growth. Unreleased IPC connections
or queue lifetimes are a lead to investigate, not an established root cause.

The supervisor itself also failed. Immediately after launching
`20261102-202832`, it raced the driver's newly created CSV before the header
was available, raising `TypeError` inside `csv.DictReader`. It then retained
the previous attempt's status, and its recovery code repeatedly tried to stop
already-unloaded transient units. That caused 838 supervisor restarts; these
are not 838 browser resets. The independently launched driver still completed
its audit after 17,897.366 seconds, with 2,336 healthy documents before an
unverified Wikipedia document, but its terminal result was absent from the
supervisor ledger until this review.

The monitoring repair captures and validates the CSV header before iteration,
defers incomplete records, persists a new attempt's identity before reading
its progress, tolerates only genuinely absent units during cleanup, and
reconciles the latest independently audited attempt exactly once on restart.
Real cleanup failures still surface. Eleven focused regression checks and the
existing nine supervisor verdict checks passed. The CSV race also reproduces
the original production exception against the saved pre-repair supervisor.
A stalled synthetic reproduction helper was stopped and corrected before
repeating that check; it never altered a browser or endurance verdict.

Before repair, the supervisor scripts, logs, and ledger were backed up.
Checksums of all 288 original manifest, CSV, and audit files remained unchanged
after recovery. The missing 96th result was reconciled as failed. A fresh
attempt, `20261103-104155`, started at 10:41 EST on display `:1` with the same
browser/runtime candidate and strict gates. It failed after 118.239 seconds,
with ten healthy documents before Microsoft remained unready. The corrected
supervisor captured that failure, retired the instance, and scheduled another
independently timed attempt without restarting itself. Supervision is active
again, including its delays between failed attempts. Browser loading,
decoder-crash, and resource-lifetime issues remain unresolved; the 24-hour
objective is still unmet.

## Convergence review on 2026-11-06 EST

At 23:49 EST (2026-11-07 UTC), supervision had 300 ended attempts, with no
24-hour pass. All 300 manifests identify exactly the same application
(`1439948c…`), WebKit (`50cbf91f…`), and libsoup (`10a83d9b…`) builds. The
application source and binary were last modified on October 26; the November 3
changes repaired test supervision, not the outstanding browser failures.
Continued background testing did not implement the user's requested ongoing
browser repair work. There is no new browser fix candidate to compare here.

The terminal classifications are 271 unverified documents, 15 load errors,
12 WebKit resets, and two control failures. In 270 attempts the terminal site
was Microsoft. The twelve resets consist of ten process crashes and two
automation-timeout resets. An unverified document is not itself evidence of
a process crash: the Microsoft resource/parser stalls still need separation
from website or network behavior using an independent baseline.

The first 97 ended attempts had a median recorded duration of 21.75 minutes;
the later 203 had a median of 6.56 minutes. Their maximum durations were
17.214 and 8.841 hours respectively. Website and network variability affect
these timings, and the same browser build was used throughout, so this is
not a causal comparison of software versions. The results do not support a
claim that stability is converging. Twelve attempts had evaluable resource
trends: six flagged thread growth, two also flagged descriptor growth, and
none flagged sustained PSS growth under the configured criteria.

Targeted UI queue diagnostics began during this review without modifying the
ongoing endurance instance. The first local control completed forty
navigations and forty delayed keepalive responses, with two UI ReceiveQueue
threads throughout, but did not change WebProcess PID. It therefore did not
exercise the process turnover seen on real sites and cannot validate a fix.
A COOP same-origin variant triggered real process turnover; its first attempt
stopped because the diagnostic failed to tolerate a process disappearing
during memory sampling. That diagnostic error was preserved, its name lookup
was corrected, and the bounded test was repeated. None of these local controls
constitutes a browser repair or endurance pass.

The corrected COOP control completed forty navigations and all forty delayed
keepalive responses. Its snapshots observed nine distinct WebProcess PIDs,
while UI ReceiveQueue count stayed at two, including after the final quiet
period. Thus simple origin-driven process turnover plus keepalive completion
does not reproduce the observed accumulation in this bounded control. The
real-site trigger and owning object lifetime remain to be isolated; no new
browser patch or fix claim follows from this result.

## Decoder repair iterations on 2026-11-07 EST

The next change modifies the isolated gst-libav runtime, rather than running
the unchanged October 26 candidate again. Allocation fault injection against
both the system and source-built gst-libav 1.24.1 reproduces the same fatal
video-metadata width assertion recorded in the endurance crashes. Ordinary
resolution changes alone did not reproduce that assertion. The fault fixture
therefore establishes a deterministic fatal condition, without establishing
what originally supplied the mismatched allocation on the live websites.

The first repair retained the source frame while allocating and copying into
a fresh negotiated output. It avoided the abort and preserved all sampled
pixels, but a stricter output check found that the destination could still
advertise width 642 while its negotiated picture was width 640. That candidate
was withdrawn. The second repair also normalizes fresh output metadata while
preserving offsets and strides, rejects undersized or incompatible layouts,
and returns a decoding error when mapping fails. The regression requires
matching output metadata and correct sampled pixels in every frame.

The unpatched control aborts with SIGABRT; the second candidate passes both
ordinary and fault-injected decoding of all 288 frames, including 23 resolution
transitions. Six upstream decoder suites, four browser playback/seek/loop
combinations, 25 verifier checks, three download checks, and 42 browser
integration checks also passed. Browser media checks verify that the actual
WebProcess maps the candidate plugin, rather than relying on its environment.
See the [patch and reproducible regression](../runtime/gst-libav-1.24.1/README.md).

A separately timed formal attempt, `20261107-023025`, began at 02:30 EST with
the second plugin (`848d06c0…`). Its startup decoded local H.264 and verified
the plugin mapping. The previous two live attempts were intentionally retired
to change candidates, with explicit retirement markers; their durations are
not combined and those retirements are not newly discovered browser crashes.
System runtime files and the production Lightview executable were unchanged.

The new attempt retains all 17 sites, sandboxing, original control deadlines,
and strict document/resource gates. No 24-hour stability pass has been earned.
UI ReceiveQueue/descriptor accumulation and Microsoft loading stalls remain
separate investigations. A compiler-layout-validated, hash-gated, read-only
connection-metadata capture is prepared for failed attempts, while a separate
bounded real-site reproduction and independent Edge control investigate those
issues without attaching a debugger to the formal endurance instance.

Further negative allocation tests found another error path: with undersized
metadata, the second candidate emitted a frame-mapping diagnostic and then
crashed with SIGSEGV while draining. A debugger on the standalone fixture
identified `no_output` dereferencing a null input frame. The third source
iteration avoids mapping incompatible direct-rendering destinations by using
libav-owned decode storage, and guards the absent input frame in the output
error path. Normal, width-mismatch, and height-mismatch streams each pass all
288 strict frame checks; undersized and incompatible-format cases return an
ordinary decoding error without a process crash. Six upstream suites and all
four browser media combinations pass with the third plugin (`3cce94f7…`).

The second candidate's formal attempt `20261107-023025` failed after 553.278
seconds and 58 healthy documents because a page operation timed out on
Wikipedia, with zero resets. It remains a failed result. The separately started
`20261107-024102` was intentionally retired for the third runtime correction.
The first third-candidate integration run was interrupted by a fatal AT-SPI
client error after its shared accessibility-bus socket disappeared. Its unit
and download checks passed, but it is not a passing full integration result.
Requalification uses private XDG cache and runtime directories as well as a
private session bus and display; accessibility coverage stays enabled.

Read-only connection metadata and backtraces from the separate real-site
diagnostic also prevent a misleading leak attribution. After its first full
rotation there were two live IPC connections and three threads named
ReceiveQueue. Two were polling IPC run loops, while the third was bmalloc's
background scavenger inheriting its creator's thread name. That one thread
does not establish a leaked IPC queue. In contrast, the saved longest and
second-longest endurance observations show all 93 and 96 named threads
waiting in poll. Those larger historical thread/descriptor trends remain
unresolved and are not explained by the single allocator thread.

With private XDG directories, requalification completed: 25 verifier checks,
three download checks, and all 42 browser integration checks passed. A further
50-case threaded decode/error-path stress test passed in 23.598 seconds. The
third candidate entered the independently timed attempt `20261107-025701` at
02:57 EST; startup again verified actual media decoding and plugin mapping.
This is a new candidate under test, not a completed endurance result.

A separate native control also created and destroyed 1,000 WorkQueues using
the same JavaScriptCore/WTF runtime. Descriptor count stayed at six, and named
ReceiveQueue threads returned to zero after each batch of 100. That control
does not reproduce the historical accumulation. Its first helper incorrectly
initialized the main RunLoop twice and aborted before testing; that diagnostic
mistake and a premature executable launch were preserved separately and are
not browser stability failures.

## Full-duration result and remaining root-cause work

The V3 attempt `20261110-003449` ran from November 10 to November 11 UTC for
86,400.268 seconds. All 12,887 navigations across the seventeen-site set
verified readable committed documents. The main PID and control socket stayed
unchanged; full resets, WebView rebuilds, automation timeouts, load errors,
and maintenance recycles were zero. Three navigation retries were recorded.

Its independent audit still returned `passed: false`, solely because the
post-warmup resource gate detected sustained descriptor and thread growth:

| Comparable complete-round metric | Early median | Late median |
| --- | ---: | ---: |
| Total file descriptors | 235 | 334 |
| Total threads | 121 | 219 |
| Process-tree PSS, MiB | 1,275 | 1,346.005 |

PSS did not exceed the configured material-growth threshold. That does not
establish that all resources were released: descriptors and threads failed
their gates. Completing the wall-clock duration is not sufficient for LTV-017
acceptance.

Earlier post-failure inspection of V3 attempt `20261107-071047` found 64 live
IPC Connection objects and 64 named receive threads. Of those connections,
62 were invalid and disconnected, had no client, and still had two strong
references. Their work queues remained allocated. This provides concrete
evidence of retained closed connections, beyond the allocator-thread naming
ambiguity described above. The exact reference owners and missing release
path had not been established at the November 16 review.

Intermittent document-loading failures, particularly on Microsoft, also
remain unresolved. Completed HTML responses with an unready document and
pending stylesheet activity do not by themselves identify whether the cause
is in WebKit, the transport, or the site. The libsoup comparison and fresh
profiles did not eliminate the failure.

As of the November 16 review, all 291 V3 manifests identified the same
application, WebKit, libsoup, and decoder builds introduced on November 7.
No later fix entered that series of runs. Repeated background testing and
evidence capture must not be described as ongoing automatic source repair.
At that review, the decoder abort and drain-error corrections were reproducible
fixes; IPC-retention and loading failures still required targeted investigation,
regression coverage, a new candidate, and another strict endurance audit.

## Closed-connection scheduling retention: November 17

A fresh isolated real-site diagnostic reproduced a retained invalid connection
by the transition from YouTube Music to Facebook. The connection had no client,
no pending output, empty reply-handler maps, and stopped read/write socket
monitors. It remained allocated across the following navigation and twelve idle
observations. The compiler-layout-checked reader found its pointer in the shared
`SyncMessageState::m_didScheduleDispatchMessagesWorkSet` while its own
`m_syncState` was already null. This identifies an owning bookkeeping entry;
the socket-monitor callbacks were not retaining it in this reproduction.

`processIncomingMessage()` adds a strong connection reference to that set and
schedules `dispatchSyncStateMessages()`. If `invalidate()` clears `m_syncState`
first, that deferred call returns without removing the entry. Other live
connections keep the shared state alive, allowing closed connections and their
receive queues to accumulate. This is a concrete missing cleanup path, beyond
the earlier correlation between descriptor and thread counts.

The new experimental
[`sync-state-invalidation-cleanup.patch`](../runtime/webkitgtk-2.52.6/sync-state-invalidation-cleanup.patch)
removes the invalidated connection from both shared scheduling sets and pending
message deques before clearing its state. A native regression forces the ordering
deterministically: the original code retains the connection and bookkeeping,
whereas the candidate releases them and preserves another connection's message
delivery. The regression also exercises invalidation from within message dispatch.
Its build driver preserves independent diagnostic libraries and never modifies
the existing source/build tree or installed engine.

The candidate runtime is tested separately with the V3 decoder and existing
libsoup candidate. Its engine and child executable paths are selected consistently
and verified in `/proc` mappings. The previous background launcher had stopped
because its long-lived namespace helper was gone. The replacement test launcher
creates a fresh private mount namespace, mounts a fresh proc view, and drops to
UID 1000 with no capabilities and `NoNewPrivs`, retaining the WebKit sandbox.

Qualification passed 25 verifier checks, 42 browser integration checks, and 60
native lifetime cases, including reentrant invalidation. Six control cases
confirmed the original retention in both deferred-dispatch scenarios and its
existing behavior during reentrant invalidation. A separate three-round site
diagnostic verified 51 documents across all seventeen sites. All 52 metadata
samples had zero retained invalid connections; final UI connections/receive
threads returned to two, with zero resets and operation timeouts. The site
diagnostic is explicitly ineligible for the formal endurance gate.

The new strict attempt `20261117-014854` started at 06:48:56 UTC on November 17,
with the V3 decoder, sandbox enabled, normal mode, the same full site set and
acceptance gates, and no proactive or network-PSS maintenance recycles. Its
engine SHA-256 is
`7f5e12486f7ad620e097a65d7d08d0ad32c2923dcf8222a1a5f02b7c4a197bf8`.
It failed its acceptance period as described below. A separate capture job waits for a
failed terminal driver verdict or independent audit before collecting document
resource timing and connection metadata. Resource origins and DOM indices
correlate pending styles/scripts without recording URL paths, queries, or
resource bodies. It does not probe a healthy attempt or restart a failed one.

This correction addresses a reproduced resource-retention bug. It does not prove
that every historical failure had the same cause, and Microsoft document-loading
stalls remain unproven. Another independently timed full seventeen-site endurance
run with unchanged strict acceptance gates is still required.

### First cleanup-candidate endurance result

Attempt `20261117-014854` ended after 24,311.532 seconds (6 h 45 min), with
3,903 verified documents out of 3,904 navigations across all seventeen sites.
The last Microsoft document remained `loading` after the existing document
verification budget. The application survived with the same main PID/socket,
zero resets, operation timeouts, navigation retries, view rebuilds, and
maintenance recycles. The independent audit rejected the unverified document,
incomplete duration, and sustained PSS growth. This is a failed attempt.

The post-warmup PSS median increased from about 1,128 to 1,274 MiB, with a
robust slope near 24 MiB/hour. Descriptor/thread medians increased only from
195/85 to 200/87, below their material-growth thresholds. The main process's
complete-round medians remained 21 threads and 39 descriptors. A post-failure
compiler-layout-checked read found two valid connections/receive queues and no
retained invalid connection. These observations support the targeted lifetime
correction; they do not establish absence of other memory leaks.

Stored-data analysis found a change from Amazon's small intermediate document
to its full homepage near hour five, coinciding with a step in memory use.
Later half-hour medians decreased, but the original acceptance gate continued
to report growth. This correlation neither proves a leak nor excuses the
failed resource gate.

The failed Microsoft page subsequently reached `complete`, with
`domInteractive` around 608 seconds and no reset. During the stalled period,
a small same-origin fetch completed while repeated requests for a larger
script were aborted by an eight-second diagnostic timer. An independent HTTP/2
client fetched that script successfully. Different connections and request
contexts prevent attributing this to a specific HTTP/2 or libsoup defect.
The document's Resource Timing buffer had reached its 250-entry capacity:
missing completed entries therefore cannot identify the blocking script.
Optional `LIGHTVIEW_TRACE_RESOURCES` logging now records WebKit's actual
response/failure/completion events beyond that buffer, without URL paths,
queries, credentials, or response bodies. This improves diagnosis; it is not
a document-stall repair or a passing endurance result.

The original automatic capture ran its public control requests as root, which
the browser correctly rejected because its control socket requires the
browser owner's UID. The original capture is preserved and supplemented by
owner-UID observations. Its successor drops to the socket owner's credentials
for public control calls while keeping the separately gated metadata reader
privileged; the actual helper was verified for status and document requests.

### Resource-traced reproduction

The independent diagnostic `20261117-085350` is explicitly ineligible for
endurance acceptance. Its driver reproduced the Microsoft failure after
423.735 seconds: 61 verified documents out of 62, with no reset or operation
timeout. The corrected owner-UID capture successfully recorded the document
and connection metadata. The document was still in its head with no body;
two valid IPC connections and zero retained invalid connections remained.

Unlike the bounded Resource Timing buffer, the new resource signals recorded
actual HTTP 403 responses from Microsoft subresources. Several responses took
about 120 seconds. A separate browser fetch returned a 148-byte HTML error
page with status 403. Later observations found pending shared-head stylesheet
and script requests in libsoup's HTTP/2 queue, with request writing complete,
no response headers yet, and the read source attached. The document eventually
became complete around 616 seconds after navigation without a reset. Its
navigation sequence then had 593 observed resources and no resource lacking
an observed terminal event. This is stronger evidence of delayed/rejected
requests than the earlier inference from missing performance entries.

Fresh independent IPv4 and IPv6 connections fetched the actual pending assets
successfully. Fresh connections with the browser's User-Agent, page-readable
cookies, and both together also succeeded. A fresh asynchronous libsoup 3.8.1
session using that request context fetched the script over HTTP/2 normally.
Cookie values were held only in private temporary request configuration and
were not written into results or logs. These comparisons do not reproduce
all browser headers, connection reuse, server timing, or HTTP-only cookies;
they do not establish a single cause. In particular, neither disabling IPv6
nor replacing the User-Agent is justified by these observations.

A bounded diagnostic experiment now tests an existing-connection hypothesis
after a failed verdict, using the same browser and control socket. It may close
one uniquely identified remote socket owned by that test's network process
and compare the same resource before and after. It performs no broad socket
cleanup, browser reset, or intervention during a healthy attempt. Such a
diagnostic intervention cannot count as a browser fix or passing endurance run.

The first controlled comparison reproduced another Microsoft document failure
after 515.079 seconds, with 78 verified documents out of 79. A same-browser
bodyless fetch using no cookies then received no response within its 20-second
diagnostic bound. Native state and the socket owner identified a unique remote
HTTP/2 connection. Closing only that exact socket restored the same resource:
HTTP 200 with 27,126 bytes in 0.145 seconds, with the same Lightview PID and
control-socket inode. This supports local connection recovery; it does not
identify why that remote connection behaved differently, and the completed
failed verdict remains unchanged.

An isolated libsoup source candidate now retires an unresponsive asynchronous
HTTP/2 connection from future reuse after a bodyless GET/HEAD has waited for
response headers. It uses the existing message restart path once, then surfaces
a timeout if the retry also stalls. The first native regression warms a local
HTTP/2 connection and delays its next response: the unmodified control retained
that connection and failed the new-connection requirement after about 28
seconds; the candidate obtained the complete payload on a fresh connection
after about 20 seconds. This is only the first positive recovery case.
Subsequent negative coverage exposed a delayed-cancellation path: a request
with no incoming response could keep waiting after its cancellation signal.
The second candidate adds a cancellable source on the owning context, removes
it when the header wait finishes, and avoids retiring/retrying a cancelled
request. It also clears sources during message teardown and owner changes.

Eleven independent local TLS/HTTP2 scenarios now pass: GET and HEAD recovery,
an exhausted one-retry bound, POST with and without a body, GET with a body,
headers followed by a delayed body, explicit 403, GET/POST cancellation, and
preservation of a concurrent POST on the retired connection. A frame-controlled
server replaced an unsuitable SoupServer delayed-body fixture; those failed
fixture results remain archived and are not counted as passing checks.
The unmodified library failed the independent cancellation deadline, whereas
the corrected candidate completed cancellation promptly. Ninety consecutive
cancellations left the same fourteen descriptors and six threads after each
batch, and a subsequent healthy request reused the original connection.
This is lifecycle evidence, not proof against every possible memory leak.

Five related upstream suites passed 77 subtests, including all 43 original
HTTP/2 cases. Qualification additionally passed 25 verifier checks and 45
Lightview integration checks with the candidate mapped in all three process
roles. A sandboxed WebKit page on the local HTTP/2 fault server retrieved its
parser-blocking script on a healthy connection after about 20 seconds,
completed parsing, and executed the script without a process termination.

The new library remains in an independent diagnostic prefix; the distribution
and baseline libraries were preserved. Its SHA-256 is
`fba8d47b14542192b3ec18cd6dd150214912667f915043110bfbde18bb9b11ae`.
The [source patch and runtime notes](../runtime/libsoup-3.8.1/README.md) record
the recovery policy. It does not retry an already received HTTP 403 or replay
a POST, and it does not disable HTTP/2, IPv6, media, or certificate checking.

Fresh formal attempt `20261117-102801` started on November 17 at approximately
15:28 UTC on display `:1`. It pins this library, the IPC-cleanup WebKit engine,
and the V3 decoder, with the unchanged seventeen-site set, 60-second control
budget, memory settings, zero-maintenance requirement, and resource-growth
gates. The main PID is 620446. Independent audit and owner-UID failure capture
are armed. It has not completed the required 24-hour period.
