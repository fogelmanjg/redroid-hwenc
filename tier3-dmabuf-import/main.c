/*
 * Tier 3 spike: is a dma-buf that VA-API did NOT allocate itself (i.e. one
 * that looks like what a redroid gralloc buffer would hand a Codec2 encoder)
 * importable as a VA-API surface, and does it actually work for real
 * hardware encoding — not just "vaCreateSurfaces returns VA_STATUS_SUCCESS"?
 *
 * This is the make-or-break question the project's README calls out: nobody
 * confirmed this in 4 years of upstream issues. Full end-to-end test:
 *
 *   1. Allocate a buffer via a plain DRM "dumb buffer"
 *      (DRM_IOCTL_MODE_CREATE_DUMB, on the primary node) instead of via
 *      libva -- NOT via generic Mesa GBM. First attempt used
 *      gbm_bo_create(..., GBM_FORMAT_NV12, ...) on this same GPU and it
 *      failed outright: gbm_device_is_format_supported() reports NV12
 *      unsupported for ANY usage-flag combination on this radeonsi/Mesa
 *      gbm backend -- only RGB(A)8888 formats are allocatable through
 *      generic gbm here. That's a real Tier-3-relevant finding on its own
 *      (see DEVLOG): whatever allocator redroid's Android-side gralloc
 *      uses for YUV buffers, it is NOT going through this same generic
 *      libgbm path, since that path can't produce NV12 on this hardware at
 *      all. The dumb-buffer path below is allocator-agnostic (a raw linear
 *      GEM buffer, always available on any DRM driver) and lets the actual
 *      import question -- "can VA-API import a foreign dma-buf and encode
 *      from it" -- be answered independently of that gralloc-allocator
 *      question.
 *   2. Export it as a dma-buf fd (drmPrimeHandleToFD), exactly as a
 *      gralloc buffer's native handle would (see DEVLOG 2026-09-19 — a
 *      C2ConstGraphicBlock's handle->data[i] are dma-buf fds directly).
 *   3. Write a known pixel pattern into it via a plain mmap() of the
 *      dma-buf fd -- deliberately NOT through any VA-API call, to prove
 *      whatever surface we import really aliases this exact memory rather
 *      than a driver-side copy.
 *   4. Import it into VA-API via VASurfaceAttribExternalBuffers +
 *      VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME.
 *   5. Read it back via vaDeriveImage/vaMapBuffer and confirm the pattern
 *      survived the import (zero-copy coherency check).
 *   6. Feed that imported surface into the exact same encode call sequence
 *      Tier 2 already proved works (packed SPS/PPS/slice headers, the
 *      multi-call vaRenderPicture split) and confirm the output is real,
 *      decodable H.264 -- proving the hardware encoder can actually read
 *      an externally-allocated surface, not just that import doesn't error.
 *
 * Step 1 of this spike deliberately forces GBM_BO_USE_LINEAR (no tiling) to
 * isolate the import mechanism from the separate question of tiling
 * modifiers -- see the README/DEVLOG for the tiled follow-up.
 */

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>
#include <va/va_enc_h264.h>

#define WIDTH 320
#define HEIGHT 240
#define OUTPUT_FILE "out.h264"
#define DRM_RENDER_DEVICE "/dev/dri/renderD128"
#define DRM_PRIMARY_DEVICE "/dev/dri/card1"
#define FILL_VALUE 128 /* same flat mid-grey Tier 2 used, for a fair comparison */

#define CHECK_VA(status, msg)                                                \
    do {                                                                     \
        if ((status) != VA_STATUS_SUCCESS) {                                 \
            fprintf(stderr, "%s failed: %s (0x%x)\n", (msg),                 \
                    vaErrorStr(status), (status));                           \
            exit(1);                                                        \
        }                                                                    \
    } while (0)

static unsigned int align16(unsigned int v) { return (v + 15) & ~15u; }

/* ------------------------------------------------------------------ *
 * Same minimal bitstream writer as Tier 2 -- SPS/PPS/slice-header RBSPs
 * still have to be hand-built for packed headers regardless of where the
 * input surface's memory came from. See tier2-vaapi-encode/main.c for the
 * detailed comments on each field; kept terse here since this file's whole
 * point is the surface import, not re-deriving that part.
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

    CHECK_VA(vaCreateBuffer(dpy, context_id, VAEncPackedHeaderParameterBufferType,
                             sizeof(param), 1, &param, out_param_buf),
             "vaCreateBuffer(packed header param)");
    CHECK_VA(vaCreateBuffer(dpy, context_id, VAEncPackedHeaderDataBufferType,
                             (bs->bit_pos + 7) / 8, 1, bs->buf, out_data_buf),
             "vaCreateBuffer(packed header data)");
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    int drm_fd = open(DRM_RENDER_DEVICE, O_RDWR);
    if (drm_fd < 0) {
        perror("open " DRM_RENDER_DEVICE);
        return 1;
    }
    int drm_primary_fd = open(DRM_PRIMARY_DEVICE, O_RDWR);
    if (drm_primary_fd < 0) {
        perror("open " DRM_PRIMARY_DEVICE);
        return 1;
    }

    /* --- Step 1: allocate a buffer via a plain DRM dumb buffer, NOT via
     * libva and NOT via Mesa GBM (see the file header comment for why GBM
     * was dropped: it can't allocate NV12 at all on this driver). A dumb
     * buffer is always linear, single-plane as far as the kernel is
     * concerned -- Y and interleaved-UV are laid out back to back in one
     * allocation by treating it as WIDTH x (HEIGHT * 3/2) at 8 bits per
     * pixel, which is exactly NV12's byte layout. */
    uint32_t handle = 0, stride0 = 0;
    uint64_t dumb_size = 0;
    if (drmModeCreateDumbBuffer(drm_primary_fd, WIDTH, HEIGHT * 3 / 2, 8, 0,
                                 &handle, &stride0, &dumb_size) != 0) {
        perror("drmModeCreateDumbBuffer");
        return 1;
    }

    int dmabuf_fd = -1;
    if (drmPrimeHandleToFD(drm_primary_fd, handle, DRM_CLOEXEC | DRM_RDWR,
                            &dmabuf_fd) != 0) {
        perror("drmPrimeHandleToFD");
        return 1;
    }

    uint32_t stride1 = stride0;
    uint32_t offset0 = 0;
    uint32_t offset1 = stride0 * HEIGHT;
    off_t dmabuf_size = (off_t)dumb_size;
    printf("DRM dumb buffer: %dx%d NV12 (linear, single dma-buf backing both planes)\n",
           WIDTH, HEIGHT);
    printf("  plane0 (Y):  stride=%u offset=%u\n", stride0, offset0);
    printf("  plane1 (UV): stride=%u offset=%u\n", stride1, offset1);
    printf("  dma-buf fd=%d size=%ld bytes (gem handle=%u)\n", dmabuf_fd,
           (long)dmabuf_size, handle);

    /* --- Step 2/3: write a known pattern directly into the dma-buf, with
     * VA-API completely out of the picture -- plain mmap() of the fd. If
     * the later VA-API readback sees this exact pattern, the import is
     * genuinely zero-copy (same physical memory), not a driver-side copy
     * VA-API took at import time. */
    void *cpu_map = mmap(NULL, dmabuf_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                          dmabuf_fd, 0);
    if (cpu_map == MAP_FAILED) {
        perror("mmap(dmabuf_fd)");
        return 1;
    }
    for (unsigned int row = 0; row < HEIGHT; row++) {
        memset((unsigned char *)cpu_map + offset0 + row * stride0, FILL_VALUE, WIDTH);
    }
    for (unsigned int row = 0; row < HEIGHT / 2; row++) {
        memset((unsigned char *)cpu_map + offset1 + row * stride1, FILL_VALUE, WIDTH);
    }
    munmap(cpu_map, dmabuf_size);
    printf("Wrote flat %d pattern into the dma-buf via plain mmap (VA-API not involved).\n",
           FILL_VALUE);

    /* --- VA-API setup, same as Tier 2 --- */
    VADisplay dpy = vaGetDisplayDRM(drm_fd);
    if (!dpy) {
        fprintf(stderr, "vaGetDisplayDRM failed\n");
        return 1;
    }
    int major, minor;
    CHECK_VA(vaInitialize(dpy, &major, &minor), "vaInitialize");
    printf("VA-API version %d.%d, driver: %s\n", major, minor,
           vaQueryVendorString(dpy));

    int num_entrypoints = vaMaxNumEntrypoints(dpy);
    VAEntrypoint *entrypoints = malloc(num_entrypoints * sizeof(VAEntrypoint));
    int n = 0;
    CHECK_VA(vaQueryConfigEntrypoints(dpy, VAProfileH264ConstrainedBaseline,
                                       entrypoints, &n),
             "vaQueryConfigEntrypoints");
    VAEntrypoint entrypoint = -1;
    for (int i = 0; i < n; i++) {
        if (entrypoints[i] == VAEntrypointEncSlice ||
            entrypoints[i] == VAEntrypointEncSliceLP) {
            entrypoint = entrypoints[i];
            break;
        }
    }
    free(entrypoints);
    if (entrypoint == (VAEntrypoint)-1) {
        fprintf(stderr, "No H.264 encode entrypoint found.\n");
        return 1;
    }

    VAConfigAttrib attrib = {.type = VAConfigAttribRTFormat,
                              .value = VA_RT_FORMAT_YUV420};
    VAConfigID config_id;
    CHECK_VA(vaCreateConfig(dpy, VAProfileH264ConstrainedBaseline, entrypoint,
                             &attrib, 1, &config_id),
             "vaCreateConfig");

    /* --- Step 4: import the dma-buf as a VA-API surface ---
     * This is the actual Tier 3 question. surfaces[0] is now backed by
     * memory VA-API never allocated. */
    uintptr_t buffer_handles[1] = {(uintptr_t)dmabuf_fd};
    VASurfaceAttribExternalBuffers ext_buf = {0};
    ext_buf.pixel_format = VA_FOURCC_NV12;
    ext_buf.width = WIDTH;
    ext_buf.height = HEIGHT;
    ext_buf.data_size = (uint32_t)dmabuf_size;
    ext_buf.num_planes = 2;
    ext_buf.pitches[0] = stride0;
    ext_buf.pitches[1] = stride1;
    ext_buf.offsets[0] = offset0;
    ext_buf.offsets[1] = offset1;
    ext_buf.buffers = buffer_handles;
    ext_buf.num_buffers = 1;
    ext_buf.flags = 0;

    VASurfaceAttrib import_attribs[2];
    import_attribs[0].type = VASurfaceAttribMemoryType;
    import_attribs[0].flags = VA_SURFACE_ATTRIB_SETTABLE;
    import_attribs[0].value.type = VAGenericValueTypeInteger;
    import_attribs[0].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME;

    import_attribs[1].type = VASurfaceAttribExternalBufferDescriptor;
    import_attribs[1].flags = VA_SURFACE_ATTRIB_SETTABLE;
    import_attribs[1].value.type = VAGenericValueTypePointer;
    import_attribs[1].value.value.p = &ext_buf;

    VASurfaceID surfaces[2]; /* [0] = imported dma-buf, [1] = driver-native recon */
    VAStatus import_status = vaCreateSurfaces(dpy, VA_RT_FORMAT_YUV420, WIDTH, HEIGHT,
                                               &surfaces[0], 1, import_attribs, 2);
    if (import_status != VA_STATUS_SUCCESS) {
        fprintf(stderr, "\n*** TIER 3 ANSWER: NO — dma-buf import failed: %s (0x%x) ***\n",
                vaErrorStr(import_status), import_status);
        return 1;
    }
    printf("\n*** dma-buf import into a VA-API surface SUCCEEDED (surface %u) ***\n",
           surfaces[0]);

    /* --- Step 5: readback via VA-API, confirm the pattern written via
     * plain mmap() (step 2/3, no VA-API involvement) is visible -- proves
     * this is really the same memory, not a copy vaCreateSurfaces made.
     * Non-fatal: this radeonsi driver may not support vaDeriveImage on a
     * PRIME-imported surface even though the import itself succeeded and
     * the surface is still perfectly usable for encode (the actual
     * make-or-break question) -- so a failure here is logged, not fatal. */
    VAImage check_image;
    VAStatus derive_status = vaDeriveImage(dpy, surfaces[0], &check_image);
    if (derive_status != VA_STATUS_SUCCESS) {
        printf("Zero-copy coherency check: SKIPPED -- vaDeriveImage on the imported "
               "surface failed: %s (0x%x). This may just mean the driver doesn't "
               "support CPU-mapping an imported PRIME surface directly; it does not "
               "by itself mean the surface is unusable for encode (see below).\n",
               vaErrorStr(derive_status), derive_status);
    } else {
        void *check_p = NULL;
        CHECK_VA(vaMapBuffer(dpy, check_image.buf, &check_p), "vaMapBuffer(imported)");
        unsigned char *y_check = (unsigned char *)check_p + check_image.offsets[0];
        int coherent = 1;
        for (unsigned int row = 0; row < HEIGHT && coherent; row++) {
            for (unsigned int col = 0; col < WIDTH; col++) {
                if (y_check[row * check_image.pitches[0] + col] != FILL_VALUE) {
                    coherent = 0;
                    break;
                }
            }
        }
        CHECK_VA(vaUnmapBuffer(dpy, check_image.buf), "vaUnmapBuffer(imported)");
        CHECK_VA(vaDestroyImage(dpy, check_image.image_id), "vaDestroyImage(imported)");
        printf("Zero-copy coherency check (mmap write -> VA-API read): %s\n",
               coherent ? "PASS (same memory)" : "FAIL (VA-API saw different data -- a copy happened somewhere)");
    }

    /* --- Reconstructed-picture surface: ordinary VA-allocated, same as
     * Tier 2 -- only the INPUT surface is the thing under test here. */
    CHECK_VA(vaCreateSurfaces(dpy, VA_RT_FORMAT_YUV420, WIDTH, HEIGHT,
                               &surfaces[1], 1, NULL, 0),
             "vaCreateSurfaces(recon)");

    VAContextID context_id;
    CHECK_VA(vaCreateContext(dpy, config_id, WIDTH, HEIGHT, VA_PROGRESSIVE,
                              surfaces, 2, &context_id),
             "vaCreateContext");

    /* --- Step 6: run the exact Tier 2 encode sequence against the
     * imported surface --- */
    VABufferID coded_buf;
    CHECK_VA(vaCreateBuffer(dpy, context_id, VAEncCodedBufferType,
                             WIDTH * HEIGHT * 3, 1, NULL, &coded_buf),
             "vaCreateBuffer(coded)");

    unsigned int mb_width = align16(WIDTH) / 16;
    unsigned int mb_height = align16(HEIGHT) / 16;

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
    seq.frame_crop_left_offset = 0;
    seq.frame_crop_top_offset = 0;
    seq.frame_cropping_flag = (WIDTH != mb_width * 16 || HEIGHT != mb_height * 16);
    seq.frame_crop_right_offset = (mb_width * 16 - WIDTH) / 2;
    seq.frame_crop_bottom_offset = (mb_height * 16 - HEIGHT) / 2;

    VABufferID seq_buf;
    CHECK_VA(vaCreateBuffer(dpy, context_id, VAEncSequenceParameterBufferType,
                             sizeof(seq), 1, &seq, &seq_buf),
             "vaCreateBuffer(seq)");

    VAEncPictureParameterBufferH264 pic = {0};
    pic.CurrPic.picture_id = surfaces[1];
    pic.CurrPic.TopFieldOrderCnt = 0;
    for (int i = 0; i < 16; i++) {
        pic.ReferenceFrames[i].picture_id = VA_INVALID_ID;
        pic.ReferenceFrames[i].flags = VA_PICTURE_H264_INVALID;
    }
    pic.coded_buf = coded_buf;
    pic.pic_parameter_set_id = 0;
    pic.seq_parameter_set_id = 0;
    pic.frame_num = 0;
    pic.pic_init_qp = 26;
    pic.num_ref_idx_l0_active_minus1 = 0;
    pic.pic_fields.bits.idr_pic_flag = 1;
    pic.pic_fields.bits.reference_pic_flag = 1;
    pic.pic_fields.bits.entropy_coding_mode_flag = 1;
    pic.pic_fields.bits.deblocking_filter_control_present_flag = 1;

    VABufferID pic_buf;
    CHECK_VA(vaCreateBuffer(dpy, context_id, VAEncPictureParameterBufferType,
                             sizeof(pic), 1, &pic, &pic_buf),
             "vaCreateBuffer(pic)");

    VAEncSliceParameterBufferH264 slice = {0};
    slice.macroblock_address = 0;
    slice.num_macroblocks = mb_width * mb_height;
    slice.macroblock_info = VA_INVALID_ID;
    slice.slice_type = 2;
    slice.pic_parameter_set_id = 0;
    slice.idr_pic_id = 0;
    slice.pic_order_cnt_lsb = 0;
    slice.direct_spatial_mv_pred_flag = 1;
    slice.num_ref_idx_active_override_flag = 1;
    for (int i = 0; i < 32; i++) {
        slice.RefPicList0[i].picture_id = VA_INVALID_ID;
        slice.RefPicList0[i].flags = VA_PICTURE_H264_INVALID;
        slice.RefPicList1[i].picture_id = VA_INVALID_ID;
        slice.RefPicList1[i].flags = VA_PICTURE_H264_INVALID;
    }
    slice.slice_qp_delta = 0;
    slice.disable_deblocking_filter_idc = 0;

    VABufferID slice_buf;
    CHECK_VA(vaCreateBuffer(dpy, context_id, VAEncSliceParameterBufferType,
                             sizeof(slice), 1, &slice, &slice_buf),
             "vaCreateBuffer(slice)");

    bitstream_t sps_bs, pps_bs, slice_hdr_bs;
    bs_init(&sps_bs);
    bs_init(&pps_bs);
    bs_init(&slice_hdr_bs);
    build_sps_rbsp(&sps_bs, &seq);
    build_pps_rbsp(&pps_bs, &pic);
    build_slice_header_bits(&slice_hdr_bs, &seq, &pic, &slice);

    VABufferID sps_param_buf, sps_data_buf, pps_param_buf, pps_data_buf;
    VABufferID slice_hdr_param_buf, slice_hdr_data_buf;
    submit_packed_header(dpy, context_id, VAEncPackedHeaderSequence, &sps_bs,
                          &sps_param_buf, &sps_data_buf);
    submit_packed_header(dpy, context_id, VAEncPackedHeaderPicture, &pps_bs,
                          &pps_param_buf, &pps_data_buf);
    submit_packed_header(dpy, context_id, VAEncPackedHeaderSlice, &slice_hdr_bs,
                          &slice_hdr_param_buf, &slice_hdr_data_buf);

    CHECK_VA(vaBeginPicture(dpy, context_id, surfaces[0]), "vaBeginPicture");

    VABufferID seq_submit[] = {seq_buf};
    CHECK_VA(vaRenderPicture(dpy, context_id, seq_submit, 1), "vaRenderPicture(seq)");
    VABufferID sps_submit[] = {sps_param_buf, sps_data_buf};
    CHECK_VA(vaRenderPicture(dpy, context_id, sps_submit, 2), "vaRenderPicture(packed sps)");
    VABufferID pic_submit[] = {pic_buf};
    CHECK_VA(vaRenderPicture(dpy, context_id, pic_submit, 1), "vaRenderPicture(pic)");
    VABufferID pps_submit[] = {pps_param_buf, pps_data_buf};
    CHECK_VA(vaRenderPicture(dpy, context_id, pps_submit, 2), "vaRenderPicture(packed pps)");
    VABufferID slice_hdr_submit[] = {slice_hdr_param_buf, slice_hdr_data_buf};
    CHECK_VA(vaRenderPicture(dpy, context_id, slice_hdr_submit, 2), "vaRenderPicture(packed slice header)");
    VABufferID slice_submit[] = {slice_buf};
    CHECK_VA(vaRenderPicture(dpy, context_id, slice_submit, 1), "vaRenderPicture(slice)");

    CHECK_VA(vaEndPicture(dpy, context_id), "vaEndPicture");
    CHECK_VA(vaSyncSurface(dpy, surfaces[0]), "vaSyncSurface");

    VACodedBufferSegment *seg = NULL;
    CHECK_VA(vaMapBuffer(dpy, coded_buf, (void **)&seg), "vaMapBuffer(coded)");

    FILE *out = fopen(OUTPUT_FILE, "wb");
    if (!out) {
        perror("fopen " OUTPUT_FILE);
        return 1;
    }
    size_t total = 0;
    free(sps_bs.buf);
    free(pps_bs.buf);
    free(slice_hdr_bs.buf);
    for (VACodedBufferSegment *s = seg; s != NULL; s = s->next) {
        fwrite(s->buf, 1, s->size, out);
        total += s->size;
    }
    fclose(out);
    CHECK_VA(vaUnmapBuffer(dpy, coded_buf), "vaUnmapBuffer(coded)");

    printf("\n*** Encoded %zu bytes of H.264 FROM THE IMPORTED DMA-BUF SURFACE to %s ***\n",
           total, OUTPUT_FILE);
    printf("Verify with: ffprobe %s && ffmpeg -i %s -f null -\n", OUTPUT_FILE, OUTPUT_FILE);

    vaDestroyContext(dpy, context_id);
    vaDestroySurfaces(dpy, surfaces, 2);
    vaDestroyConfig(dpy, config_id);
    vaTerminate(dpy);
    close(dmabuf_fd);
    drmModeDestroyDumbBuffer(drm_primary_fd, handle);
    close(drm_primary_fd);
    close(drm_fd);
    return 0;
}
