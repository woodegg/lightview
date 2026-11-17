# Experimental libsoup comparison

This is an additional network-runtime candidate for LTV-017, alongside the
[WebKitGTK source patches](../webkitgtk-2.52.6/README.md). It is not part of a
formal release or installed over the distribution library. Building Lightview
alone does not select this runtime.

The head-only Microsoft document in the second source-engine run stayed
unready for the full 60-second verification budget, despite no load stop or
reset. It later completed parsing naturally after about ten minutes. Its
recovery preceded the subsequent diagnostic fetch and syscall trace; those
actions must not be credited with restoring it.

The installed distribution library was `3.4.4-5ubuntu0.9`. The upstream
[3.8.1 release notes](https://raw.githubusercontent.com/GNOME/libsoup/3.8.1/NEWS)
include fixes for HTTP/2 stalls and response-stream error propagation. That
motivates a controlled comparison; it does not establish the live fault's
cause. The two 30-minute diagnostic rotations passed with both versions:
233 valid navigations with the old library and 232 with the new one. Both
used the full seventeen-site set, the same application and WebKit engine,
and IPC-return tracing, with zero resets, rebuilds, or automation timeouts.
These runs are not formal endurance evidence.

## Source and build

The official source is
[`libsoup-3.8.1.tar.xz`](https://download.gnome.org/sources/libsoup/3.8/libsoup-3.8.1.tar.xz),
verified against the publisher's
[SHA-256 list](https://download.gnome.org/sources/libsoup/3.8/libsoup-3.8.1.sha256sum):

```text
5013dcef9136f6f861e6529fca578af50c1f52a39219027fb5aff652d1371a04
```

It retains the upstream LGPL-2.0-or-later license. No libsoup binary or source
archive is included in this repository.

The candidate uses Meson/Ninja, GCC 13, `debugoptimized`, a separate `lib`
directory, and an independent install prefix. Build dependencies include
GLib/GIO >= 2.70, nghttp2, SQLite, libpsl, zlib, Brotli, zstd, and GSSAPI
development headers. GSSAPI is explicitly enabled. The optional external
`ntlm_auth` helper is absent on this host; this does not disable the built-in
explicit-credentials NTLM implementation. Live domain authentication is
outside the public-site test coverage.

Configuration options used:

```text
--buildtype=debugoptimized
--libdir=lib
-Dgssapi=enabled
-Dtests=true
-Ddocs=disabled
-Dintrospection=disabled
-Dvapi=disabled
-Dautobahn=disabled
-Dpkcs11_tests=disabled
```

The controlled test host used extracted GSSAPI development headers and its
existing system runtime, avoiding host runtime upgrades during comparison.
All symbols in the resulting library resolved successfully. Its local
SHA-256 is
`10a83d9b509ee55baf3ecd6115554933dfb808cfd75b0499109d2941b583faa4`;
this identifies the tested build, not a reproducible-build guarantee.

Install only in the independent prefix. Do not replace system libraries or
run a global `ldconfig`. To select it for an unprivileged diagnostic, include
both the WebKit and libsoup candidate `lib` directories in `LD_LIBRARY_PATH`.
Check `/proc/<pid>/maps` in the UI, network, and page processes to confirm
which library was loaded. The existing setuid launcher must not be weakened
to accept arbitrary user-provided libraries.

## Verification

Eight selected upstream suites passed 119 subtests in total: HTTP/2,
HTTP/2 body streams, cache, streaming, timeout, cookies, decompression, and
server authentication. The container lacks IPv6 loopback, so these tests use
upstream's `SOUP_TEST_NO_IPV6=1` option. Apache, PKCS#11, and live Kerberos
infrastructure were not available for additional coverage.

The combined application/engine/network candidate passed 25 verifier tests,
three repeated automatic-download tests, and all 42 real-browser integration
tests. Additional checks passed detached keepalive cleanup, all four
play/seek/loop combinations, and real kernel IPC descriptor pressure without
tracing (2.013 seconds, all 1,040 descriptors released, zero resets/timeouts).
Fifty diagnostic cycles that intentionally killed a replacement WebProcess
and then recovered it through hard reset also passed. A prior isolated
connection refusal during synthetic fault injection lacked a recorded main
exit code; the exact cause remains unclassified. Exit-code logging and the
subsequent stress checks do not retrospectively explain that event.

See [the investigation](../../docs/stability-investigation.md) for the failed
runs and the remaining independent 24-hour qualification gate.

## Bounded response-header recovery candidate

The November 17 extension
[`http2-response-header-recovery.patch`](http2-response-header-recovery.patch)
targets asynchronous bodyless GET/HEAD requests whose HTTP/2 request headers
were sent but have received no response headers for 20 seconds. It excludes
that connection from future reuse, lets existing responses and non-replayable
requests finish, and restarts the eligible request once on a healthy
connection. A second expiry reports a timeout. Received responses, including
HTTP 403, are not retried by this guard. A separate cancellation source wakes
the owner context even if the peer sends nothing.

Apply the patch from the source root before configuring a new, independent
prefix with the existing build options:

```sh
patch --batch --fuzz=0 -p1 < /path/to/http2-response-header-recovery.patch
```

The patch uses zero context lines and was checked to reproduce both qualified
source files byte for byte. Keep the baseline runtime intact.
This changes neither HTTP/2/IPv6/media support nor Lightview's process,
control socket, memory thresholds, or process-recycling policy. Recovery
logging contains connection/stream IDs and the retry bound, not URLs,
headers, cookies, or response bodies.

The extended candidate passed eleven local TLS/HTTP2 fault scenarios,
ninety consecutive cancellations with stable FD/thread counts, five related
upstream suites (77 subtests), 25 verifier checks, and 45 Lightview integration
checks. A sandboxed WebKit page also completed parsing and executed its
critical script after connection recovery. Its tested library SHA-256 is
`fba8d47b14542192b3ec18cd6dd150214912667f915043110bfbde18bb9b11ae`.
These checks do not establish a 24-hour stability pass; the full seventeen-site
endurance and resource-growth audit remains required.
