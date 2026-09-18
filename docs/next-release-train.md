# Next Release Train

This document records accepted requirements for the next Lightview release.
Items remain requirements until implementation, validation, and release work
are completed.

## LTV-008 — Version information button

Status: accepted requirement; implementation pending.

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
