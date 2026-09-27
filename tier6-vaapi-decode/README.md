# Tier 7: standalone VA-API H.264 decode (NVIDIA)

Minimal, Android-free C program that drives the *decode* side of VA-API
end to end on real hardware: parses a real H.264 Annex-B elementary
stream itself (hand-written Exp-Golomb bitreader, SPS/PPS/slice-header
parsers), decodes exactly one frame (the IDR) via `VAEntrypointVLD`, and
writes the decoded NV12 frame to a raw file. The decode-side mirror of
[Tier 2](../tier2-vaapi-encode/)'s own encode spike, same scope and rigor:
prove the mechanism on real hardware, not full H.264 spec coverage
(baseline profile, CAVLC, one slice per picture, no B-frames, no
interlacing, no cropping - exactly what the test stream this program
generates for itself uses).

Built against `nvidia-vaapi-driver` ([elFarto](https://github.com/elFarto/nvidia-vaapi-driver)),
tested on a GTX 1050 Ti (Pascal). Unlike this project's own encode work,
NVIDIA decode doesn't need its own vendor-specific transport - it's the
fourth GPU for the same VA-API mechanism `tier5-vaapi-daemon` already
speaks for AMD/Intel encode, not a fork of it.

## Build & run

```sh
sudo apt-get install -y libva-dev libvulkan-dev nvidia-vaapi-driver vainfo
gcc -O2 -Wall -o tier6-decode main.c -lva -lva-drm -ldl
ffmpeg -y -f lavfi -i testsrc=size=640x480:rate=10:duration=2 \
  -c:v libx264 -profile:v baseline -pix_fmt yuv420p /tmp/test_h264.mp4
ffmpeg -y -i /tmp/test_h264.mp4 -c:v copy -bsf:v h264_mp4toannexb -f h264 /tmp/test.h264
LIBVA_DRIVER_NAME=nvidia ./tier6-decode
ffmpeg -y -f rawvideo -pix_fmt nv12 -s 640x480 -i /tmp/decoded_nv12.yuv \
  -update 1 -frames:v 1 decoded.png
```

`INPUT_FILE`/`OUTPUT_FILE`/`DRM_DEVICE` are compile-time constants in
`main.c`, matching Tier 2's own convention.

## The actual findings

Four real, independent bugs, each isolated by direct evidence rather than
assumption - see the repo's own `DEVLOG.md`, 2026-09-27 entry, for the
full trail with the diagnostic steps kept in:

1. **`vaDeriveImage` fails outright.** This driver is built primarily for
   video *playback* (decoded frames go straight into EGL/GL) - a
   CPU-readback path its own primary use case never needs isn't properly
   supported. Use `vaCreateImage` + `vaGetImage` instead.

2. **The decode target surface is genuinely tiled - a real, non-zero DRM
   format modifier, confirmed via `vaExportSurfaceHandle` + a raw
   `mmap`** - and neither creating the surface with an explicit
   `DRM_FORMAT_MOD_LINEAR` request nor a `VAEntrypointVideoProc` blit to a
   second "linear" surface changes this; NVDEC's own hardware decode
   target apparently can't be linear on this GPU at all. Fixed by
   importing the tiled dma-buf into Vulkan with its real, exported
   modifier and `vkCmdCopyImage`-ing into a genuinely linear destination -
   the same technique [redroid-nvidia](https://github.com/fogelmanjg/redroid-nvidia)'s
   own NVENC work used for the mirror-image problem. One new wrinkle:
   `vkAllocateMemory` for the *import* has to use the dma-buf object's own
   real reported size, not a probe image's own computed requirement - they
   don't have to (and here, didn't) agree.

3. **`VASliceParameterBufferH264::slice_data_bit_offset`** - a real
   required field this program's first version left zeroed - needs the
   *true* end-of-header bit position, translated from the de-escaped RBSP
   the header parser reads back into the original, still-escaped
   bitstream VA-API's own field is defined against (not a 1:1 mapping
   whenever an emulation-prevention byte falls before that point).

4. **The actual root cause, found by reading the driver's own source**
   (cloned from GitHub) after `NVD_LOG=1 NVD_LOG_VERBOSE=1` (an
   undocumented env var, found by `strings`-ing the driver binary) traced
   the failure to somewhere *before* the real `cuvidDecodePicture` call:
   this driver reconstructs the bitstream it hands to real NVDEC by
   prepending its own start code directly in front of whatever bytes
   `VASliceDataBufferType` provides, then lets CUVID parse the NAL header
   itself from that. This program's own NAL-extraction helper deliberately
   excludes the 1-byte NAL header everywhere else (already parsed out
   separately) - which meant the slice data buffer was missing that byte,
   so the reconstructed bitstream had a real slice-header bit sitting
   exactly where a NAL header byte should be, and CUVID's own parser
   derived a bogus `nal_unit_type` from it and silently decoded nothing
   real. **Not NVIDIA-specific** - "slice data excludes the start code but
   includes the NAL header byte" is the standard VA-API convention every
   backend expects.

5. **Chroma plane width was wrong by 2x.** NV12 chroma has half as many
   *texels* across as luma, not the same count - each `R8G8`/`GR88` texel
   is one interleaved U,V pair covering a 2x2 luma block. Passing the full
   luma width for the chroma plane's own Vulkan image too computed a
   rowPitch that happened to look plausible (in the same ballpark as this
   GPU's real tiled chroma padding), so the bug produced structured-but-
   wrong output (a washed-out/ghosted look, PSNR ~10dB) rather than an
   obvious failure. Fixed by passing `width/2` for chroma specifically -
   confirmed by the probed layout's own size then matching the real
   dma-buf object's size exactly, for the first time.

6. **`chroma_qp_index_offset`**, parsed from the PPS and immediately
   discarded, never reached `VAPictureParameterBufferH264`'s own field -
   left at zero instead of the real encoded value. Doesn't touch luma at
   all (which stayed bit-exact throughout), but leaves chroma dequantized
   at the wrong effective QP: consistently, not randomly, off (PSNR ~25dB
   after the width fix, before this one). Fixed by keeping the parsed PPS
   value and setting both `chroma_qp_index_offset` and
   `second_chroma_qp_index_offset` from it.

## Result

**Bit-exact.** `ffmpeg`'s own PSNR filter against software decode of the
same frame: `y:inf u:inf v:inf average:inf` - every plane byte-for-byte
identical, not just visually correct. Real NVDEC hardware decode via
VA-API, driven entirely by this program's own from-scratch H.264
bitstream parser. Next: fold this into `tier5-vaapi-daemon`'s existing
multi-vendor architecture as NVIDIA's own supported decode path, then the
Codec2-side decoder component (new work in either project - neither has
one yet, only encoders).
