/*
 * Tier 2 prototype: minimal standalone VA-API H.264 hardware encoder.
 *
 * Not tied to Android/redroid at all — the point is to drive the *encode*
 * side of VA-API end to end on real hardware (vaCreateConfig with
 * VAEntrypointEncSlice/EncSliceLP, surface + context setup, sequence/
 * picture/slice parameter buffers, vaRenderPicture, and reading the coded
 * bitstream back out) before touching anything Android-specific.
 *
 * Deliberately minimal: encodes a single synthetic NV12 frame (a solid
 * color) as one all-intra H.264 I-frame and writes the raw Annex B
 * bitstream to a file. No GOP structure, no rate control tuning, no CLI
 * flags — just prove the pipeline produces valid, decodable H.264 on this
 * GPU. Resolution/output path are compile-time constants on purpose.
 *
 * IMPORTANT finding this prototype exists to nail down: the driver does
 * NOT synthesize SPS/PPS NAL units into the coded buffer on its own — it
 * only emits the slice data. The application has to build the SPS/PPS
 * RBSP itself and submit it via VAEncPackedHeader{Parameter,Data}Buffer,
 * or the output is a bare slice NAL that no decoder can parse (confirmed:
 * an earlier version of this file without packed headers produced 49
 * bytes that ffmpeg rejected as "Invalid data found when processing
 * input"). Hence the bitstream writer below.
 */

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_enc_h264.h>

#define WIDTH 320
#define HEIGHT 240
#define OUTPUT_FILE "out.h264"
#define DRM_DEVICE "/dev/dri/renderD128"

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
 * Minimal MSB-first bitstream writer, enough to hand-build the SPS/PPS
 * RBSPs that this driver expects as packed headers (Exp-Golomb ue(v)/
 * se(v) per H.264 spec 9.1).
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

/* Exp-Golomb unsigned, H.264 9.1: codeNum -> leadingZeroBits '0's, a '1',
 * then leadingZeroBits bits of (codeNum + 1) with the leading 1 dropped. */
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
    bs_put_bit(bs, 1); /* rbsp_stop_one_bit */
    while (bs->bit_pos % 8) bs_put_bit(bs, 0);
}

/* Per va_enc_h264.h: "Each associated packed header data buffer shall
 * contain the start code prefix 0x000001 followed by the complete NAL
 * unit" — the driver does NOT prepend this itself. Omitting it isn't a
 * cosmetic difference: it made the radeonsi driver choke (fortify-detected
 * buffer overflow) rather than just produce a slightly-off bitstream. */
static void bs_start_code_and_nal_header(bitstream_t *bs, int nal_ref_idc, int nal_unit_type) {
    bs_put_bits(bs, 0x000001, 24); /* start code prefix */
    bs_put_bit(bs, 0);             /* forbidden_zero_bit */
    bs_put_bits(bs, nal_ref_idc, 2);
    bs_put_bits(bs, nal_unit_type, 5);
}

/* Builds a full SPS NAL (start code + NAL header + RBSP), per the
 * va_enc_h264.h packed-header contract above. Emulation-prevention bytes
 * are still the driver's job (has_emulation_bytes = 0 at the call site).
 * Fields mirror the VAEncSequenceParameterBufferH264 this same program
 * submits, so the two must be kept in sync by hand. */
static void build_sps_rbsp(bitstream_t *bs, const VAEncSequenceParameterBufferH264 *seq) {
    bs_start_code_and_nal_header(bs, 3, 7); /* nal_ref_idc=3, nal_unit_type=7 (SPS) */

    bs_put_bits(bs, 66, 8);           /* profile_idc: 66 = Baseline */
    bs_put_bit(bs, 1);                /* constraint_set0_flag (baseline) */
    bs_put_bit(bs, 1);                /* constraint_set1_flag (constrained baseline) */
    bs_put_bit(bs, 0);                /* constraint_set2_flag */
    bs_put_bit(bs, 0);                /* constraint_set3_flag */
    bs_put_bits(bs, 0, 4);            /* reserved_zero_4bits */
    bs_put_bits(bs, seq->level_idc, 8);

    bs_put_ue(bs, seq->seq_parameter_set_id);
    bs_put_ue(bs, seq->seq_fields.bits.log2_max_frame_num_minus4);
    bs_put_ue(bs, seq->seq_fields.bits.pic_order_cnt_type);
    /* pic_order_cnt_type == 2: no further POC fields (matches this encoder). */
    bs_put_ue(bs, seq->max_num_ref_frames);
    bs_put_bit(bs, 0);                /* gaps_in_frame_num_value_allowed_flag */
    bs_put_ue(bs, seq->picture_width_in_mbs - 1);
    bs_put_ue(bs, seq->picture_height_in_mbs - 1); /* frame_mbs_only_flag=1: map units == mbs */
    bs_put_bit(bs, seq->seq_fields.bits.frame_mbs_only_flag);
    bs_put_bit(bs, 1);                /* direct_8x8_inference_flag */
    bs_put_bit(bs, seq->frame_cropping_flag);
    if (seq->frame_cropping_flag) {
        bs_put_ue(bs, seq->frame_crop_left_offset);
        bs_put_ue(bs, seq->frame_crop_right_offset);
        bs_put_ue(bs, seq->frame_crop_top_offset);
        bs_put_ue(bs, seq->frame_crop_bottom_offset);
    }
    bs_put_bit(bs, 0); /* vui_parameters_present_flag */

    bs_rbsp_trailing_bits(bs);
}

/* Builds a full PPS RBSP; fields mirror VAEncPictureParameterBufferH264. */
static void build_pps_rbsp(bitstream_t *bs, const VAEncPictureParameterBufferH264 *pic) {
    bs_start_code_and_nal_header(bs, 3, 8); /* nal_ref_idc=3, nal_unit_type=8 (PPS) */

    bs_put_ue(bs, pic->pic_parameter_set_id);
    bs_put_ue(bs, pic->seq_parameter_set_id);
    bs_put_bit(bs, pic->pic_fields.bits.entropy_coding_mode_flag);
    bs_put_bit(bs, 0);  /* bottom_field_pic_order_in_frame_present_flag */
    bs_put_ue(bs, 0);   /* num_slice_groups_minus1 */
    bs_put_ue(bs, pic->num_ref_idx_l0_active_minus1);
    bs_put_ue(bs, 0);   /* num_ref_idx_l1_default_active_minus1 */
    bs_put_bit(bs, 0);  /* weighted_pred_flag */
    bs_put_bits(bs, 0, 2); /* weighted_bipred_idc */
    bs_put_se(bs, pic->pic_init_qp - 26); /* pic_init_qp_minus26 */
    bs_put_se(bs, 0);   /* pic_init_qs_minus26 */
    bs_put_se(bs, 0);   /* chroma_qp_index_offset */
    bs_put_bit(bs, pic->pic_fields.bits.deblocking_filter_control_present_flag);
    bs_put_bit(bs, 0);  /* constrained_intra_pred_flag */
    bs_put_bit(bs, 0);  /* redundant_pic_cnt_present_flag */

    bs_rbsp_trailing_bits(bs);
}

/* Builds a packed slice_header() for a single I/IDR slice. Unlike SPS/PPS,
 * this deliberately does NOT call bs_rbsp_trailing_bits(): per
 * va_enc_h264.h, the packed slice-header buffer holds only slice_header(),
 * and the driver's hardware entropy coder appends slice_data() immediately
 * afterward, continuing from this exact (non-byte-aligned) bit position —
 * for CABAC, slice_data() itself starts with cabac_alignment_one_bit
 * padding per spec 7.3.4, so the driver — not us — is responsible for
 * reaching the next byte boundary. This path only covers the IDR/I-slice
 * case this prototype actually emits (fixed nal_ref_idc=3): the P/B
 * branches of slice_header() (ref_pic_list_modification, pred_weight_table,
 * the non-IDR dec_ref_pic_marking form) are simply not implemented. */
static void build_slice_header_bits(bitstream_t *bs,
                                     const VAEncSequenceParameterBufferH264 *seq,
                                     const VAEncPictureParameterBufferH264 *pic,
                                     const VAEncSliceParameterBufferH264 *slice) {
    bs_start_code_and_nal_header(bs, 3, 5); /* nal_ref_idc=3, nal_unit_type=5 (IDR slice) */

    bs_put_ue(bs, slice->macroblock_address);   /* first_mb_in_slice */
    bs_put_ue(bs, slice->slice_type);
    bs_put_ue(bs, slice->pic_parameter_set_id);
    bs_put_bits(bs, pic->frame_num,
                seq->seq_fields.bits.log2_max_frame_num_minus4 + 4);
    bs_put_ue(bs, slice->idr_pic_id);
    /* pic_order_cnt_type == 2: no pic_order_cnt_lsb syntax.
     * redundant_pic_cnt_present_flag == 0: nothing here.
     * slice_type == I: no ref_idx_active_override, ref_pic_list_modification,
     * or pred_weight_table — all are P/SP/B-only per spec 7.3.3. */
    bs_put_bit(bs, 0); /* no_output_of_prior_pics_flag (dec_ref_pic_marking, IDR form) */
    bs_put_bit(bs, 0); /* long_term_reference_flag */
    /* entropy_coding_mode_flag && slice_type != I/SI: skip cabac_init_idc. */
    bs_put_se(bs, slice->slice_qp_delta);
    if (pic->pic_fields.bits.deblocking_filter_control_present_flag) {
        bs_put_ue(bs, slice->disable_deblocking_filter_idc);
        if (slice->disable_deblocking_filter_idc != 1) {
            bs_put_se(bs, 0); /* slice_alpha_c0_offset_div2 */
            bs_put_se(bs, 0); /* slice_beta_offset_div2 */
        }
    }
}

/* Submits one packed header (parameter buffer + data buffer pair) and
 * frees the bitstream's backing storage. */
static void submit_packed_header(VADisplay dpy, VAContextID context_id,
                                  VAEncPackedHeaderType type, bitstream_t *bs,
                                  VABufferID *out_param_buf, VABufferID *out_data_buf) {
    VAEncPackedHeaderParameterBuffer param = {0};
    param.type = type;
    param.bit_length = (unsigned int)bs->bit_pos;
    param.has_emulation_bytes = 0; /* let the driver insert 0x03 emulation bytes */

    CHECK_VA(vaCreateBuffer(dpy, context_id, VAEncPackedHeaderParameterBufferType,
                             sizeof(param), 1, &param, out_param_buf),
             "vaCreateBuffer(packed header param)");
    CHECK_VA(vaCreateBuffer(dpy, context_id, VAEncPackedHeaderDataBufferType,
                             (bs->bit_pos + 7) / 8, 1, bs->buf, out_data_buf),
             "vaCreateBuffer(packed header data)");
    /* vaCreateBuffer copies bs->buf into the VA buffer; the caller frees
     * its own copy once it's done reading it back for reference/debugging. */
}

/* Fills a derived NV12 image with a flat mid-grey Y plane and neutral
 * chroma — enough to produce a real, decodable frame without needing a
 * test-pattern generator. */
static void fill_nv12(VAImage *image, unsigned char *buf) {
    unsigned char *y = buf + image->offsets[0];
    unsigned char *uv = buf + image->offsets[1];

    for (unsigned int row = 0; row < image->height; row++) {
        memset(y + row * image->pitches[0], 128, image->width);
    }
    for (unsigned int row = 0; row < image->height / 2; row++) {
        memset(uv + row * image->pitches[1], 128, image->width);
    }
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    int drm_fd = open(DRM_DEVICE, O_RDWR);
    if (drm_fd < 0) {
        perror("open " DRM_DEVICE);
        return 1;
    }

    VADisplay dpy = vaGetDisplayDRM(drm_fd);
    if (!dpy) {
        fprintf(stderr, "vaGetDisplayDRM failed — is %s a valid render node?\n",
                DRM_DEVICE);
        return 1;
    }

    int major, minor;
    CHECK_VA(vaInitialize(dpy, &major, &minor), "vaInitialize");
    printf("VA-API version %d.%d, driver: %s\n", major, minor,
           vaQueryVendorString(dpy));

    /* Find an H.264 encode entrypoint. AMD/most desktop GPUs report
     * VAEntrypointEncSlice; Intel's iHD driver reports VAEntrypointEncSliceLP
     * (low-power) instead — confirmed both ways during Tier 0, so this
     * checks for either rather than assuming one. */
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
        fprintf(stderr,
                "No H.264 encode entrypoint (EncSlice/EncSliceLP) found for "
                "VAProfileH264ConstrainedBaseline on this GPU.\n");
        return 1;
    }
    printf("Using entrypoint %s\n",
           entrypoint == VAEntrypointEncSlice ? "VAEntrypointEncSlice"
                                               : "VAEntrypointEncSliceLP");

    /* Check whether this driver can accept SPS/PPS as packed headers at
     * all before spending time building them — some drivers only support
     * VAEncPackedHeaderSlice, or generate SPS/PPS themselves (this one
     * doesn't, confirmed by the very first cut of this program producing
     * an unparseable 49-byte file with no SPS/PPS in it). */
    VAConfigAttrib packed_attr = {.type = VAConfigAttribEncPackedHeaders};
    CHECK_VA(vaGetConfigAttributes(dpy, VAProfileH264ConstrainedBaseline,
                                    entrypoint, &packed_attr, 1),
             "vaGetConfigAttributes(EncPackedHeaders)");
    int supports_packed_seq = (packed_attr.value != VA_ATTRIB_NOT_SUPPORTED) &&
                               (packed_attr.value & VA_ENC_PACKED_HEADER_SEQUENCE);
    int supports_packed_pic = (packed_attr.value != VA_ATTRIB_NOT_SUPPORTED) &&
                               (packed_attr.value & VA_ENC_PACKED_HEADER_PICTURE);
    printf("Packed header support: sequence=%d picture=%d (raw attr=0x%x)\n",
           supports_packed_seq, supports_packed_pic, packed_attr.value);
    if (!supports_packed_seq || !supports_packed_pic) {
        fprintf(stderr,
                "This driver doesn't support packed SPS/PPS headers — this "
                "prototype doesn't have a fallback path for that case.\n");
        return 1;
    }

    VAConfigAttrib attrib = {.type = VAConfigAttribRTFormat,
                              .value = VA_RT_FORMAT_YUV420};
    VAConfigID config_id;
    CHECK_VA(vaCreateConfig(dpy, VAProfileH264ConstrainedBaseline, entrypoint,
                             &attrib, 1, &config_id),
             "vaCreateConfig");

    /* Two surfaces: index 0 is the raw input we fill ourselves, index 1 is
     * the reconstructed-picture surface the driver needs even for a single
     * I-frame (it's where the decoded/reconstructed picture used for future
     * reference would live in a real GOP — required by the API regardless).
     */
    VASurfaceID surfaces[2];
    CHECK_VA(vaCreateSurfaces(dpy, VA_RT_FORMAT_YUV420, WIDTH, HEIGHT,
                               surfaces, 2, NULL, 0),
             "vaCreateSurfaces");

    VAContextID context_id;
    CHECK_VA(vaCreateContext(dpy, config_id, WIDTH, HEIGHT, VA_PROGRESSIVE,
                              surfaces, 2, &context_id),
             "vaCreateContext");

    /* --- Fill the input surface with a synthetic NV12 frame --- */
    VAImage image;
    CHECK_VA(vaDeriveImage(dpy, surfaces[0], &image), "vaDeriveImage");
    void *surface_p = NULL;
    CHECK_VA(vaMapBuffer(dpy, image.buf, &surface_p), "vaMapBuffer(input)");
    fill_nv12(&image, (unsigned char *)surface_p);
    CHECK_VA(vaUnmapBuffer(dpy, image.buf), "vaUnmapBuffer(input)");
    CHECK_VA(vaDestroyImage(dpy, image.image_id), "vaDestroyImage");

    /* --- Coded (output) buffer --- */
    VABufferID coded_buf;
    CHECK_VA(vaCreateBuffer(dpy, context_id, VAEncCodedBufferType,
                             WIDTH * HEIGHT * 3, 1, NULL, &coded_buf),
             "vaCreateBuffer(coded)");

    unsigned int mb_width = align16(WIDTH) / 16;
    unsigned int mb_height = align16(HEIGHT) / 16;

    /* --- Sequence parameters (SPS) --- */
    VAEncSequenceParameterBufferH264 seq = {0};
    seq.seq_parameter_set_id = 0;
    seq.level_idc = 30; /* Level 3.0, plenty for 320x240 */
    seq.intra_period = 1;     /* every frame is an I-frame (we only send one) */
    seq.intra_idr_period = 1;
    seq.ip_period = 0;
    seq.bits_per_second = 2000000;
    seq.max_num_ref_frames = 1;
    seq.picture_width_in_mbs = mb_width;
    seq.picture_height_in_mbs = mb_height;
    seq.seq_fields.bits.frame_mbs_only_flag = 1;
    seq.seq_fields.bits.chroma_format_idc = 1; /* 4:2:0 */
    seq.seq_fields.bits.log2_max_frame_num_minus4 = 0;
    seq.seq_fields.bits.pic_order_cnt_type = 2; /* simplest: derive POC from frame_num */
    seq.frame_crop_left_offset = 0;
    seq.frame_crop_top_offset = 0;
    /* Crop off the 16-pixel macroblock alignment padding, if any. */
    seq.frame_cropping_flag = (WIDTH != mb_width * 16 || HEIGHT != mb_height * 16);
    seq.frame_crop_right_offset = (mb_width * 16 - WIDTH) / 2;
    seq.frame_crop_bottom_offset = (mb_height * 16 - HEIGHT) / 2;

    VABufferID seq_buf;
    CHECK_VA(vaCreateBuffer(dpy, context_id, VAEncSequenceParameterBufferType,
                             sizeof(seq), 1, &seq, &seq_buf),
             "vaCreateBuffer(seq)");

    /* --- Picture parameters (PPS + frame-level info) --- */
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
    pic.pic_fields.bits.entropy_coding_mode_flag = 1; /* CABAC */
    pic.pic_fields.bits.deblocking_filter_control_present_flag = 1;

    VABufferID pic_buf;
    CHECK_VA(vaCreateBuffer(dpy, context_id, VAEncPictureParameterBufferType,
                             sizeof(pic), 1, &pic, &pic_buf),
             "vaCreateBuffer(pic)");

    /* --- Slice parameters (one slice covering the whole frame) --- */
    VAEncSliceParameterBufferH264 slice = {0};
    slice.macroblock_address = 0;
    slice.num_macroblocks = mb_width * mb_height;
    slice.macroblock_info = VA_INVALID_ID;
    slice.slice_type = 2; /* I slice */
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

    /* --- Build and stage the packed SPS/PPS/slice headers --- */
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

    /* --- Submit and encode ---
     * One vaRenderPicture call per buffer "family" (sequence, packed-SPS,
     * picture, packed-PPS, slice) rather than one batched call with all
     * seven buffers — matches the pattern libva-utils' h264encode.c uses.
     * Batching everything into a single vaRenderPicture call crashed this
     * radeonsi driver with a fortify-detected buffer overflow, so treat
     * that combination as unsupported/buggy here rather than fighting it. */
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

    /* --- Read the coded bitstream back out --- */
    VACodedBufferSegment *seg = NULL;
    CHECK_VA(vaMapBuffer(dpy, coded_buf, (void **)&seg), "vaMapBuffer(coded)");

    FILE *out = fopen(OUTPUT_FILE, "wb");
    if (!out) {
        perror("fopen " OUTPUT_FILE);
        return 1;
    }
    size_t total = 0;
    /* Once packed SPS + PPS + slice header are all submitted, this driver
     * emits three separate coded-buffer segments itself: SPS NAL, PPS NAL,
     * then a properly-headered slice NAL (0x65, IDR) — it apparently only
     * synthesizes the parameter-set NALs once a packed slice header is
     * also present (confirmed: with packed SPS/PPS alone but no packed
     * slice header, the coded buffer was a single segment containing only
     * a corrupt slice NAL with an invalid 0x00 header byte). So the coded
     * buffer alone is now a complete Annex-B stream — no manual prepending
     * of our own sps_bs/pps_bs bytes needed (that duplicated them). */
    free(sps_bs.buf);
    free(pps_bs.buf);
    free(slice_hdr_bs.buf);
    for (VACodedBufferSegment *s = seg; s != NULL; s = s->next) {
        fwrite(s->buf, 1, s->size, out);
        total += s->size;
    }
    fclose(out);
    CHECK_VA(vaUnmapBuffer(dpy, coded_buf), "vaUnmapBuffer(coded)");

    printf("Wrote %zu bytes of H.264 to %s\n", total, OUTPUT_FILE);

    vaDestroyContext(dpy, context_id);
    vaDestroySurfaces(dpy, surfaces, 2);
    vaDestroyConfig(dpy, config_id);
    vaTerminate(dpy);
    close(drm_fd);
    return 0;
}
