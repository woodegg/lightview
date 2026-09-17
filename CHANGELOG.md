# Changelog

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
