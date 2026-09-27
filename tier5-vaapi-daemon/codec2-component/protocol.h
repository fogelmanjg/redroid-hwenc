/*
 * Wire protocol between the Codec2 components running inside Android
 * (bionic) and this VA-API daemon running on the host (glibc).
 *
 * Why this exists at all: porting Mesa's radeonsi Gallium driver (+ the
 * LLVM shader compiler it needs) to build against bionic turned out to be
 * a vastly bigger undertaking than expected -- this AOSP build's Mesa only
 * compiles the gfxstream/ANGLE/Vulkan pieces for Android; there is no
 * Android-side radeonsi at all, real GLES rendering is forwarded to the
 * host's own Mesa the same way. This protocol does the equivalent for
 * VA-API encode: the Android side sends a dma-buf fd + frame parameters,
 * the host side (running the exact VA-API pipeline already proven in
 * tier2-vaapi-encode/tier3-dmabuf-import) does the real encode and sends
 * back the coded bytes.
 *
 * Transport: SOCK_STREAM AF_UNIX. One connection = one encode request/
 * response for now (matches Tier 2/3's "one frame, prove it end to end"
 * scope; a real streaming protocol is Tier 5.6's concern once this
 * baseline round-trip is confirmed).
 *
 * Request: sendmsg() with EncodeRequest as the regular data and the
 * dma-buf fd as SCM_RIGHTS ancillary data, in a single call (so the fd and
 * the metadata describing it can never arrive mismatched).
 *
 * Response: EncodeResponse header, then (if status == 0) exactly
 * `coded_size` bytes of Annex-B H.264.
 *
 * Tier 7 added a second command, decode, on the same socket/connection
 * shape (one connection = one request/response) but a simpler wire shape
 * of its own: every byte flows as plain stream data, no SCM_RIGHTS at all
 * in either direction. The daemon's own persistent VA-API decode surface
 * is driver-allocated (there's no client dma-buf to import on the way
 * in), and - unlike encode's real hardware-tiled output, which stays on
 * the GPU as a coded bitstream - a decoded NV12 frame is small enough,
 * and this project's existing convention already asks for it (see
 * VaapiEncComponent's own encodeViaDaemon()/process(): the daemon returns
 * plain bytes, the component copies them into its own output block), that
 * doing the same for decode's output keeps this new code path consistent
 * with the encode path already proven end to end, rather than adding a
 * new, less-tested zero-copy-into-a-client-buffer scheme up front.
 *
 * Every new connection sends a 4-byte VaapiCommand tag FIRST (a plain
 * write()/read(), before any encode-specific sendmsg()/recvmsg()), so the
 * daemon can dispatch before touching either request struct - existing
 * encode clients (this repo's own test-client.c and
 * VaapiEncComponent.cpp) were updated to send VAAPI_CMD_ENCODE first;
 * anything predating this tag would desync the protocol entirely, so
 * there's no backward-compatible "old" framing to preserve.
 *
 * Decode request: VaapiCommand tag, then a DecodeRequest header, then
 * exactly `bitstream_size` bytes of Annex-B H.264 - the real NAL unit as
 * it appears in the stream (start code excluded, but - a real bug
 * tier6-vaapi-decode's own README documents in detail - the NAL's own
 * 1-byte header INCLUDED, not just the RBSP payload after it).
 *
 * Decode response: DecodeResponse header, then (if status == 0) exactly
 * `frame_size` bytes of tightly-packed NV12 (no plane padding).
 */

#ifndef TIER5_VAAPI_DAEMON_PROTOCOL_H
#define TIER5_VAAPI_DAEMON_PROTOCOL_H

#include <stdint.h>

#define VAAPI_DAEMON_SOCKET_PATH "/dev/vaapi-helper/socket"

typedef enum {
    VAAPI_CMD_ENCODE = 1,
    VAAPI_CMD_DECODE = 2,
} VaapiCommand;

/* NV12 only for now -- matches what a real Codec2 encoder input buffer
 * (GRALLOC_USAGE_HW_VIDEO_ENCODER) is expected to be, unlike the RGBA
 * SurfaceFlinger composited buffer the Tier 3 real-gralloc spike found
 * (that one back was the wrong kind of buffer for this exact reason). */
typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t stride_y;    /* plane 0 (Y) row stride, bytes */
    uint32_t stride_uv;   /* plane 1 (interleaved UV) row stride, bytes */
    uint32_t offset_uv;   /* plane 1 offset within the same dma-buf, bytes */
    uint32_t dmabuf_size; /* total dma-buf size, bytes -- sanity check on the server side */
    /* Tier 5.7 finding: a buffer that comes from a real gralloc allocation
     * (as opposed to the dumb/synthetic buffers earlier tiers used) can be
     * GPU-tiled even with DCC disabled (AMD_DEBUG=nodcc only turns off
     * *compression*, not tiling) -- importing it as if it were a plain
     * linear NV12 raster produces a garbled, striped decode. cros_gralloc's
     * native handle (cros_gralloc_handle.h) carries the real DRM format
     * modifier alongside the buffer; forwarding it here lets the daemon use
     * VA-API's modifier-aware DRM_PRIME_2 import instead of guessing LINEAR. */
    uint64_t drm_format_modifier;
} EncodeRequest;

typedef struct {
    int32_t status;      /* 0 = ok, negative = error (see vaErrorStr equivalents server-side) */
    uint32_t coded_size; /* bytes of Annex-B H.264 following this header, 0 if status != 0 */
} EncodeResponse;

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t bitstream_size; /* bytes of Annex-B H.264 immediately following
                              * this header on the wire - see this file's
                              * own top comment for the exact NAL framing. */
} DecodeRequest;

typedef struct {
    int32_t status;      /* 0 = ok, negative = error */
    uint32_t frame_size; /* bytes of tightly-packed NV12 following this header, 0 if status != 0 */
} DecodeResponse;

#endif
