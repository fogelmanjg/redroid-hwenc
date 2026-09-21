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
#define GL_GLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

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
    /* Tier 5.10: some GPUs (confirmed: Polaris/GFX8) report *zero* DRM
     * format modifiers for a GPU-render-target RGBA buffer at all -- there
     * is no modifier value that describes their tiling, because minigbm
     * falls back to Mesa-opaque, non-modifier tiling (TILE_TYPE_DRI) for
     * that combination. DRM_PRIME_2 fundamentally cannot import a buffer
     * like that (confirmed: a genuinely-linear buffer at the exact same
     * stride imports fine, so it's specifically about the tiling, not a
     * geometry mistake). needs_egl_bridge, set once at startup, means
     * encode_one_frame() bounces the buffer through GL first instead --
     * see egl_bridge_convert_to_linear()'s own comment for the important
     * caveat: this does NOT actually fix Polaris/GFX8 (confirmed cross-
     * process import still fails there), it's kept because the mechanism
     * may still be exactly right for some *other* GPU whose opaque tiling
     * genuinely can be resolved cross-process, which hasn't been tried. */
    int needs_egl_bridge;
    struct gbm_device *gbm;
    EGLDisplay egl_dpy;
    EGLContext egl_ctx;
    GLuint blit_program;
    GLint blit_tex_uniform;
    GLuint blit_vbo;
} vaapi_state_t;

static PFNEGLCREATEIMAGEKHRPROC pEglCreateImageKHR;
static PFNEGLDESTROYIMAGEKHRPROC pEglDestroyImageKHR;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC pGlEGLImageTargetTexture2DOES;

static GLuint compile_shader(GLenum type, const char *src) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, NULL);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        fprintf(stderr, "shader compile failed: %s\n", log);
        return 0;
    }
    return shader;
}

static GLuint link_blit_program(void) {
    /* Deliberately trivial: samples the source texture and writes it
     * straight through. Both ends of this copy are plain offscreen
     * buffers (no window-system Y-flip conventions in play), so a direct
     * 1:1 mapping is correct. */
    static const char *vs_src =
        "attribute vec2 aPos;\n"
        "varying vec2 vTex;\n"
        "void main() {\n"
        "    vTex = aPos * 0.5 + 0.5;\n"
        "    gl_Position = vec4(aPos, 0.0, 1.0);\n"
        "}\n";
    static const char *fs_src =
        "precision mediump float;\n"
        "varying vec2 vTex;\n"
        "uniform sampler2D uTex;\n"
        "void main() {\n"
        "    gl_FragColor = texture2D(uTex, vTex);\n"
        "}\n";
    GLuint vs = compile_shader(GL_VERTEX_SHADER, vs_src);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fs_src);
    if (!vs || !fs) return 0;
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glBindAttribLocation(prog, 0, "aPos");
    glLinkProgram(prog);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(prog, sizeof(log), NULL, log);
        fprintf(stderr, "program link failed: %s\n", log);
        return 0;
    }
    return prog;
}

/* Tier 5.10: the bridge's persistent GBM+EGL+GLES2 context, created only
 * when rgba_modifier_init() below has determined it's actually needed --
 * confirmed the hard way that merely *creating* a GBM device + EGL display
 * in this process at all (even one immediately destroyed again right after
 * a modifier query, never advancing to a context) made VA-API's own,
 * unrelated dma-buf import start failing with "resource allocation failed"
 * on server01 -- for a request that worked before any of this existed, on
 * a GPU that never needs the bridge. Whatever that interaction is, this
 * daemon now only ever creates GBM/EGL state on a GPU that has already
 * been confirmed (via rgba_modifier_init()'s own short-lived, self-
 * contained probe) to need it. */
static int gl_bridge_context_init(vaapi_state_t *st) {
    int gbm_fd = open(DRM_RENDER_DEVICE, O_RDWR);
    if (gbm_fd < 0) {
        perror("gl_bridge_context_init: open " DRM_RENDER_DEVICE);
        return -1;
    }
    st->gbm = gbm_create_device(gbm_fd);
    if (!st->gbm) {
        fprintf(stderr, "gl_bridge_context_init: gbm_create_device failed\n");
        return -1;
    }
    st->egl_dpy = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, st->gbm, NULL);
    EGLint major, minor;
    if (st->egl_dpy == EGL_NO_DISPLAY || !eglInitialize(st->egl_dpy, &major, &minor)) {
        fprintf(stderr, "gl_bridge_context_init: eglInitialize failed\n");
        return -1;
    }
    eglBindAPI(EGL_OPENGL_ES_API);
    /* No EGL_SURFACE_TYPE constraint: this is a surfaceless context (never
     * bound to a real window/pbuffer surface), and asking for
     * EGL_PBUFFER_BIT here fails outright on a GBM-platform display that
     * doesn't offer pbuffer-capable configs (confirmed the hard way in the
     * standalone probe -- silent EGL_BAD_CONFIG cascading into every later
     * call quietly no-op'ing against no current context). */
    EGLint cfg_attribs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE};
    EGLConfig cfg;
    EGLint num_cfg = 0;
    eglChooseConfig(st->egl_dpy, cfg_attribs, &cfg, 1, &num_cfg);
    if (num_cfg == 0) {
        fprintf(stderr, "gl_bridge_context_init: eglChooseConfig found no config\n");
        return -1;
    }
    EGLint ctx_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    st->egl_ctx = eglCreateContext(st->egl_dpy, cfg, EGL_NO_CONTEXT, ctx_attribs);
    if (st->egl_ctx == EGL_NO_CONTEXT) {
        fprintf(stderr, "gl_bridge_context_init: eglCreateContext failed: 0x%x\n", eglGetError());
        return -1;
    }
    if (!eglMakeCurrent(st->egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, st->egl_ctx)) {
        fprintf(stderr, "gl_bridge_context_init: eglMakeCurrent failed: 0x%x\n", eglGetError());
        return -1;
    }

    pEglCreateImageKHR = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    pEglDestroyImageKHR = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    pGlEGLImageTargetTexture2DOES =
        (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    if (!pEglCreateImageKHR || !pEglDestroyImageKHR || !pGlEGLImageTargetTexture2DOES) {
        fprintf(stderr, "gl_bridge_context_init: missing required EGL/GL extension entry points\n");
        return -1;
    }

    st->blit_program = link_blit_program();
    if (!st->blit_program) return -1;
    st->blit_tex_uniform = glGetUniformLocation(st->blit_program, "uTex");

    static const GLfloat quad[] = {-1, -1, 1, -1, -1, 1, 1, 1};
    glGenBuffers(1, &st->blit_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, st->blit_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);

    return 0;
}

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
 * Tier 5.10: zero modifiers reported doesn't mean LINEAR here -- it means
 * this GPU can't describe its render-target tiling as a modifier at all
 * (confirmed: Polaris/GFX8). Sets st->needs_egl_bridge in that case instead
 * of silently assuming LINEAR (which produced "resource allocation failed"
 * on that GPU -- a genuinely-linear buffer at the identical stride imports
 * fine, proving the real buffer just isn't linear).
 *
 * Deliberately self-contained (creates and destroys its own throwaway
 * GBM/EGL, using st->drm_fd) rather than reusing any longer-lived state --
 * confirmed the hard way (see gl_bridge_context_init()'s comment) that
 * *any* longer-lived GBM/EGL object in this process, even on a GPU that
 * never touches it again, disturbs VA-API's own unrelated dma-buf import.
 * A query-and-destroy right here, before VA-API has imported anything for
 * real yet, doesn't have that problem. */
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
                "enabling the EGL bridge\n");
        st->needs_egl_bridge = 1;
        gbm_device_destroy(gbm);
        return fallback;
    }
    EGLuint64KHR *mods = malloc(num_mods * sizeof(EGLuint64KHR));
    EGLBoolean *external = malloc(num_mods * sizeof(EGLBoolean));
    queryMods(dpy, DRM_FORMAT_ABGR8888, num_mods, mods, external, &num_mods);
    uint64_t chosen = mods[0];
    fprintf(stderr, "rgba_modifier_init: %d modifiers for ABGR8888, using first: 0x%016llx\n",
            num_mods, (unsigned long long)chosen);
    free(mods);
    free(external);
    gbm_device_destroy(gbm);
    return chosen;
}

/* Tier 5.10: the bridge. Imports src_fd (whatever opaque tiling this GPU
 * gave the real buffer) via plain, non-modifier EGL import, GPU-blits it
 * into a fresh GBM_BO_USE_LINEAR buffer (this driver's own explicit,
 * always-linear allocation path -- confirmed a real combination exists by
 * simply trying it), and hands back that buffer's own fd/stride/size for
 * the caller to import into VA-API exactly like any other request, with
 * modifier=DRM_FORMAT_MOD_LINEAR (now actually true). Caller owns *out_bo
 * and must gbm_bo_destroy() it (which also invalidates *out_fd) once done
 * with the encode.
 *
 * IMPORTANT, confirmed on Polaris/GFX8 (see DEVLOG Tier 5.10 for the full
 * story): the *source* import below (src_image) only actually works when
 * the process calling this function is the same one that allocated
 * src_fd's buffer. It fails with GL_INVALID_VALUE for a genuinely foreign
 * fd -- e.g. exactly the real case, this daemon receiving src_fd from a
 * different process over SCM_RIGHTS. Mesa's same-process success is
 * apparently a shortcut (recognizing its own already-tracked GEM object),
 * not genuine cross-process opaque-tiling resolution. This function is not
 * a working fix for that GPU. Left in (reachable only when
 * needs_egl_bridge is set) because a *different* GPU's opaque tiling might
 * genuinely be cross-process-resolvable this way -- untested, no such GPU
 * available yet. */
static int egl_bridge_convert_to_linear(vaapi_state_t *st, int src_fd, uint32_t width,
                                         uint32_t height, uint32_t src_stride,
                                         struct gbm_bo **out_bo, int *out_fd, uint32_t *out_stride,
                                         uint32_t *out_size) {
    eglMakeCurrent(st->egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, st->egl_ctx);

    EGLint src_attribs[] = {
        EGL_WIDTH,
        (EGLint)width,
        EGL_HEIGHT,
        (EGLint)height,
        EGL_LINUX_DRM_FOURCC_EXT,
        DRM_FORMAT_ABGR8888,
        EGL_DMA_BUF_PLANE0_FD_EXT,
        src_fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT,
        0,
        EGL_DMA_BUF_PLANE0_PITCH_EXT,
        (EGLint)src_stride,
        EGL_NONE,
    };
    EGLImageKHR src_image = pEglCreateImageKHR(st->egl_dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT,
                                                (EGLClientBuffer)NULL, src_attribs);
    if (src_image == EGL_NO_IMAGE_KHR) {
        fprintf(stderr, "egl_bridge: source import failed: 0x%x\n", eglGetError());
        return -1;
    }

    GLuint src_tex = 0;
    glGenTextures(1, &src_tex);
    glBindTexture(GL_TEXTURE_2D, src_tex);
    pGlEGLImageTargetTexture2DOES(GL_TEXTURE_2D, src_image);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (glGetError() != GL_NO_ERROR) {
        fprintf(stderr, "egl_bridge: binding source EGLImage as texture failed\n");
        glDeleteTextures(1, &src_tex);
        pEglDestroyImageKHR(st->egl_dpy, src_image);
        return -1;
    }

    struct gbm_bo *dst_bo = gbm_bo_create(st->gbm, width, height, GBM_FORMAT_ABGR8888,
                                           GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
    if (!dst_bo) {
        fprintf(stderr, "egl_bridge: gbm_bo_create(LINEAR) failed\n");
        glDeleteTextures(1, &src_tex);
        pEglDestroyImageKHR(st->egl_dpy, src_image);
        return -1;
    }
    int dst_fd = gbm_bo_get_fd(dst_bo);
    uint32_t dst_stride = gbm_bo_get_stride(dst_bo);

    EGLint dst_attribs[] = {
        EGL_WIDTH,
        (EGLint)width,
        EGL_HEIGHT,
        (EGLint)height,
        EGL_LINUX_DRM_FOURCC_EXT,
        DRM_FORMAT_ABGR8888,
        EGL_DMA_BUF_PLANE0_FD_EXT,
        dst_fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT,
        0,
        EGL_DMA_BUF_PLANE0_PITCH_EXT,
        (EGLint)dst_stride,
        EGL_NONE,
    };
    EGLImageKHR dst_image = pEglCreateImageKHR(st->egl_dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT,
                                                (EGLClientBuffer)NULL, dst_attribs);
    if (dst_image == EGL_NO_IMAGE_KHR) {
        fprintf(stderr, "egl_bridge: dest import failed: 0x%x\n", eglGetError());
        glDeleteTextures(1, &src_tex);
        pEglDestroyImageKHR(st->egl_dpy, src_image);
        close(dst_fd);
        gbm_bo_destroy(dst_bo);
        return -1;
    }

    GLuint dst_tex = 0;
    glGenTextures(1, &dst_tex);
    glBindTexture(GL_TEXTURE_2D, dst_tex);
    pGlEGLImageTargetTexture2DOES(GL_TEXTURE_2D, dst_image);

    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dst_tex, 0);
    GLenum fbo_status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (fbo_status != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "egl_bridge: destination framebuffer incomplete: 0x%x\n", fbo_status);
        glDeleteFramebuffers(1, &fbo);
        glDeleteTextures(1, &src_tex);
        glDeleteTextures(1, &dst_tex);
        pEglDestroyImageKHR(st->egl_dpy, src_image);
        pEglDestroyImageKHR(st->egl_dpy, dst_image);
        close(dst_fd);
        gbm_bo_destroy(dst_bo);
        return -1;
    }

    glViewport(0, 0, (GLint)width, (GLint)height);
    glUseProgram(st->blit_program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, src_tex);
    glUniform1i(st->blit_tex_uniform, 0);
    glBindBuffer(GL_ARRAY_BUFFER, st->blit_vbo);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
    glEnableVertexAttribArray(0);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glFinish();

    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &src_tex);
    glDeleteTextures(1, &dst_tex);
    pEglDestroyImageKHR(st->egl_dpy, src_image);
    pEglDestroyImageKHR(st->egl_dpy, dst_image);

    /* Same reasoning as gl_bridge_init(): don't leave this context current
     * once the GL work is done, or VA-API's own import of the *result*
     * (which happens right after this returns, still within the same
     * request) can fail. */
    eglMakeCurrent(st->egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

    *out_bo = dst_bo;
    *out_fd = dst_fd;
    *out_stride = dst_stride;
    *out_size = dst_stride * height;
    return 0;
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
    if (st->needs_egl_bridge) {
        fprintf(stderr,
                "This GPU can't describe its RGBA render-target tiling as a DRM modifier -- "
                "every request will be bounced through a GL blit into a fresh linear buffer "
                "first (see DEVLOG Tier 5.10).\n");
        if (gl_bridge_context_init(st) != 0) {
            fprintf(stderr, "gl_bridge_context_init failed -- cannot set up the required bridge\n");
            return -1;
        }
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

    int import_fd = dmabuf_fd;
    uint32_t import_stride = req->stride_y;
    uint32_t import_size = req->dmabuf_size;
    uint64_t import_modifier = st->rgba_modifier;
    struct gbm_bo *bridge_bo = NULL;
    int bridge_fd = -1;

    /* Tier 5.10: this GPU can't describe the real buffer's tiling as a DRM
     * modifier at all (see rgba_modifier_init()) -- bounce it through GL
     * first so VA-API only ever has to deal with a buffer it already knows
     * how to import correctly. */
    if (st->needs_egl_bridge) {
        if (egl_bridge_convert_to_linear(st, dmabuf_fd, req->width, req->height, req->stride_y,
                                          &bridge_bo, &bridge_fd, &import_stride,
                                          &import_size) != 0) {
            return -1;
        }
        import_fd = bridge_fd;
        import_modifier = DRM_FORMAT_MOD_LINEAR;
    }

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
    prime_desc.fourcc = VA_FOURCC_RGBA;
    prime_desc.width = req->width;
    prime_desc.height = req->height;
    prime_desc.num_objects = 1;
    prime_desc.objects[0].fd = import_fd;
    prime_desc.objects[0].size = import_size;
    prime_desc.objects[0].drm_format_modifier = import_modifier;
    prime_desc.num_layers = 1;
    prime_desc.layers[0].drm_format = DRM_FORMAT_ABGR8888;
    prime_desc.layers[0].num_planes = 1;
    prime_desc.layers[0].object_index[0] = 0;
    prime_desc.layers[0].offset[0] = 0;
    prime_desc.layers[0].pitch[0] = import_stride;

    VASurfaceAttrib import_attribs[2];
    import_attribs[0].type = VASurfaceAttribMemoryType;
    import_attribs[0].flags = VA_SURFACE_ATTRIB_SETTABLE;
    import_attribs[0].value.type = VAGenericValueTypeInteger;
    import_attribs[0].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2;
    import_attribs[1].type = VASurfaceAttribExternalBufferDescriptor;
    import_attribs[1].flags = VA_SURFACE_ATTRIB_SETTABLE;
    import_attribs[1].value.type = VAGenericValueTypePointer;
    import_attribs[1].value.value.p = &prime_desc;

    VAStatus st_import = vaCreateSurfaces(st->dpy, VA_RT_FORMAT_RGB32, req->width, req->height,
                                           &st->surfaces[0], 1, import_attribs, 2);
    if (st_import != VA_STATUS_SUCCESS) {
        fprintf(stderr, "dma-buf import failed: %s\n", vaErrorStr(st_import));
        if (bridge_bo) {
            close(bridge_fd);
            gbm_bo_destroy(bridge_bo);
        }
        return -1;
    }

    if (convert_rgba_to_nv12(st, st->surfaces[0], req->width, req->height) != 0) {
        vaDestroySurfaces(st->dpy, &st->surfaces[0], 1);
        if (bridge_bo) {
            close(bridge_fd);
            gbm_bo_destroy(bridge_bo);
        }
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
    if (bridge_bo) {
        close(bridge_fd);
        gbm_bo_destroy(bridge_bo);
    }

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
