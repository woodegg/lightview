# Candidate correction for the video metadata abort

This development runtime patch addresses the `gst_ffmpegviddec_video_frame`
width/height assertion recorded in several Lightview endurance crashes. It
does not establish the originating website condition, resolve the separate
UI queue accumulation or document-loading faults, or constitute a release.

The decoder can retain an output buffer whose video metadata differs from
the newly negotiated picture. The previous assertion aborted the WebProcess.
The patch retains that buffer while using the decoder's existing allocation
and copy path to produce the current frame, then releases the old buffer.
It also normalizes the fresh destination's video metadata to the negotiated
visible picture while preserving offsets and strides, and rejects layouts
that are too small or have an incompatible pixel format. Mapping failure
returns a decode error instead of accidentally reporting success.
Invalid direct-rendering destinations first fall back to libav-owned storage,
so their descriptors cannot reach the frame mapper. The output-copy error path
also handles draining, where there is no current input frame, without a null
dereference.
It checks the mismatch in all builds. Assertions elsewhere, codecs, video
playback, and seeking remain enabled. System runtime files are not replaced.

The upstream 1.24.13 implementation still terminates on this condition; its
more detailed diagnostic is not itself a recovery fix. The patch is based on
the version installed on the test host, gst-libav `1.24.1-1build1`.

The official source is [gst-libav-1.24.1.tar.xz](https://gstreamer.freedesktop.org/src/gst-libav/gst-libav-1.24.1.tar.xz),
verified against the publisher's [checksum](https://gstreamer.freedesktop.org/src/gst-libav/gst-libav-1.24.1.tar.xz.sha256sum):

```text
549b7821fb5a4c866d6197888496015106cc6ec96ffb12a64e6efa91aa6562fd
```

Apply `reallocate-mismatched-video-meta.patch` with `patch -p1` in that source
directory. Build with Meson/Ninja, matching FFmpeg development headers, and
GStreamer development files. The isolated test-host build used:

```sh
meson setup build gst-libav-1.24.1 \
  --prefix=/usr/local/lib/lightview-diagnostics/libav-width-guard-v3-1.24.1 \
  --libdir=lib --buildtype=debugoptimized --wrap-mode=nofallback \
  -Ddoc=disabled -Dtests=enabled
meson compile -C build -j 2
meson test -C build --print-errorlogs
sudo meson install -C build
```

This isolated prefix is a diagnostic convention, not a production installer.
For an unprivileged test, set `GST_PLUGIN_PATH` to its `lib/gstreamer-1.0`
directory and use a separate `GST_REGISTRY` file. Retain normal system-plugin
discovery for other formats. Verify the selected decoder filename and its
mapping in the WebProcess; an environment setting alone is insufficient.
The compiled candidate plugin has SHA-256
`3cce94f75f71552a976a68cce8c29e6eff233ff1d9ffb2d235e3f03e6ff8392d`.

The plugin and patch retain gst-libav's LGPL-2.0-or-later license. No upstream
source archive or compiled plugin is committed in this repository.

## Regression

Run the standalone test against a selected plugin directory:

```sh
python3 tests/libav_frame_pool.py --plugin-dir /path/to/gstreamer-1.0 \
  --output /tmp/lightview-libav-check
```

Use `--expect-abort` only for the unpatched control. Each output directory is
exclusive, preserving earlier evidence. The test needs FFmpeg with libx264,
a C compiler, pkg-config, and GStreamer app/video development files.

The first candidate avoided the abort and passed sampled-pixel checks, but
a stricter regression found its copied output could still retain the old
metadata width. The second normalized that metadata, but an undersized-pool
test exposed a separate null-input-frame crash while draining after a failed
output. Both earlier candidates are superseded by this third iteration.

The fixture alternates cropped 318x242 red frames and 640x384 blue frames,
including B-frame reordering and 23 resolution transitions. It checks all 288
frames, matching output metadata and negotiated dimensions, and sampled Y/U/V pixels at corners and centers
of all planes. A separate allocation fault injects a metadata mismatch without
changing the actual decoded picture. The original system and source-built
plugins abort at the same width assertion observed in the browser; the
candidate decodes all 288 frames with both ordinary and faulted allocation.
The candidate also decodes 288 frames with a height-mismatch fault and rejects
undersized or incompatible-format metadata with a normal decoding error. The
second candidate crashed with SIGSEGV on the undersized control; the third
passes that same fixture without a mapping assertion or process crash.
The original abort control defers post-sample metadata equality checks to
allow the decoder's later fatal invariant to fire; the fixed candidate always
uses the strict output-metadata checks. This is a controlled reproduction of the fatal condition, not proof of the
website's initiating cause. Browser media regressions and a new strict
endurance run remain required before any stability claim.
