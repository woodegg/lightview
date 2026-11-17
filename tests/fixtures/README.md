# Media fixture

`media.mp4` is one second of silent black H.264 video generated for the
standalone-media navigation regression. It contains no third-party media.

Generate it with FFmpeg:

```sh
ffmpeg -f lavfi -i color=c=black:s=64x64:r=12 -t 1 \
  -c:v libx264 -profile:v baseline -preset ultrafast -threads 1 \
  -pix_fmt yuv420p -movflags +faststart -an tests/fixtures/media.mp4
```
