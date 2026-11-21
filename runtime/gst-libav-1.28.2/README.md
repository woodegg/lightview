# Ubuntu 26.04 native decoder

The 26.04 package uses Ubuntu source `gst-libav1.0 1.28.2-1`, GStreamer
1.28.2 and native FFmpeg 8. It does not install the Ubuntu 24.04 FFmpeg ABI.
The corresponding `.dsc` and every SHA256-listed archive, including detached
signatures, accompany the platform's runtime-source release asset.

Extract with `dpkg-source -x gst-libav1.0_1.28.2-1.dsc`, retaining Ubuntu's
applied patches. Apply `reallocate-mismatched-video-meta.patch` with
`patch --fuzz=0 -p1` in the extracted source. This ports the 1.24.1 V3 guards:
incompatible direct-rendering buffers use libav-owned storage; mismatched
frames are copied into checked, normalized output buffers; mapping failures
return decode errors; draining errors tolerate a missing input frame.

Build as an ordinary user in the source directory. Ubuntu's existing
`00_plugin-dependencies.patch` requires the `FFMPEG_LIBDIR` C-string define
from `debian/rules`; the value is also recorded in
`../ubuntu26.04/native-decoder.json`. A Python argv list avoids shell-quoting
ambiguity when configuring Meson:

```python
import subprocess
subprocess.run([
    'meson', 'setup', 'build',
    '--prefix=/usr/local/lib/lightview-diagnostics/libav-width-guard-v3-1.28.2-ubuntu26.04',
    '--libdir=lib', '--buildtype=debugoptimized', '--wrap-mode=nofallback',
    '-Ddoc=disabled', '-Dtests=enabled',
    '-Dc_args=[\'-DFFMPEG_LIBDIR="/usr/lib/x86_64-linux-gnu"\']',
], check=True)
```

```sh
meson compile -C build -j 4
meson test -C build --print-errorlogs
sudo meson install -C build
python3 /path/to/lightview/tests/libav_frame_pool.py \
  --plugin-dir /usr/local/lib/lightview-diagnostics/libav-width-guard-v3-1.28.2-ubuntu26.04/lib/gstreamer-1.0 \
  --output /tmp/lightview-native-decoder-check
```

All six upstream suites and five strict fixture cases passed. Normal, width-
and height-mismatch cases each verify 288 frames, dimensions and sampled
pixels; undersized and incompatible-format destinations return clean errors.
The stock 26.04 plugin fails the strict metadata check at frame 13 (642x384
metadata versus a 640x384 picture). This fixture did **not** trigger its fatal
upstream check, so this control is a metadata failure, not a reproduced abort.
The exact installed plugin SHA256 is in the native-decoder manifest.

This functional coverage does not constitute a separate Ubuntu 26.04 24-hour
qualification. Retain the normal WebKit sandbox and system plugin discovery;
verify the actual selected plugin and native FFmpeg mappings during playback.
The patch and plugin retain gst-libav's LGPL-2.0-or-later license.
