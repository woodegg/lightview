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

Keyboard shortcuts: Ctrl+L focuses the address, Ctrl+R or F5 reloads, Alt+Left /
Alt+Right navigate history, Escape stops loading, and Ctrl+Q quits. Enter a URL or
an absolute local HTML path. Bare hostnames use HTTPS. There is no search engine
integration. User-initiated links targeting a new window are opened in the
current view; separate popup windows are not implemented.

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
  with `--memory-limit MIB` (128–65536); this is a pressure target rather than a
  hard allocation cap.
- `--low-memory` selects a 384 MiB target and disables automatic images, WebRTC,
  WebGL, and accelerated 2D canvas. Audio, video, Media Source, encrypted media,
  WebAudio, and JavaScript remain enabled so streaming sites can still function.
  Video-conferencing and graphics-heavy sites lose features. An explicit
  `--memory-limit` overrides its 384 MiB default. The
  pressure handler releases critical caches but does not automatically kill an
  oversized page; use `lightviewctl reset` between heavy workflows when needed.

WebKit documents the cache model's effect on memory in its
[cache API reference](https://webkitgtk.org/reference/webkit2gtk/stable/method.WebContext.set_cache_model.html).
The choice of GTK 3 uses the
[WebKitGTK 4.1 API](https://webkitgtk.org/reference/webkit2gtk/stable/index.html),
not the obsolete WebKit1 engine.

WebKit uses multiple processes for rendering and networking; one view does not
mean one OS process. JavaScript heaps, video, GPU buffers, and the content itself
can dominate memory. There is no fixed memory cap or universal low-RAM claim.
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
./tools/lightviewctl eval 'document.title'
./tools/lightviewctl wait 'document.querySelector("#search") !== null'
./tools/lightviewctl fill '#search' 'hello world'
./tools/lightviewctl click 'button[type=submit]'
./tools/lightviewctl eval '({url: location.href, text: document.body.innerText})'
./tools/lightviewctl eval 'fetch("/api/data").then(r => r.json())'
./tools/lightviewctl reset
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
to a 30-second timeout, configurable with `--timeout SECONDS` before the command.
The browser independently expires each connection after about 30 seconds; use
short expressions and client-side polling for long workflows. A timeout cancels
the pending control operation; it is not a reliable way to interrupt an infinite
JavaScript loop in the web process.

`reset` terminates the current WebKit web process and opens `about:blank` in a
fresh process. Use it after a heavy workflow when memory does not fall after
ordinary navigation. It discards page state and history but keeps cookies and
other data in the selected profile. The command acknowledges immediately; use
`lightviewctl wait` afterward when a script must wait for the new blank page.

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
| `status` | none | URI, title, loading state, load error, history state, memory mode/limit, private mode, PID |
| `open` | `uri` string | `null` after navigation is requested |
| `eval` | `script` string | JSON value after expression / promise completes |
| `back`, `forward`, `reload`, `stop`, `reset` | none | `null` |
| `quit` | none | `null`, then exit |

Failures use `{"ok":false,"error":"message"}`. Status exposes load failures
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
cookie/local-storage persistence, private-mode isolation, malformed requests,
concurrent clients, profile locking, and socket lifecycle. It does not require
external sites, credentials, or login tokens. It does require a working display
backend and a host that permits WebKit's process sandbox.

This development container rejects bubblewrap's proc mount, so the normal
sandboxed test invocation cannot launch WebKit here. For **local fixture tests
only**, the suite can be run with a process-scoped WebKit override:

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
or another composited layer appears. Lightview translates that legacy setting
to `WEBKIT_DMABUF_RENDERER_FORCE_SHM=1` at startup for compatibility.

There are currently no tabs, downloads UI, bookmarks, password manager, popup
windows, or permission prompts for camera/microphone/location. Website requests
that need unimplemented permission UI retain WebKit's default behavior. Major
site login, video codecs, DRM, accessibility, and long-session memory behavior
need a separate compatibility pass on the target desktop.
