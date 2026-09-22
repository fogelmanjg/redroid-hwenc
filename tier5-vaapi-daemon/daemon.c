/*
 * Tier 5.2: VA-API encode daemon.
 *
 * A persistent host-side (glibc) process that does real hardware H.264
 * encode on behalf of the Codec2 component that will eventually run inside
 * Android (bionic) -- see protocol.h for why this exists instead of a
 * native Android-side VA-API stack.
 *
 * The actual encode logic (packed SPS/PPS/slice headers, the VA-API call
 * sequence, the dma-buf import via VASurfaceAttribExternalBuffers) is the
 * same pipeline already proven end to end in tier2-vaapi-encode and
 * tier3-dmabuf-import -- this just wraps it in a socket server instead of
 * a one-shot CLI tool, and imports whatever dma-buf a client hands it
 * instead of allocating/filling one itself.
 */

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>
#include <va/va_enc_h264.h>
#include <va/va_vpp.h>
#include <drm/drm_fourcc.h>

#include <gbm.h>
#define EGL_EGLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include "protocol.h"

#define DRM_RENDER_DEVICE "/dev/dri/renderD128"

#define CHECK_VA(status, msg)                                                \
    do {                                                                     \
        if ((status) != VA_STATUS_SUCCESS) {                                 \
            fprintf(stderr, "%s failed: %s (0x%x)\n", (msg),                 \
                    vaErrorStr(status), (status));                           \
            return -1;                                                      \
        }                                                                    \
    } while (0)

static unsigned int align16(unsigned int v) { return (v + 15) & ~15u; }

/* ------------------------------------------------------------------ *
 * Same bitstream writer as tier2-vaapi-encode/main.c -- see that file
 * for the detailed spec-reference comments on each field. Kept terse
 * here since this daemon's own comments focus on what's new (the socket
 * protocol and per-request dma-buf import), not re-deriving H.264 syntax.
 * ------------------------------------------------------------------ */
typedef struct {
    unsigned char *buf;
    size_t capacity;
    size_t bit_pos;
} bitstream_t;

static void bs_init(bitstream_t *bs) {
    bs->capacity = 256;
    bs->buf = calloc(1, bs->capacity);
    bs->bit_pos = 0;
}

static void bs_grow_if_needed(bitstream_t *bs, size_t extra_bits) {
    size_t needed_bytes = (bs->bit_pos + extra_bits + 7) / 8;
    if (needed_bytes <= bs->capacity) return;
    while (needed_bytes > bs->capacity) bs->capacity *= 2;
    bs->buf = realloc(bs->buf, bs->capacity);
}

static void bs_put_bit(bitstream_t *bs, unsigned int bit) {
    bs_grow_if_needed(bs, 1);
    size_t byte_i = bs->bit_pos / 8;
    int shift = 7 - (bs->bit_pos % 8);
    if (bit) bs->buf[byte_i] |= (1u << shift);
    bs->bit_pos++;
}

static void bs_put_bits(bitstream_t *bs, unsigned int value, int n) {
    for (int i = n - 1; i >= 0; i--) bs_put_bit(bs, (value >> i) & 1);
}

static void bs_put_ue(bitstream_t *bs, unsigned int val) {
    unsigned int code_num = val + 1;
    int bits = 0;
    for (unsigned int tmp = code_num; tmp; tmp >>= 1) bits++;
    bs_put_bits(bs, 0, bits - 1);
    bs_put_bits(bs, code_num, bits);
}

static void bs_put_se(bitstream_t *bs, int val) {
    unsigned int mapped = (val <= 0) ? (unsigned int)(-2 * val)
                                      : (unsigned int)(2 * val - 1);
    bs_put_ue(bs, mapped);
}

static void bs_rbsp_trailing_bits(bitstream_t *bs) {
    bs_put_bit(bs, 1);
    while (bs->bit_pos % 8) bs_put_bit(bs, 0);
}

static void bs_start_code_and_nal_header(bitstream_t *bs, int nal_ref_idc, int nal_unit_type) {
    bs_put_bits(bs, 0x000001, 24);
    bs_put_bit(bs, 0);
    bs_put_bits(bs, nal_ref_idc, 2);
    bs_put_bits(bs, nal_unit_type, 5);
}

static void build_sps_rbsp(bitstream_t *bs, const VAEncSequenceParameterBufferH264 *seq) {
    bs_start_code_and_nal_header(bs, 3, 7);
    bs_put_bits(bs, 66, 8);
    bs_put_bit(bs, 1);
    bs_put_bit(bs, 1);
    bs_put_bit(bs, 0);
    bs_put_bit(bs, 0);
    bs_put_bits(bs, 0, 4);
    bs_put_bits(bs, seq->level_idc, 8);
    bs_put_ue(bs, seq->seq_parameter_set_id);
    bs_put_ue(bs, seq->seq_fields.bits.log2_max_frame_num_minus4);
    bs_put_ue(bs, seq->seq_fields.bits.pic_order_cnt_type);
    bs_put_ue(bs, seq->max_num_ref_frames);
    bs_put_bit(bs, 0);
    bs_put_ue(bs, seq->picture_width_in_mbs - 1);
    bs_put_ue(bs, seq->picture_height_in_mbs - 1);
    bs_put_bit(bs, seq->seq_fields.bits.frame_mbs_only_flag);
    bs_put_bit(bs, 1);
    bs_put_bit(bs, seq->frame_cropping_flag);
    if (seq->frame_cropping_flag) {
        bs_put_ue(bs, seq->frame_crop_left_offset);
        bs_put_ue(bs, seq->frame_crop_right_offset);
        bs_put_ue(bs, seq->frame_crop_top_offset);
        bs_put_ue(bs, seq->frame_crop_bottom_offset);
    }
    bs_put_bit(bs, 0);
    bs_rbsp_trailing_bits(bs);
}

static void build_pps_rbsp(bitstream_t *bs, const VAEncPictureParameterBufferH264 *pic) {
    bs_start_code_and_nal_header(bs, 3, 8);
    bs_put_ue(bs, pic->pic_parameter_set_id);
    bs_put_ue(bs, pic->seq_parameter_set_id);
    bs_put_bit(bs, pic->pic_fields.bits.entropy_coding_mode_flag);
    bs_put_bit(bs, 0);
    bs_put_ue(bs, 0);
    bs_put_ue(bs, pic->num_ref_idx_l0_active_minus1);
    bs_put_ue(bs, 0);
    bs_put_bit(bs, 0);
    bs_put_bits(bs, 0, 2);
    bs_put_se(bs, pic->pic_init_qp - 26);
    bs_put_se(bs, 0);
    bs_put_se(bs, 0);
    bs_put_bit(bs, pic->pic_fields.bits.deblocking_filter_control_present_flag);
    bs_put_bit(bs, 0);
    bs_put_bit(bs, 0);
    bs_rbsp_trailing_bits(bs);
}

static void build_slice_header_bits(bitstream_t *bs,
                                     const VAEncSequenceParameterBufferH264 *seq,
                                     const VAEncPictureParameterBufferH264 *pic,
                                     const VAEncSliceParameterBufferH264 *slice) {
    bs_start_code_and_nal_header(bs, 3, 5);
    bs_put_ue(bs, slice->macroblock_address);
    bs_put_ue(bs, slice->slice_type);
    bs_put_ue(bs, slice->pic_parameter_set_id);
    bs_put_bits(bs, pic->frame_num,
                seq->seq_fields.bits.log2_max_frame_num_minus4 + 4);
    bs_put_ue(bs, slice->idr_pic_id);
    bs_put_bit(bs, 0);
    bs_put_bit(bs, 0);
    bs_put_se(bs, slice->slice_qp_delta);
    if (pic->pic_fields.bits.deblocking_filter_control_present_flag) {
        bs_put_ue(bs, slice->disable_deblocking_filter_idc);
        if (slice->disable_deblocking_filter_idc != 1) {
            bs_put_se(bs, 0);
            bs_put_se(bs, 0);
        }
    }
}

static void submit_packed_header(VADisplay dpy, VAContextID context_id,
                                  VAEncPackedHeaderType type, bitstream_t *bs,
                                  VABufferID *out_param_buf, VABufferID *out_data_buf) {
    VAEncPackedHeaderParameterBuffer param = {0};
    param.type = type;
    param.bit_length = (unsigned int)bs->bit_pos;
    param.has_emulation_bytes = 0;
    vaCreateBuffer(dpy, context_id, VAEncPackedHeaderParameterBufferType,
                   sizeof(param), 1, &param, out_param_buf);
    vaCreateBuffer(dpy, context_id, VAEncPackedHeaderDataBufferType,
                   (bs->bit_pos + 7) / 8, 1, bs->buf, out_data_buf);
}

/* ------------------------------------------------------------------ *
 * Persistent VA-API state, initialized once at daemon startup and reused
 * across requests. Config/context are sized to a specific resolution;
 * encode_one_frame() recreates them if a request's resolution changes.
 * ------------------------------------------------------------------ */
typedef struct {
    int drm_fd;
    VADisplay dpy;
    VAEntrypoint entrypoint;
    VAConfigID config_id;
    VAContextID context_id;
    /* Tier 5.8: a real Surface-sourced frame is RGBA, not NV12 (Tier 5.7's
     * closing finding) -- this driver's own VAEntrypointVideoProc (VPP)
     * pipeline does the RGBA->NV12 conversion entirely on the GPU (queried
     * capable of both surface types simultaneously; confirmed via a
     * standalone probe before writing any of this). [0] = imported RGBA
     * input (recreated per request, since it wraps a different dma-buf
     * every time), [1] = recon, [2] = VPP's NV12 output / the encode's
     * actual source surface (created once per resolution, reused/
     * overwritten every request -- VPP replaces its content, no need to
     * destroy and recreate like the imported surface). */
    VASurfaceID surfaces[3];
    VAConfigID vpp_config_id;
    VAContextID vpp_context_id;
    unsigned int width, height;
    int have_context;
    /* Tier 5.9: the real DRM format modifier a GPU-render-target RGBA
     * buffer ends up with is GPU-generation-specific (confirmed: differs
     * between a Renoir/GFX9 APU and a Polaris/GFX8 discrete card) and can't
     * be read off the buffer itself on this Android build (see DEVLOG) --
     * determined once at startup instead, per host GPU, so a new machine
     * only ever needs this daemon rebuilt (a local `gcc`, seconds) and never
     * the Android-side component (an AOSP rebuild, tied to a single build
     * host). See rgba_modifier_init() for how. */
    uint64_t rgba_modifier;
    /* Tier 5.10 finding, Tier 5.12 fix: some GPUs (confirmed: Polaris/GFX8)
     * report *zero* DRM format modifiers for a GPU-render-target RGBA
     * buffer at all -- there is no modifier value that describes their
     * tiling, because minigbm falls back to Mesa-opaque, non-modifier
     * tiling (TILE_TYPE_DRI) for that combination. DRM_PRIME_2
     * fundamentally cannot import a buffer like that. Tier 5.10 tried
     * bridging via GL (import through EGL's plain, non-modifier extension,
     * then GPU-blit into a fresh explicitly-linear buffer) -- confirmed
     * working only when the same process both allocates and re-imports the
     * buffer, which is a Mesa same-process shortcut, not real cross-process
     * opaque-tiling resolution; failed for the actual (cross-process) case.
     * Tier 5.12 found the real fix instead: VA-API's own *old*, pre-
     * modifier import (VASurfaceAttribExternalBuffers /
     * VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME) resolves this GPU's opaque
     * tiling correctly even across processes -- unlike EGL's equivalent,
     * apparently because libva's import machinery is built around
     * cross-process IPC as the normal case to begin with. use_legacy_prime,
     * set once at startup, means encode_one_frame() takes that import path
     * for this GPU's requests instead of DRM_PRIME_2. */
    int use_legacy_prime;
} vaapi_state_t;

/* Tier 5.9: reproduces minigbm's own tie-break for which tiling a
 * GPU-render-target RGBA buffer gets (amdgpu.c's amdgpu_add_kms_item():
 * one combination per modifier Mesa reports via dri_query_modifiers(), all
 * at equal priority; drv.c's drv_get_combination() keeps whichever combo
 * was registered *first* on a priority tie) -- by asking Mesa for that same
 * ordering directly via EGL and taking its first answer, instead of relying
 * on a value hardcoded for one specific GPU generation.
 *
 * Needs AMD_DEBUG=nodcc set (main() does this, before any Mesa entry point
 * runs -- Mesa parses its debug env vars once per process and caches them,
 * so setenv() here would be too late: confirmed the hard way, calling it at
 * the top of this function still queried under the process's original
 * environment). surfaceflinger gets the same env var exported to it at
 * boot (see redroid-nodcc.rc) so the real buffer's allocation never picks
 * a DCC-compressed tiling at all (VCN can't encode from those, confirmed
 * in Tier 5.7) -- without nodcc here too, this queries 8 modifiers instead
 * of 4 and the first one has DCC=1: a decision the real allocation was
 * never actually given the chance to make.
 *
 * Tier 5.10 finding, Tier 5.12 fix: zero modifiers reported doesn't mean
 * LINEAR here -- it means this GPU can't describe its render-target tiling
 * as a modifier at all (confirmed: Polaris/GFX8). Sets st->use_legacy_prime
 * in that case instead of silently assuming LINEAR (which produced
 * "resource allocation failed" -- a genuinely-linear buffer at the
 * identical stride imports fine, proving the real buffer just isn't
 * linear); encode_one_frame() then imports via the old, pre-modifier
 * VA-API path instead of DRM_PRIME_2 for this GPU (see its own comment).
 *
 * Deliberately self-contained (creates and destroys its own throwaway
 * GBM/EGL, using st->drm_fd) rather than keeping any of it around --
 * confirmed the hard way, during the Tier 5.10 GL-bridge attempt that
 * later got removed, that *any* longer-lived GBM/EGL object in this
 * process, even on a GPU that never touches it again, disturbed VA-API's
 * own unrelated dma-buf import. A query-and-destroy right here, before
 * VA-API has imported anything for real yet, doesn't have that problem.
 *
 * Tier 5.11: "index 0 wins" is an AMD-specific fact, not a general one --
 * confirmed on Intel (TigerLake/gen12, iHD driver): the real buffer came
 * out visually corrupted (tiled-as-linear striping) despite this exact
 * function reporting modifier[0] = DRM_FORMAT_MOD_LINEAR and the import
 * succeeding without error. Read minigbm's *other* backend, i915.c, to
 * understand why: unlike amdgpu.c (one combination per modifier, all at
 * equal priority, so registration order -- which matches this query's
 * order -- decides ties), i915.c registers LINEAR at explicit priority 1
 * and X/Y/4-tiled at explicit priority 2/3 for the *same* render+encoder
 * usage whenever the request has no SW/CPU-read/write usage bits (which
 * ours never does) -- and drv_get_combination() always keeps the highest
 * priority match. Tiled beats linear on Intel, unconditionally, for this
 * usage class. Confirmed by comparing the reported modifier list against
 * i915.c's I915_FORMAT_MOD_Y_TILED constant -- present, and exactly what
 * minigbm's priority rule would pick for this GPU generation (not MTL+,
 * which prefers 4-tiled instead; not handled here since no such GPU has
 * been tested yet). Detected via the VA-API vendor string rather than by
 * driver library name, since that's already queried at daemon startup
 * for logging and reliably distinguishes Intel from AMD/other vendors. */
static uint64_t rgba_modifier_init(vaapi_state_t *st) {
    uint64_t fallback = DRM_FORMAT_MOD_LINEAR;
    struct gbm_device *gbm = gbm_create_device(st->drm_fd);
    if (!gbm) {
        fprintf(stderr, "rgba_modifier_init: gbm_create_device failed, assuming LINEAR\n");
        return fallback;
    }
    EGLDisplay dpy = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, gbm, NULL);
    EGLint major, minor;
    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, &major, &minor)) {
        fprintf(stderr, "rgba_modifier_init: eglInitialize failed, assuming LINEAR\n");
        gbm_device_destroy(gbm);
        return fallback;
    }
    PFNEGLQUERYDMABUFMODIFIERSEXTPROC queryMods =
        (PFNEGLQUERYDMABUFMODIFIERSEXTPROC)eglGetProcAddress("eglQueryDmaBufModifiersEXT");
    EGLint num_mods = 0;
    if (!queryMods || !queryMods(dpy, DRM_FORMAT_ABGR8888, 0, NULL, NULL, &num_mods) ||
        num_mods == 0) {
        fprintf(stderr,
                "rgba_modifier_init: no modifiers reported for this GPU -- opaque tiling, "
                "using the old pre-modifier VA-API import path instead\n");
        st->use_legacy_prime = 1;
        gbm_device_destroy(gbm);
        return fallback;
    }
    EGLuint64KHR *mods = malloc(num_mods * sizeof(EGLuint64KHR));
    EGLBoolean *external = malloc(num_mods * sizeof(EGLBoolean));
    queryMods(dpy, DRM_FORMAT_ABGR8888, num_mods, mods, external, &num_mods);

    uint64_t chosen = mods[0];
    const char *vendor = vaQueryVendorString(st->dpy);
    if (vendor && strstr(vendor, "Intel")) {
        /* I915_FORMAT_MOD_Y_TILED / X_TILED (fourcc_mod_code(INTEL, 2/1)) --
         * not including drm_fourcc.h's i915-specific header here (kernel
         * UAPI header, not always present), just the two known numeric
         * values, per the priority rule above. */
        const uint64_t y_tiled = 0x0100000000000002ULL;
        const uint64_t x_tiled = 0x0100000000000001ULL;
        int found = 0;
        for (int i = 0; i < num_mods && !found; i++) {
            if (mods[i] == y_tiled) {
                chosen = y_tiled;
                found = 1;
            }
        }
        for (int i = 0; i < num_mods && !found; i++) {
            if (mods[i] == x_tiled) {
                chosen = x_tiled;
                found = 1;
            }
        }
        fprintf(stderr, "rgba_modifier_init: Intel GPU, preferring tiled over linear -> %s\n",
                found ? (chosen == y_tiled ? "Y_TILED" : "X_TILED") : "none found, using index 0");
    }

    fprintf(stderr, "rgba_modifier_init: %d modifiers for ABGR8888, using: 0x%016llx\n", num_mods,
            (unsigned long long)chosen);
    free(mods);
    free(external);
    gbm_device_destroy(gbm);
    return chosen;
}

static int vaapi_state_init(vaapi_state_t *st) {
    memset(st, 0, sizeof(*st));
    st->drm_fd = open(DRM_RENDER_DEVICE, O_RDWR);
    if (st->drm_fd < 0) {
        perror("open " DRM_RENDER_DEVICE);
        return -1;
    }
    st->dpy = vaGetDisplayDRM(st->drm_fd);
    if (!st->dpy) {
        fprintf(stderr, "vaGetDisplayDRM failed\n");
        return -1;
    }
    int major, minor;
    CHECK_VA(vaInitialize(st->dpy, &major, &minor), "vaInitialize");
    fprintf(stderr, "VA-API %d.%d, driver: %s\n", major, minor,
            vaQueryVendorString(st->dpy));

    st->rgba_modifier = rgba_modifier_init(st);
    if (st->use_legacy_prime) {
        fprintf(stderr,
                "This GPU can't describe its RGBA render-target tiling as a DRM modifier -- "
                "using the old, pre-modifier VA-API import path for it instead (see DEVLOG "
                "Tier 5.12).\n");
    }

    int num_entrypoints = vaMaxNumEntrypoints(st->dpy);
    VAEntrypoint *entrypoints = malloc(num_entrypoints * sizeof(VAEntrypoint));
    int n = 0;
    CHECK_VA(vaQueryConfigEntrypoints(st->dpy, VAProfileH264ConstrainedBaseline,
                                       entrypoints, &n),
             "vaQueryConfigEntrypoints");
    st->entrypoint = (VAEntrypoint)-1;
    for (int i = 0; i < n; i++) {
        if (entrypoints[i] == VAEntrypointEncSlice || entrypoints[i] == VAEntrypointEncSliceLP) {
            st->entrypoint = entrypoints[i];
            break;
        }
    }
    free(entrypoints);
    if (st->entrypoint == (VAEntrypoint)-1) {
        fprintf(stderr, "No H.264 encode entrypoint on this GPU\n");
        return -1;
    }
    fprintf(stderr, "Using entrypoint %s\n",
            st->entrypoint == VAEntrypointEncSlice ? "VAEntrypointEncSlice" : "VAEntrypointEncSliceLP");

    VAConfigAttrib attrib = {.type = VAConfigAttribRTFormat, .value = VA_RT_FORMAT_YUV420};
    CHECK_VA(vaCreateConfig(st->dpy, VAProfileH264ConstrainedBaseline, st->entrypoint,
                             &attrib, 1, &st->config_id),
             "vaCreateConfig");

    /* VPP config for the RGBA->NV12 conversion stage (Tier 5.8). No RT
     * format restriction needed here -- confirmed via a standalone probe
     * that this driver's VPP surface attributes already list both RGBA-
     * family and YUV-family pixel formats for VAProfileNone/VideoProc. */
    CHECK_VA(vaCreateConfig(st->dpy, VAProfileNone, VAEntrypointVideoProc, NULL, 0,
                             &st->vpp_config_id),
             "vaCreateConfig(VideoProc)");

    st->have_context = 0;
    return 0;
}

/* (Re)creates the context + recon surface for a new resolution. The
 * imported input surface (surfaces[0]) is created fresh per request
 * regardless, since it wraps a different dma-buf every time. */
static int vaapi_state_ensure_resolution(vaapi_state_t *st, unsigned int width, unsigned int height) {
    if (st->have_context && st->width == width && st->height == height) return 0;
    if (st->have_context) {
        vaDestroyContext(st->dpy, st->context_id);
        vaDestroyContext(st->dpy, st->vpp_context_id);
        vaDestroySurfaces(st->dpy, &st->surfaces[1], 1);
        vaDestroySurfaces(st->dpy, &st->surfaces[2], 1);
        st->have_context = 0;
    }
    CHECK_VA(vaCreateSurfaces(st->dpy, VA_RT_FORMAT_YUV420, width, height,
                               &st->surfaces[1], 1, NULL, 0),
             "vaCreateSurfaces(recon)");
    CHECK_VA(vaCreateContext(st->dpy, st->config_id, width, height, VA_PROGRESSIVE,
                              &st->surfaces[1], 1, &st->context_id),
             "vaCreateContext");

    /* Tier 5.8: the NV12 surface VPP converts into and the encode context
     * then reads from directly -- allocated internally by the driver
     * (vaCreateSurfaces with no import attribs), so unlike the RGBA input
     * there's no gralloc tiling/modifier to worry about here at all. */
    CHECK_VA(vaCreateSurfaces(st->dpy, VA_RT_FORMAT_YUV420, width, height,
                               &st->surfaces[2], 1, NULL, 0),
             "vaCreateSurfaces(vpp-nv12)");
    CHECK_VA(vaCreateContext(st->dpy, st->vpp_config_id, width, height, VA_PROGRESSIVE,
                              &st->surfaces[2], 1, &st->vpp_context_id),
             "vaCreateContext(VideoProc)");

    st->width = width;
    st->height = height;
    st->have_context = 1;
    return 0;
}

/* Tier 5.8: converts the imported RGBA surface into st->surfaces[2] (NV12)
 * entirely on the GPU via VA-API's video post-processing pipeline -- the
 * only VA-API mechanism this driver offers for turning what
 * GraphicBufferSource actually hands the encoder (RGBA; see DEVLOG for why
 * getting real YUV out of GraphicBufferSource itself isn't viable on this
 * Mesa/minigbm stack) into something VCN's encode block can read at all. */
static int convert_rgba_to_nv12(vaapi_state_t *st, VASurfaceID rgba_surface,
                                 unsigned int width, unsigned int height) {
    VARectangle region = {.x = 0, .y = 0, .width = (short)width, .height = (short)height};

    VAProcPipelineParameterBuffer pipeline_param = {0};
    pipeline_param.surface = rgba_surface;
    pipeline_param.surface_region = &region;
    pipeline_param.output_region = &region;
    pipeline_param.surface_color_standard = VAProcColorStandardNone;
    pipeline_param.output_color_standard = VAProcColorStandardBT601;

    VABufferID pipeline_buf;
    CHECK_VA(vaCreateBuffer(st->dpy, st->vpp_context_id, VAProcPipelineParameterBufferType,
                             sizeof(pipeline_param), 1, &pipeline_param, &pipeline_buf),
             "vaCreateBuffer(vpp-pipeline)");

    CHECK_VA(vaBeginPicture(st->dpy, st->vpp_context_id, st->surfaces[2]), "vaBeginPicture(vpp)");
    CHECK_VA(vaRenderPicture(st->dpy, st->vpp_context_id, &pipeline_buf, 1), "vaRenderPicture(vpp)");
    CHECK_VA(vaEndPicture(st->dpy, st->vpp_context_id), "vaEndPicture(vpp)");
    CHECK_VA(vaSyncSurface(st->dpy, st->surfaces[2]), "vaSyncSurface(vpp)");

    vaDestroyBuffer(st->dpy, pipeline_buf);
    return 0;
}

/* Imports the client's dma-buf as surfaces[0], runs the same encode
 * sequence tier2/tier3 already proved, and returns malloc'd Annex-B H.264
 * bytes (caller frees). Returns byte count, or -1 on error. */
static long encode_one_frame(vaapi_state_t *st, int dmabuf_fd, const EncodeRequest *req,
                              unsigned char **out_buf) {
    if (vaapi_state_ensure_resolution(st, req->width, req->height) != 0) return -1;

    uint32_t import_stride = req->stride_y;
    uint32_t import_size = req->dmabuf_size;
    uint64_t import_modifier = st->rgba_modifier;

    /* Tier 5.12: GPUs where no DRM modifier exists for this buffer class at
     * all (e.g. Polaris/GFX8's opaque TILE_TYPE_DRI, see rgba_modifier_init())
     * use the OLD, pre-modifier VA-API import (VASurfaceAttribExternalBuffers
     * / VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME) directly on the real fd instead
     * of DRM_PRIME_2 -- confirmed working, including cross-process, unlike
     * the Tier 5.10 GL-bridge attempt this replaced (EGL's plain dma-buf
     * import only resolves opaque tiling within the same process that
     * allocated the buffer; VA-API's own import machinery doesn't share that
     * limitation, likely because libva is built around cross-process IPC as
     * the normal case to begin with, unlike EGL/GL). */
    int use_legacy_prime_import = st->use_legacy_prime;

    /* Tier 5.7 finding: VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME (the old path
     * used up through Tier 5.6) has no way to describe a DRM format
     * modifier -- it always assumes the buffer is a plain linear raster.
     * That held for every earlier tier's synthetic/dumb buffers (which
     * genuinely were linear) but not for a real gralloc-allocated Surface
     * buffer, which can be GPU-tiled even with DCC compression disabled
     * (AMD_DEBUG=nodcc only affects compression, not tiling) -- importing a
     * tiled buffer this way produced a garbled, striped decode. DRM_PRIME_2
     * (VADRMPRIMESurfaceDescriptor) carries the modifier the component read
     * out of cros_gralloc's native handle, so the driver interprets the
     * memory layout correctly regardless of tiling.
     *
     * Tier 5.8 finding: the buffer itself is RGBA, not NV12 (Tier 5.7's
     * closing discovery -- GraphicBufferSource hands the encoder
     * SurfaceFlinger's raw GL-composited output, and this Mesa/minigbm
     * stack cannot allocate a buffer that's both GPU-renderable and real
     * YUV, see DEVLOG). VA_FOURCC_RGBA's byte order matches Android's
     * RGBA_8888 directly (confirmed back in tier3-dmabuf-import); the DRM
     * equivalent is DRM_FORMAT_ABGR8888, not DRM_FORMAT_RGBA8888 -- DRM
     * format names describe *bit* packing in a little-endian 32-bit word,
     * which inverts to the opposite letter order when read as bytes.
     *
     * Tier 5.9: the modifier comes from st->rgba_modifier (this host's own
     * GPU, determined once at daemon startup), not from the client's
     * request -- the Android-side component has no reliable way to read
     * its real value at all (see DEVLOG), and the value is GPU-generation-
     * specific besides, so asking this host's own driver directly is both
     * the only correct source and the one that needs no Android rebuild to
     * change machines. req->drm_format_modifier is intentionally ignored. */
    VADRMPRIMESurfaceDescriptor prime_desc = {0};
    VASurfaceAttribExternalBuffers legacy_ext_buf = {0};
    uintptr_t legacy_buffer_handles[1];
    VASurfaceAttrib import_attribs[2];
    VAStatus st_import;

    if (use_legacy_prime_import) {
        legacy_buffer_handles[0] = (uintptr_t)dmabuf_fd;
        legacy_ext_buf.pixel_format = VA_FOURCC_RGBA;
        legacy_ext_buf.width = req->width;
        legacy_ext_buf.height = req->height;
        legacy_ext_buf.data_size = req->dmabuf_size;
        legacy_ext_buf.num_planes = 1;
        legacy_ext_buf.pitches[0] = req->stride_y;
        legacy_ext_buf.offsets[0] = 0;
        legacy_ext_buf.buffers = legacy_buffer_handles;
        legacy_ext_buf.num_buffers = 1;

        import_attribs[0].type = VASurfaceAttribMemoryType;
        import_attribs[0].flags = VA_SURFACE_ATTRIB_SETTABLE;
        import_attribs[0].value.type = VAGenericValueTypeInteger;
        import_attribs[0].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME;
        import_attribs[1].type = VASurfaceAttribExternalBufferDescriptor;
        import_attribs[1].flags = VA_SURFACE_ATTRIB_SETTABLE;
        import_attribs[1].value.type = VAGenericValueTypePointer;
        import_attribs[1].value.value.p = &legacy_ext_buf;

        st_import = vaCreateSurfaces(st->dpy, VA_RT_FORMAT_RGB32, req->width, req->height,
                                      &st->surfaces[0], 1, import_attribs, 2);
    } else {
        prime_desc.fourcc = VA_FOURCC_RGBA;
        prime_desc.width = req->width;
        prime_desc.height = req->height;
        prime_desc.num_objects = 1;
        prime_desc.objects[0].fd = dmabuf_fd;
        prime_desc.objects[0].size = import_size;
        prime_desc.objects[0].drm_format_modifier = import_modifier;
        prime_desc.num_layers = 1;
        prime_desc.layers[0].drm_format = DRM_FORMAT_ABGR8888;
        prime_desc.layers[0].num_planes = 1;
        prime_desc.layers[0].object_index[0] = 0;
        prime_desc.layers[0].offset[0] = 0;
        prime_desc.layers[0].pitch[0] = import_stride;

        import_attribs[0].type = VASurfaceAttribMemoryType;
        import_attribs[0].flags = VA_SURFACE_ATTRIB_SETTABLE;
        import_attribs[0].value.type = VAGenericValueTypeInteger;
        import_attribs[0].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2;
        import_attribs[1].type = VASurfaceAttribExternalBufferDescriptor;
        import_attribs[1].flags = VA_SURFACE_ATTRIB_SETTABLE;
        import_attribs[1].value.type = VAGenericValueTypePointer;
        import_attribs[1].value.value.p = &prime_desc;

        st_import = vaCreateSurfaces(st->dpy, VA_RT_FORMAT_RGB32, req->width, req->height,
                                      &st->surfaces[0], 1, import_attribs, 2);
    }
    if (st_import != VA_STATUS_SUCCESS) {
        fprintf(stderr, "dma-buf import failed: %s\n", vaErrorStr(st_import));
        return -1;
    }

    if (convert_rgba_to_nv12(st, st->surfaces[0], req->width, req->height) != 0) {
        vaDestroySurfaces(st->dpy, &st->surfaces[0], 1);
        return -1;
    }

    unsigned int mb_width = align16(req->width) / 16;
    unsigned int mb_height = align16(req->height) / 16;

    VABufferID coded_buf;
    CHECK_VA(vaCreateBuffer(st->dpy, st->context_id, VAEncCodedBufferType,
                             req->width * req->height * 3, 1, NULL, &coded_buf),
             "vaCreateBuffer(coded)");

    VAEncSequenceParameterBufferH264 seq = {0};
    seq.seq_parameter_set_id = 0;
    seq.level_idc = 30;
    seq.intra_period = 1;
    seq.intra_idr_period = 1;
    seq.ip_period = 0;
    seq.bits_per_second = 2000000;
    seq.max_num_ref_frames = 1;
    seq.picture_width_in_mbs = mb_width;
    seq.picture_height_in_mbs = mb_height;
    seq.seq_fields.bits.frame_mbs_only_flag = 1;
    seq.seq_fields.bits.chroma_format_idc = 1;
    seq.seq_fields.bits.log2_max_frame_num_minus4 = 0;
    seq.seq_fields.bits.pic_order_cnt_type = 2;
    seq.frame_cropping_flag = (req->width != mb_width * 16 || req->height != mb_height * 16);
    seq.frame_crop_right_offset = (mb_width * 16 - req->width) / 2;
    seq.frame_crop_bottom_offset = (mb_height * 16 - req->height) / 2;

    VABufferID seq_buf;
    vaCreateBuffer(st->dpy, st->context_id, VAEncSequenceParameterBufferType, sizeof(seq), 1, &seq, &seq_buf);

    VAEncPictureParameterBufferH264 pic = {0};
    pic.CurrPic.picture_id = st->surfaces[1];
    for (int i = 0; i < 16; i++) {
        pic.ReferenceFrames[i].picture_id = VA_INVALID_ID;
        pic.ReferenceFrames[i].flags = VA_PICTURE_H264_INVALID;
    }
    pic.coded_buf = coded_buf;
    pic.pic_init_qp = 26;
    pic.pic_fields.bits.idr_pic_flag = 1;
    pic.pic_fields.bits.reference_pic_flag = 1;
    pic.pic_fields.bits.entropy_coding_mode_flag = 1;
    pic.pic_fields.bits.deblocking_filter_control_present_flag = 1;

    VABufferID pic_buf;
    vaCreateBuffer(st->dpy, st->context_id, VAEncPictureParameterBufferType, sizeof(pic), 1, &pic, &pic_buf);

    VAEncSliceParameterBufferH264 slice = {0};
    slice.num_macroblocks = mb_width * mb_height;
    slice.macroblock_info = VA_INVALID_ID;
    slice.slice_type = 2;
    slice.direct_spatial_mv_pred_flag = 1;
    slice.num_ref_idx_active_override_flag = 1;
    for (int i = 0; i < 32; i++) {
        slice.RefPicList0[i].picture_id = VA_INVALID_ID;
        slice.RefPicList0[i].flags = VA_PICTURE_H264_INVALID;
        slice.RefPicList1[i].picture_id = VA_INVALID_ID;
        slice.RefPicList1[i].flags = VA_PICTURE_H264_INVALID;
    }

    VABufferID slice_buf;
    vaCreateBuffer(st->dpy, st->context_id, VAEncSliceParameterBufferType, sizeof(slice), 1, &slice, &slice_buf);

    bitstream_t sps_bs, pps_bs, slice_hdr_bs;
    bs_init(&sps_bs);
    bs_init(&pps_bs);
    bs_init(&slice_hdr_bs);
    build_sps_rbsp(&sps_bs, &seq);
    build_pps_rbsp(&pps_bs, &pic);
    build_slice_header_bits(&slice_hdr_bs, &seq, &pic, &slice);

    VABufferID sps_param_buf, sps_data_buf, pps_param_buf, pps_data_buf;
    VABufferID slice_hdr_param_buf, slice_hdr_data_buf;
    submit_packed_header(st->dpy, st->context_id, VAEncPackedHeaderSequence, &sps_bs, &sps_param_buf, &sps_data_buf);
    submit_packed_header(st->dpy, st->context_id, VAEncPackedHeaderPicture, &pps_bs, &pps_param_buf, &pps_data_buf);
    submit_packed_header(st->dpy, st->context_id, VAEncPackedHeaderSlice, &slice_hdr_bs, &slice_hdr_param_buf, &slice_hdr_data_buf);

    CHECK_VA(vaBeginPicture(st->dpy, st->context_id, st->surfaces[2]), "vaBeginPicture");
    VABufferID b1[] = {seq_buf};
    vaRenderPicture(st->dpy, st->context_id, b1, 1);
    VABufferID b2[] = {sps_param_buf, sps_data_buf};
    vaRenderPicture(st->dpy, st->context_id, b2, 2);
    VABufferID b3[] = {pic_buf};
    vaRenderPicture(st->dpy, st->context_id, b3, 1);
    VABufferID b4[] = {pps_param_buf, pps_data_buf};
    vaRenderPicture(st->dpy, st->context_id, b4, 2);
    VABufferID b5[] = {slice_hdr_param_buf, slice_hdr_data_buf};
    vaRenderPicture(st->dpy, st->context_id, b5, 2);
    VABufferID b6[] = {slice_buf};
    vaRenderPicture(st->dpy, st->context_id, b6, 1);
    CHECK_VA(vaEndPicture(st->dpy, st->context_id), "vaEndPicture");
    CHECK_VA(vaSyncSurface(st->dpy, st->surfaces[2]), "vaSyncSurface");

    free(sps_bs.buf);
    free(pps_bs.buf);
    free(slice_hdr_bs.buf);

    VACodedBufferSegment *seg = NULL;
    CHECK_VA(vaMapBuffer(st->dpy, coded_buf, (void **)&seg), "vaMapBuffer(coded)");
    size_t total = 0;
    for (VACodedBufferSegment *s = seg; s != NULL; s = s->next) total += s->size;
    unsigned char *out = malloc(total);
    size_t off = 0;
    for (VACodedBufferSegment *s = seg; s != NULL; s = s->next) {
        memcpy(out + off, s->buf, s->size);
        off += s->size;
    }
    vaUnmapBuffer(st->dpy, coded_buf);
    vaDestroyBuffer(st->dpy, coded_buf);
    vaDestroySurfaces(st->dpy, &st->surfaces[0], 1);

    *out_buf = out;
    return (long)total;
}

/* ------------------------------------------------------------------ *
 * Socket server: one connection = one request/response, per protocol.h.
 * ------------------------------------------------------------------ */
static int recv_request(int conn_fd, EncodeRequest *req, int *out_dmabuf_fd) {
    char cmsg_buf[CMSG_SPACE(sizeof(int))];
    struct iovec iov = {.iov_base = req, .iov_len = sizeof(*req)};
    struct msghdr msg = {0};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cmsg_buf;
    msg.msg_controllen = sizeof(cmsg_buf);

    ssize_t n = recvmsg(conn_fd, &msg, 0);
    if (n != (ssize_t)sizeof(*req)) {
        if (n < 0) perror("recvmsg");
        else fprintf(stderr, "recvmsg: short read (%zd of %zu bytes)\n", n, sizeof(*req));
        return -1;
    }

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (!cmsg || cmsg->cmsg_type != SCM_RIGHTS) {
        fprintf(stderr, "recvmsg: no fd received (SCM_RIGHTS missing)\n");
        return -1;
    }
    memcpy(out_dmabuf_fd, CMSG_DATA(cmsg), sizeof(int));
    return 0;
}

static void send_response(int conn_fd, int32_t status, const unsigned char *buf, uint32_t size) {
    EncodeResponse resp = {.status = status, .coded_size = (status == 0) ? size : 0};
    if (write(conn_fd, &resp, sizeof(resp)) != (ssize_t)sizeof(resp)) {
        perror("write(response header)");
        return;
    }
    if (status == 0 && size > 0) {
        if (write(conn_fd, buf, size) != (ssize_t)size) perror("write(response body)");
    }
}

static void handle_connection(vaapi_state_t *st, int conn_fd) {
    EncodeRequest req;
    int dmabuf_fd = -1;
    if (recv_request(conn_fd, &req, &dmabuf_fd) != 0) {
        send_response(conn_fd, -1, NULL, 0);
        return;
    }

    fprintf(stderr, "Request: %ux%u, stride_y=%u stride_uv=%u offset_uv=%u dmabuf_size=%u fd=%d\n",
            req.width, req.height, req.stride_y, req.stride_uv, req.offset_uv, req.dmabuf_size, dmabuf_fd);

    unsigned char *out_buf = NULL;
    long n = encode_one_frame(st, dmabuf_fd, &req, &out_buf);
    close(dmabuf_fd);

    if (n < 0) {
        send_response(conn_fd, -1, NULL, 0);
    } else {
        fprintf(stderr, "Encoded %ld bytes\n", n);
        send_response(conn_fd, 0, out_buf, (uint32_t)n);
        free(out_buf);
    }
}

int main(void) {
    setvbuf(stderr, NULL, _IONBF, 0);

    /* Must happen before any Mesa entry point runs at all (vaInitialize
     * included) -- see rgba_modifier_init()'s comment for why. */
    setenv("AMD_DEBUG", "nodcc", 1);

    vaapi_state_t st;
    if (vaapi_state_init(&st) != 0) return 1;

    /* Parent directory is the shared bind-mount point between this host
     * process and the redroid container (see README.md) -- created here
     * so the daemon can run before that mount is even set up manually. */
    mkdir("/dev/vaapi-helper", 0755);
    unlink(VAAPI_DAEMON_SOCKET_PATH);

    int listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return 1;
    }
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, VAAPI_DAEMON_SOCKET_PATH, sizeof(addr.sun_path) - 1);
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("bind");
        return 1;
    }
    chmod(VAAPI_DAEMON_SOCKET_PATH, 0666);
    if (listen(listen_fd, 4) != 0) {
        perror("listen");
        return 1;
    }
    fprintf(stderr, "Listening on %s\n", VAAPI_DAEMON_SOCKET_PATH);

    while (1) {
        int conn_fd = accept(listen_fd, NULL, NULL);
        if (conn_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }
        handle_connection(&st, conn_fd);
        close(conn_fd);
    }

    close(listen_fd);
    return 0;
}
