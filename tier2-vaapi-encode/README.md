# Tier 2: standalone VA-API H.264 encode

Minimal, Android-free C program that drives the *encode* side of VA-API
end to end on real hardware: opens a DRM render node, negotiates an H.264
encode config, encodes one synthetic NV12 frame as an IDR I-frame, and
writes a valid, decodable Annex-B `.h264` file.

## Build & run

```sh
sudo apt-get install -y libva-dev libdrm-dev   # if not already present
gcc -Wall -Wextra -O2 -o vaapi-encode main.c $(pkg-config --cflags --libs libva libva-drm)
./vaapi-encode
ffprobe out.h264   # should report: h264, Constrained Baseline, 320x240
```

Edit `DRM_DEVICE` in `main.c` if your encode-capable GPU isn't at
`/dev/dri/renderD128`.

## The actual finding

Getting a *decodable* file out of this took more than the obvious VA-API
call sequence (config → surfaces → context → seq/pic/slice buffers →
render → sync → read coded buffer). The driver tested against (Mesa
radeonsi, AMD Renoir APU) does **not** synthesize SPS/PPS on its own, and
— more surprisingly — won't emit *any* usable coded output at all unless
the application also submits a packed slice header:

- Without packed headers: the coded buffer is a single segment containing
  a bare, headerless slice — not even a valid NAL (`ffmpeg`/`ffprobe`
  reject it outright).
- With packed SPS + PPS but no packed slice header: identical broken
  output — packed SPS/PPS are silently accepted but have no effect.
- With packed SPS + PPS + slice header all three submitted: the coded
  buffer becomes three clean segments — SPS NAL, PPS NAL, and a correctly
  headered IDR slice NAL — a complete, valid Annex-B stream with no
  further assembly needed.

This means the application has to hand-build the SPS, PPS, and slice
header as raw H.264 bitstreams (start code + NAL header + Exp-Golomb
fields, everything per spec) and submit each as a
`VAEncPackedHeaderParameterBuffer` + `VAEncPackedHeaderDataBuffer` pair —
`main.c`'s `bs_*`/`build_*_rbsp`/`build_slice_header_bits` functions are a
minimal from-scratch bitstream writer for exactly that, checked against
the H.264 spec field by field. See `DEVLOG.md` in the repo root for the
full debugging trail (a fortify-detected crash on the first cut, traced
back to a spec detail easy to miss: the packed header data buffer must
include the start code itself — the driver doesn't add it).

This isn't Android/redroid-specific at all — Tier 3 reuses the exact same
buffer-building logic, just fed by dma-buf-imported Codec2 input surfaces
instead of a synthetic test frame.
