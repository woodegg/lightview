# Changelog

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
