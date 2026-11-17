# Lightview

A small, single-view WebKit browser for Linux desktops, especially XFCE. The
browser is written in C, uses GTK 3 and WebKitGTK 4.1, and exposes a private local
socket for script control. Python is only needed for the optional command-line
tools and tests; it does not run inside the browser.

This is an initial working foundation, not a complete general-purpose browser.
Modern JavaScript, CSS, fetch, cookies, local storage, and WebKit's normal web
features remain enabled. Compatibility depends on your installed WebKit version,
codecs, and the website. Broad compatibility with major websites has not yet
been established; DRM streaming, browser extensions, popup-based sign-in flows,
and services that require another browser may not work.

## Build and run

On Ubuntu 24.04 or a Debian-family distribution with WebKitGTK >= 2.40:

```sh
sudo apt-get install build-essential pkg-config libwebkit2gtk-4.1-dev libjson-glib-dev
make
./build/lightview https://example.org
```

Run from a graphical desktop session. XFCE's X11 environment is supported by
GTK 3; Wayland is also available through GTK. `./build/lightview --help` lists
options. To install the browser, tools, and desktop launcher:

```sh
sudo make install
```

Installation also adds the Lightview application icon to the standard hicolor
icon theme in sizes from 16 to 512 pixels. The 512-pixel icon is also compiled
into the executable using GLib resources, so the XFCE window and task-switcher
entry keep their icon when the binary is copied or run without the source tree.

The default prefix is `/usr/local`; `PREFIX` and `DESTDIR` are supported. The
launcher does not change your default browser. Builds and installation need the
distribution's development libraries; fetching packages requires network access.

### Nested-container WebKit sandbox launcher

Some unprivileged LXC/Incus containers place private overlays below `/proc`,
such as `/proc/sys/kernel/random/boot_id` or lxcfs resource files. Linux then
rejects the new procfs mount made by WebKitGTK's bubblewrap sandbox even though
ordinary user and PID namespace creation succeeds. Lightview reports the
failure as `bwrap: Can't mount proc on /newroot/proc: Operation not permitted`.

Lightview includes an optional launcher for this specific environment. Build as
the normal user, then explicitly install the browser and launcher under the
required default `/usr/local` prefix as root:

```sh
make
sudo make install-sandbox-launcher
/usr/local/libexec/lightview-sandbox-launcher https://example.org
```

The launcher creates a private mount namespace, prevents mount propagation,
detaches mounts below `/proc` only in that private namespace, and then
irreversibly returns to the calling UID and GID before it executes Lightview.
The fixed Lightview executable must be root-owned, executable, and not writable
by its group or other users. The launcher enables `no_new_privs`; Lightview and
all of its normal automation, profile, memory, and hibernation options follow
the launcher command unchanged.

`make install` installs the launcher as an ordinary `0755` executable, so it
cannot perform the privileged namespace setup. Only the explicit target above
sets owner `root:root` and mode `4755`. Use Lightview directly on hosts where
WebKit's sandbox already starts. Do not make a shell script setuid, grant
Lightview `CAP_SYS_ADMIN`, make the whole container privileged, or disable the
WebKit sandbox as a production workaround.

Keyboard shortcuts: Ctrl+L focuses the address, Ctrl+R or F5 reloads, Alt+Left /
Alt+Right navigate history, Escape stops loading, and Ctrl+Q quits. The toolbar
also provides **Reset WebKit** and version-information buttons. Enter a URL or an
absolute local HTML path. Bare hostnames use HTTPS. There is no search engine
integration. User-initiated links targeting a new window are opened in the
current view; separate popup windows are not implemented.

The toolbar shows a compact running-mode label with live telemetry, for example
`MODE:LM, 312M, CPU 8.4%`. Mode abbreviations are `N` (normal), `P` (private),
`LM` (low memory), and `P/LM` (private low memory). It remains fully visible
under Matchbox configurations that disable native title bars. The native title
keeps the page title first and shows the same compact values. RAM is summed PSS
for Lightview and its WebKit children; CPU is their
aggregate usage and can exceed 100% when several processes use multiple logical
CPUs. Values refresh every two seconds without a helper process or monitoring
thread.

Downloads save automatically to the user's standard Downloads directory. If a
filename already exists, Lightview adds a numeric suffix instead of overwriting
it. Completion or failure appears in the status bar, with no destination prompt.

## Memory choices

- One native window and one WebKit view, with no tab manager, extension host,
  background automation runtime, history database, or favicon database.
- WebKit's `DOCUMENT_VIEWER` cache model and disabled back/forward page cache by
  default. History still works, but revisiting pages can reload them.
- Spell checking is disabled and media autoplay requires a user gesture.
- `--no-images` optionally skips automatic images, with an obvious usability
  tradeoff. `--browser-cache` restores WebKit's browser cache model and page
  cache when faster repeat visits matter more than minimum memory use.
- Web and network processes use WebKit's memory-pressure handler with a 768 MiB
  per-process target. Cleanup starts before the target is reached. Override it
  with `--memory-limit MIB` (128–65536). The default excessive-memory kill
  threshold is the larger of four times that target or 3072 MiB. Override it
  with `--memory-kill-threshold MIB`, or use `--disable-memory-kill` to set
  WebKit's kill threshold to zero. Disabling the kill retains the earlier
  cleanup policies; it removes only WebKit's final memory termination.
- Optional total-browser maintenance is available with
  `--memory-recycle-threshold MIB` (256–65536). Before a new top-level
  navigation, Lightview rebuilds WebKit when current aggregate process-tree PSS
  reaches that explicit threshold. The default is `0` (disabled). WebKit's
  memory-pressure and excessive-memory protection are per process, so deriving
  an aggregate threshold from their per-process kill threshold causes healthy
  multi-process workloads to recycle far too often. When explicitly enabled,
  the requested URL continues after the rebuild; the content and network
  processes are replaced while the Lightview PID, profile, persistent website
  data, and control socket stay unchanged. This loses DOM, history, media, and
  other page-process state and should be used only when an operator intentionally
  prefers a total-PSS guard over uninterrupted page state. Session-only cookies
  in private mode are not guaranteed across an engine recycle.
- Optional NetworkProcess-only maintenance is available at navigation
  boundaries through `--network-recycle-threshold MIB`. Its default is `0`
  (disabled): a 128 MiB threshold proved too low for ordinary major-site use
  and caused frequent process churn. When explicitly enabled, Lightview
  replaces only the network process; the WebView, DOM process, generation,
  reset count, profile, and control socket remain unchanged. Active downloads
  and page operations defer the maintenance.
- WebKit 2.52 can retain IPC receive-queue threads as short-lived site processes
  come and go during repeated cross-site automation. Lightview therefore also
  performs the same network-only maintenance at 512 threads by default. This is
  configurable with `--network-thread-recycle-threshold COUNT`; `0` disables
  it. Unlike a WebKit reset, this does not replace the page process, increment
  the generation/reset count, flash the address bar, or disconnect agents.
- `--low-memory` selects a 384 MiB target and disables WebRTC, WebGL, and
  accelerated 2D canvas. Images, audio, video, Media Source, encrypted media,
  WebAudio, and JavaScript remain enabled so ordinary and streaming sites work.
  Video-conferencing and graphics-heavy sites lose features. An explicit
  `--memory-limit` overrides its 384 MiB default. The default termination
  threshold remains 3072 MiB. This is a last-resort guard rather than a total
  browser memory cap. Disabling it can allow the operating system or container
  to terminate the entire browser when memory is exhausted.

WebKit documents the cache model's effect on memory in its
[cache API reference](https://webkitgtk.org/reference/webkit2gtk/stable/method.WebContext.set_cache_model.html).
The choice of GTK 3 uses the
[WebKitGTK 4.1 API](https://webkitgtk.org/reference/webkit2gtk/stable/index.html),
not the obsolete WebKit1 engine.

The ongoing endurance investigation includes an
[experimental source-built WebKitGTK candidate](runtime/webkitgtk-2.52.6/README.md)
for retained network connections, stalled Linux IPC sends, and media looping.
Those patches are separate from the Lightview executable and are not applied
by `make install`. Their regression results do not yet establish 24-hour
stability; see [the investigation](docs/stability-investigation.md).

An additional [gst-libav correction](runtime/gst-libav-1.24.1/README.md)
reallocates decoded frames when retained buffer metadata disagrees with the
negotiated picture, addressing a fatal decoder invariant seen in long tests.
It is a separate development runtime candidate with an aborting control and
frame-content regression, pending the same endurance qualification.

WebKit uses multiple processes for rendering and networking; one view does not
mean one OS process. JavaScript heaps, video, GPU buffers, and the content itself
can dominate memory. There is no fixed total-browser cap or universal low-RAM claim.
JavaScript, WebGL, media, TLS checks, and process sandboxing are not disabled to
reduce the measured footprint.

Measure the whole process tree, not just the launcher:

```sh
./tools/lightviewctl status
./tools/lightview-memory BROWSER_PID
```

The helper reads Linux `/proc/*/smaps_rollup` and reports RSS and proportional
set size (PSS) in MiB. Summed RSS double-counts shared pages; PSS apportions them.
Unreadable processes make the report incomplete, and GPU allocations are not
fully represented. Compare the same pages, load/idle intervals, display setup,
WebKit version, and cache state when evaluating memory changes.

### Idle hibernation and agent leases

Lightview defaults to hibernating WebKit after **3600 seconds (60 minutes)**.
Use `--idle-hibernate SECONDS` at startup or
`lightviewctl hibernate-after SECONDS` while running to change the interval
(0–86400 seconds); `0` disables automatic hibernation. Changing the interval
starts a fresh idle countdown and does not wake an already suspended engine.
The Version window also has a **Hibernate after idle (seconds)** control and
**Apply idle time** button for the same setting. Its value stays synchronized
when changed through `lightviewctl`. The setting applies to the running
instance; use the startup option to choose a value for future launches.
After the interval without a page operation or UI input, it destroys the
WebKit view and context while keeping the GTK window,
main PID, profile lock, and control socket. It waits while a page is loading,
audio is playing, a download is active, or an automation JavaScript request is
running. `status` reports `engine_state: "suspended"` without waking WebKit.
`open URL` recreates WebKit and loads that URL; `wake` recreates it on
`about:blank`. Other page commands sent while suspended start waking WebKit but
return a retryable error, so the agent must decide which page to reopen.
Hibernation discards DOM, JavaScript variables, form state, and page history;
the last committed URL remains in `status` for an explicit reopen. Background
page JavaScript alone does not count as user activity.

An agent can reserve the instance independently of WebKit's state. `lease
acquire` returns a token valid for 120 seconds by default; `--lease-ttl SECONDS`
changes that lifetime. Pass the token with `--lease TOKEN` or the
`LIGHTVIEW_LEASE` environment variable for navigation, script, reset, and quit
commands. `lease renew` extends it, `lease release` frees it, and expiry releases
it automatically. A reservation does not keep WebKit awake. `status` and
`version` remain readable without a token, and the human-operated window can
still be used. This prevents accidental agent collisions, not access by other
processes running under the same Unix user. A command carrying an expired or
released token is rejected instead of silently running without a reservation.

```sh
./build/lightview --private --low-memory --idle-hibernate 300 \
  --socket "$XDG_RUNTIME_DIR/lightview-agent/control.sock" about:blank
# In another terminal:
./tools/lightviewctl --socket "$XDG_RUNTIME_DIR/lightview-agent/control.sock" lease acquire
LIGHTVIEW_LEASE=TOKEN ./tools/lightviewctl \
  --socket "$XDG_RUNTIME_DIR/lightview-agent/control.sock" open https://example.org --wait
LIGHTVIEW_LEASE=TOKEN ./tools/lightviewctl \
  --socket "$XDG_RUNTIME_DIR/lightview-agent/control.sock" lease renew
LIGHTVIEW_LEASE=TOKEN ./tools/lightviewctl \
  --socket "$XDG_RUNTIME_DIR/lightview-agent/control.sock" hibernate-after 3600
```

The repeatable blank-page measurement is `python3 tests/measure_hibernation.py`
under a working graphical display. In this development container, run it under
`dbus-run-session -- xvfb-run -a` with the test-only WebKit sandbox override
documented below. The WebKit network process may remain after the page process
exits, so the suspended process-tree PSS is not zero.

On this host with WebKitGTK 2.52.6, a blank low-memory private instance under
Xvfb measured **300.72 MiB PSS active**, **129.39 MiB PSS after 20 seconds
suspended**, and **287.59 MiB PSS after waking**. The suspended total comprised
108.14 MiB in the Lightview main process and 21.25 MiB in WebKit's network
process; the web content process had exited. An isolated persistent profile
measured 303.47 MiB active and 131.74 MiB suspended after five seconds. These
are local measurements, not a fixed memory budget or a result for heavy pages.
An isolated persistent-profile instance on XFCE Display `:1` measured
296.64 MiB active and 143.00 MiB suspended; its web content process exited
while the network process remained. The local desktop and Xvfb have different
process-sharing and display costs.

## Profiles

Cookies and website data are stored in `$XDG_DATA_HOME/lightview` (normally
`~/.local/share/lightview`). The directory must be owned by your user and have
mode `0700`. Only one running browser may use a persistent profile at a time.

```sh
./build/lightview --profile "$HOME/.local/share/lightview-work" https://example.org
./build/lightview --private https://example.org
```

`--private` uses an ephemeral WebKit data manager and cannot be combined with
`--profile`. It avoids persistent website data, but is not an anonymity feature
or a promise that the operating system never writes process memory to disk.

The version window shows the active profile and provides **Choose profile
folder…**, a standard local folder chooser that can create a directory. A newly
selected empty folder is made private (`0700`) and locked before Lightview
rebuilds WebKit on `about:blank`. The Lightview PID, window, control socket, and
agent lease remain unchanged. DOM and history are discarded, while the selected
profile's cookies and site storage are loaded. An invalid or already locked
folder leaves the current WebKit session intact. An active download must finish
before switching. Selecting a persistent folder from a private session changes
that session to persistent mode.

If `--profile DIR` points to a profile already locked by another browser at
startup, Lightview opens the same folder chooser with the lock error shown.
Select or create another folder to continue startup. An invalid or locked
selection leaves the chooser open so you can retry; Cancel exits startup.
The chooser in the version window has the same retry behavior, while Cancel
there leaves the running browser unchanged.

## Script control

The browser prints its control socket on startup. The default is
`$XDG_RUNTIME_DIR/lightview/control.sock`, with `$XDG_CACHE_HOME/lightview/control.sock`
(normally `~/.cache/lightview/control.sock`) as the fallback when no runtime
directory is configured. Override it using `--socket PATH` on both tools. Use
different sockets and different profiles (or `--private`) for multiple instances.
`--no-control` disables the socket entirely.

```sh
./tools/lightviewctl open https://example.org --wait
./tools/lightviewctl status
./tools/lightviewctl version
./tools/lightviewctl eval 'document.title'
./tools/lightviewctl wait 'document.querySelector("#search") !== null'
./tools/lightviewctl fill '#search' 'hello world'
./tools/lightviewctl click 'button[type=submit]'
./tools/lightviewctl eval '({url: location.href, text: document.body.innerText})'
./tools/lightviewctl eval 'fetch("/api/data").then(r => r.json())'
./tools/lightviewctl reset
./tools/lightviewctl reset --hard
./tools/lightviewctl mode normal
./tools/lightviewctl mode low-memory
./tools/lightviewctl memory-protection on --threshold 4096
./tools/lightviewctl memory-protection off
./tools/lightviewctl profile "$HOME/.local/share/lightview-work"
./tools/lightviewctl quit
```

Selector examples require matching elements on the current page. `eval` takes a
JavaScript **expression** and awaits promises. For multiple statements use an
IIFE, for example `(() => { const x = 21; return x * 2; })()`. Use `eval -` to read
the expression from stdin. Results are JSON; `undefined` becomes `null`.
Exceptions produce a nonzero exit status and a message on stderr. Return plain
JSON-compatible values rather than DOM objects, cyclic objects, or functions.

`open` acknowledges navigation immediately. `open --wait` waits for document
load, while `wait EXPRESSION` can wait for an application's asynchronous state.
Neither means that all future network activity has finished. The client defaults
to a 65-second timeout, configurable with `--timeout SECONDS` before the command.
The browser independently expires each connection after about 60 seconds; use
short expressions and client-side polling for long workflows. Use
`--control-timeout SECONDS` (1–300) to change this limit. A page-bound timeout
first cancels that operation and returns the retryable `operation_timeout`
protocol error without replacing the page engine. If WebKit cannot acknowledge
the cancellation, Lightview reloads the current document to release its
JavaScript context while retaining the WebKit generation. Only when that reload
also cannot release the operation does Lightview escalate to a WebKit reset, so
a genuinely wedged JavaScript process cannot exhaust the 16-client admission
limit. In that final fallback the command receives retryable `webkit_reset`,
and the agent retries after `engine_state` becomes `ready`.
`status.automation_timeout_count` is monotonic, allowing long-running agents
and health monitors to distinguish a recovered operation timeout from a clean
run even when no WebKit reset was required.

`open URL --wait` observes the browser's monotonic `navigation_sequence` and
returns only after a new document at the requested site and path has committed
and finished. This avoids mistaking an already-complete previous page or a late
commit from the page being replaced for the requested destination. The browser
also supervises the pre-commit
interval. If the old page keeps WebKit's main thread busy and prevents the new
navigation from committing, Lightview first supersedes the provisional load
with a deterministic local document, allows its callbacks to drain, and
retries the destination up to twice in the retained WebView. This avoids
reattaching each retry to the same provisional request. If both retries also
stall, it replaces only the WebView/page process
while retaining the WebKit context, network process, profile, main PID, control
socket, and agent connection, then tries the destination in the fresh page
process. Only a destination that still cannot commit after that second recovery
tier resets the full WebKit engine and replays the requested URL. `status`
exposes `navigation_watchdog_active`, the monotonic
`navigation_retry_count`, and `navigation_view_rebuild_count`.
`web_process_responsive` reports WebKit's current page-process responsiveness
signal. It complements `engine_state`; a responsive process may still have an
uncommitted remote navigation.
`status.committed_uri` is the current document's committed address, or empty
when its engine has been destroyed. It records local recovery documents too.
`last_committed_uri` also retains the last visited address across a reset for
an agent to reopen; use `committed_uri` to verify the current document.
The first commit attempt receives the full `--control-timeout` budget; this
avoids treating a slow but healthy remote response as a wedged WebProcess. Each
retained-process retry receives half of that budget before escalation.
The local blank document used to initialize a new WebView has its own bounded
settle interval; the destination's watchdog begins only when that destination
is actually dispatched.
Waking from hibernation likewise initializes a local document before dispatching
the requested remote destination, so a slow server uses the navigation budget
instead of consuming the engine's shorter startup deadline.

For backend diagnosis, set `LIGHTVIEW_TRACE_NAVIGATION=1` before launch to log
navigation phases, main-resource response status, script-dialog types, and
page-process responsiveness. Traces are off by default and include URL origins
only, omitting credentials, paths, queries, and fragments.
Set `LIGHTVIEW_TRACE_RESOURCES=1` for an optional lifecycle trace of every
main resource and subresource. Each event records a resource ID, its original
navigation sequence, elapsed time, response status, and failure domain/code.
URLs are reduced to origins; error messages and resource bodies are omitted.
The `finished` event retains `failed=1` after a failure, so it does not imply a
successful download. This trace is off by default and can produce large logs.
It uses WebKit resource signals and remains useful after a page's Resource
Timing buffer fills; a missing entry in that buffer does not prove a request
is still pending.

`reset` terminates the current WebKit web process, opens `about:blank` in a fresh
process, and waits until the replacement is ready. `reset --hard` also rebuilds
the WebView and WebKit context. Both forms keep the GTK window, Lightview PID,
profile, and control-socket path. They discard DOM and JavaScript state; profile
cookies and persistent local storage remain available. The last committed URI
is retained in `status` so an agent can decide whether to reopen it.

Unexpected page-process crashes and WebKit memory-limit terminations use the
same recovery path. Page commands receive a retryable `webkit_recovering` or
`webkit_reset` protocol error while the engine is unavailable. Lightview does
not replay state-changing automation commands. A completed recovery clears its
failure streak; only a replacement engine that fails before becoming ready is
treated as a recovery loop. A reported `failed` state returns the non-retryable
`webkit_failed` error until the toolbar action or `reset --hard` starts a new
attempt.

Every manual or automatic reset turns the address bar red for five seconds so
the operator can see that page state was replaced even when Matchbox hides the
native title bar. `status.reset_flash_active` exposes the same indication to an
agent.

`version` returns the running Lightview and WebKitGTK versions. `version --show`
also activates the same local GTK dialog as the toolbar information button and
does not navigate or contact an update service. The dialog includes a **Low
memory mode (LM)** checkbox. Changing it rebuilds WebKit on `about:blank` while
keeping the Lightview window, PID, profile, and automation socket. The same
switch is available to agents through `lightviewctl mode normal` and
`lightviewctl mode low-memory`.

The dialog also includes **WebKit memory kill protection**, an editable kill
threshold in MiB, and an **Apply memory policy** button. Applying a changed
policy rebuilds WebKit on `about:blank` while retaining the main process and
control socket. A disabled policy reports an effective
`memory_kill_threshold_mib` of `0` while retaining the configured value for the
next enable. Agents can use `lightviewctl memory-protection on --threshold MIB`
or `lightviewctl memory-protection off`.

`click` and `fill` use DOM APIs and dispatch synthetic events. They are useful
for scripts but do not generate trusted physical input events. They cannot
bypass user-gesture requirements or cross-origin frame restrictions. This is a
small custom control protocol, not a WebDriver or Chrome DevTools endpoint.

### Wire protocol

Connect using a Unix stream socket, send one UTF-8 JSON object followed by a
newline, and read one newline-terminated response. The browser then closes that
connection. Requests are limited to 1 MiB, responses to 4 MiB, and simultaneous
connections to 16. Idle connections and slow readers do not block the GUI.

```json
{"command":"eval","script":"document.title"}
{"ok":true,"result":"Example Domain"}
```

| Command | Additional field | Result |
| --- | --- | --- |
| `status` | none | Page state, running mode, title/toolbar telemetry, process-tree RAM/CPU, engine state/generation, recovery details, memory policy, version, and PID |
| `version` | optional `show` boolean | Running Lightview and WebKitGTK versions |
| `open` | `uri` string | `null` after navigation is requested |
| `eval` | `script` string | JSON value after expression / promise completes |
| `back`, `forward`, `reload`, `stop` | none | `null` |
| `reset` | optional `hard` boolean | Recovery operation and target generation |
| `mode` | `low_memory` boolean | Switch mode and return the recovery operation |
| `memory-protection` | `enabled` boolean and optional `kill_threshold_mib` integer | Apply the memory kill policy and return the recovery operation |
| `profile` | `path` string | Lock the persistent profile, rebuild WebKit on `about:blank`, and return the recovery operation |
| `wake` | none | Start a replacement WebKit view on `about:blank` and return the recovery operation |
| `hibernate-after` | `seconds` integer (0–86400; 0 disables) | Set the idle interval without restarting the browser |
| `lease` | `action` (`acquire`, `renew`, `release`) and `lease` token for renew/release | Reservation token and lifetime, or release result |
| `quit` | none | `null`, then exit |

Failures use `{"ok":false,"error":"message"}`. Retryable recovery failures also
include stable `error_code` and `retryable` fields. Status exposes load failures
separately because navigation is asynchronous. The socket directory must be
private (`0700`), the socket is created under umask `0077`, and the server checks
that the peer has the same UID. Any process running as your user can control the
browser, including signed-in pages. Do not expose this socket through a network
proxy. Existing socket paths are never overwritten. After a crash, verify that
no browser owns the old socket before removing that stale socket yourself.

## Verification and current limits

```sh
sudo apt-get install python3 xvfb xauth dbus-daemon
make check
```

The integration suite runs the real browser under Xvfb with a local HTTP fixture.
It exercises module scripts, fetch, promises, DOM interaction, navigation,
soft and hard WebKit recovery, version reporting, cookie/local-storage
persistence, private-mode isolation, malformed requests, concurrent clients,
profile locking, and socket lifecycle. It does not require
external sites, credentials, or login tokens. It does require a working display
backend and a host that permits WebKit's process sandbox.

For a time-bounded major-site stability run, start an isolated browser and use
`tools/stability-test.py --socket PATH --duration-hours 24 --output NEW.csv`.
Each run requires a fresh output path; an existing CSV is never overwritten,
including if a service manager unexpectedly restarts the driver. The test exits
nonzero if the engine enters `failed`, the control connection is lost, the main
process changes, any navigation records a load error, any WebKit reset or
WebView rebuild occurs, a page-operation timeout is recorded, document content
cannot be verified, any configured site remains unexercised,
the requested duration is not completed, or a run of at least four hours shows
sustained post-warmup PSS/FD/thread growth. Resource growth is evaluated across
complete repetitions of the same site set: both a robust positive slope and a
material early-to-late increase are required, so a single heavy page or normal
cache warmup is not classified as a leak. Its final JSON includes a strict
`passed` verdict, `failure_reasons`, and the measured resource trend; the CSV
includes timestamps, resource use, resets, proactive recycle counts, and DOM
size signals. A reset, page-operation timeout, load error, or changed main PID
stops the run immediately after its evidence row is flushed. A document with
neither body elements nor visible text is rejected even if it has a title.
Page probes allow the browser's configured control timeout plus a five-second
response margin, so a busy page is judged by the same watchdog used by agents.
Lightview checks a continuing load after half that timeout (at least five
seconds). Its watchdog stops remaining resources only after the document has
a body and `DOMContentLoaded` has completed. A response can commit while
stylesheets or deferred scripts still block parsing; that state must not be
stopped into a permanently empty document. An inconclusive check is deferred,
and its JavaScript operation has a separate five-second cancellation timer.
Stopping resources retains the document, WebKit generation, PID, profile,
and control socket. A new `open` cancels the old document's timer and pending
check before replacement begins. The load watchdog never stops an uncommitted
main resource; pre-commit failures remain owned by the navigation watchdog.
`status` exposes `load_watchdog_active`,
`load_watchdog_seconds`, and `load_stop_count`; the stability CSV records the
stop count before and after each site. The stability runner also has a shorter
per-site resource-observation budget. When that budget expires, it still probes
the committed document through JavaScript. If parsing is still in progress,
it waits within the existing configured control budget from commit. A document
that remains unready at that deadline fails; a continuing resource load can
pass only with readable, populated content. UI-process URL/title metadata alone
cannot count as a successful navigation. The next `open` directly replaces the
active load. A separate `stop_loading()` before the new remote
request can leave the replacement permanently provisional. Ordinary completed
pages also receive the full DOM automation probe. A local blank commit
or stale previous document cannot satisfy `open --wait` or the stability
runner merely because the address bar already shows the requested URL.
If WebKit terminates while a page probe is completing, the control protocol
returns retryable `webkit_reset` after the retained engine restores the page;
the transient callback-order race is not exposed as a script-result failure.

After explicitly installing the nested-container launcher, run the same suite
through that path without disabling WebKit's sandbox:

```sh
LIGHTVIEW_TEST_BROWSER=/usr/local/libexec/lightview-sandbox-launcher make check
```

If the optional nested-container launcher has not been installed, this
development container rejects bubblewrap's proc mount and the normal sandboxed
test invocation cannot launch WebKit. For **local fixture tests only**, the
suite can be run with a process-scoped WebKit override:

```sh
WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1 \
WEBKIT_DMABUF_RENDERER_FORCE_SHM=1 LIBGL_ALWAYS_SOFTWARE=1 make check
```

Do not use that override for browsing untrusted sites. No sandbox override is
set in the program, launcher, or normal test target. A sandboxed launch and
hardware-accelerated rendering still need verification on a normal Linux/XFCE
host. The browser uses the system WebKit package, so keep it updated through
your distribution.

Do not use `WEBKIT_DISABLE_DMABUF_RENDERER=1` with WebKitGTK 2.52. It can leave
the accelerated backing store without a buffer transport and crash when media
or another composited layer appears. Lightview removes that legacy setting and
selects `WEBKIT_DMABUF_RENDERER_FORCE_SHM=1` by default. The shared-memory path
provides buffer transport in containers without usable DRM devices.
When the current user cannot open any `/dev/dri/renderD*` node and
`WEBKIT_GST_DISABLE_GL_SINK` was not explicitly set, Lightview disables only
GStreamer's GL video sink. Media playback falls back to the ordinary sink,
avoiding GL teardown failures without forcing slow software rendering for the
whole page. WebKit keeps its normal on-demand composition policy and hosts with
an accessible render node are unchanged. `status` and `version` expose
`media_gl_sink_disabled` and whether `LIBGL_ALWAYS_SOFTWARE` was explicitly
selected as `software_rendering`.

There are currently no tabs, download manager UI, bookmarks, password manager, popup
windows, or permission prompts for camera/microphone/location. Website requests
that need unimplemented permission UI retain WebKit's default behavior. Major
site login, video codecs, DRM, accessibility, and long-session memory behavior
need a separate compatibility pass on the target desktop.

## License

Lightview is available under the [MIT License](LICENSE).
