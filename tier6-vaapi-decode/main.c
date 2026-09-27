/*
 * Tier 6 prototype: minimal standalone VA-API H.264 hardware decoder.
 *
 * Mirror image of tier2-vaapi-encode/main.c: not tied to Android/redroid at
 * all, the point is to drive the *decode* side of VA-API end to end on real
 * hardware (vaCreateConfig with VAEntrypointVLD, surface + context setup,
 * picture/slice parameter buffers, vaRenderPicture, reading the decoded
 * NV12 surface back out) before touching anything Android-specific.
 *
 * Deliberately minimal, matching Tier 2's own scope: decodes exactly one
 * frame - the first NAL unit's IDR slice - from a real Annex-B H.264
 * elementary stream (baseline profile: no B-frames, CAVLC, one slice per
 * picture, no interlacing, no cropping). No DPB/reference-frame management,
 * no multi-frame loop - proving single-frame VLD decode on this GPU is the
 * whole point here, the same way Tier 2 proved single-frame encode.
 *
 * Unlike encode, where the driver needed the SPS/PPS handed to it as
 * packed headers because it never parses a bitstream itself, decode is
 * the opposite: nothing hands VA-API a parsed SPS/PPS to use, the
 * application has to parse the *incoming* SPS/PPS/slice-header out of a
 * real bitstream and hand VA-API their already-decoded field values via
 * VAPictureParameterBufferH264/VASliceParameterBufferH264 - the slice
 * *data* buffer, unlike encode's coded-buffer readback, is handed to the
 * driver still Annex-B-escaped (emulation prevention bytes included);
 * only the fields consumed by this program's own header parser need
 * de-escaping.
 */

#include <dlfcn.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>
#include <va/va_vpp.h>

#define INPUT_FILE "/tmp/test.h264"
#define OUTPUT_FILE "/tmp/decoded_nv12.yuv"
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
 * NAL unit extraction from an Annex-B stream (start-code scanning) and
 * RBSP de-escaping (removing emulation-prevention 0x03 bytes) - the
 * de-escaped copy is only for THIS program's own header-field parsing;
 * the driver gets the original, still-escaped bytes in the slice data
 * buffer (matching how a real hardware CABAC/CAVLC engine expects it).
 * ------------------------------------------------------------------ */
typedef struct {
    const uint8_t *data; /* still-escaped, as it appears in the stream */
    size_t size;
    int nal_ref_idc;
    int nal_unit_type;
} NalUnit;

static int find_next_start_code(const uint8_t *buf, size_t size, size_t from) {
    for (size_t i = from; i + 2 < size; i++) {
        if (buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 1)
            return (int)i;
    }
    return -1;
}

/* Splits the whole file buffer into NAL units (payload starts right after
 * the 1-byte NAL header, start code and header both excluded from data). */
static int extract_nals(const uint8_t *buf, size_t size, NalUnit *out, int max_nals) {
    int count = 0;
    int sc = find_next_start_code(buf, size, 0);
    while (sc >= 0 && count < max_nals) {
        size_t nal_start = sc + 3;
        int next_sc = find_next_start_code(buf, size, nal_start);
        size_t nal_end = (next_sc >= 0) ? (size_t)next_sc : size;
        /* Trim a trailing zero byte some encoders leave before the next
         * start code (0x000001 vs 0x00000001 four-byte variant). */
        while (nal_end > nal_start && buf[nal_end - 1] == 0)
            nal_end--;
        if (nal_end > nal_start) {
            out[count].nal_ref_idc = (buf[nal_start] >> 5) & 0x3;
            out[count].nal_unit_type = buf[nal_start] & 0x1f;
            out[count].data = buf + nal_start + 1;
            out[count].size = nal_end - nal_start - 1;
            count++;
        }
        sc = next_sc;
    }
    return count;
}

/* De-escapes a NAL payload (removes every 0x03 that follows 0x00 0x00 and
 * precedes 0x00/0x01/0x02/0x03) into a freshly malloc'd buffer the caller
 * owns - this is the RBSP the H.264 spec's own bit-level syntax is defined
 * against, distinct from the escaped bytes the driver wants verbatim. */
static uint8_t *deescape_rbsp(const uint8_t *data, size_t size, size_t *out_size) {
    uint8_t *out = malloc(size);
    size_t o = 0;
    int zero_run = 0;
    for (size_t i = 0; i < size; i++) {
        if (zero_run >= 2 && data[i] == 0x03) {
            zero_run = 0;
            continue; /* drop the emulation-prevention byte */
        }
        out[o++] = data[i];
        zero_run = (data[i] == 0) ? zero_run + 1 : 0;
    }
    *out_size = o;
    return out;
}

/* ------------------------------------------------------------------ *
 * Minimal MSB-first bitreader with Exp-Golomb ue(v)/se(v), the mirror
 * image of tier2-vaapi-encode's own bit *writer*.
 * ------------------------------------------------------------------ */
typedef struct {
    const uint8_t *buf;
    size_t size;
    size_t bit_pos;
} BitReader;

static unsigned int br_bit(BitReader *br) {
    size_t byte = br->bit_pos / 8;
    int shift = 7 - (int)(br->bit_pos % 8);
    unsigned int bit = (byte < br->size) ? ((br->buf[byte] >> shift) & 1) : 0;
    br->bit_pos++;
    return bit;
}

static unsigned int br_bits(BitReader *br, int n) {
    unsigned int v = 0;
    for (int i = 0; i < n; i++)
        v = (v << 1) | br_bit(br);
    return v;
}

/* ue(v): Exp-Golomb unsigned - count leading zero bits, then read that
 * many more bits, value = 2^leadingZeros - 1 + suffix. */
static unsigned int br_ue(BitReader *br) {
    int leading_zeros = 0;
    while (br_bit(br) == 0 && leading_zeros < 32)
        leading_zeros++;
    if (leading_zeros == 0)
        return 0;
    unsigned int suffix = br_bits(br, leading_zeros);
    return (1u << leading_zeros) - 1 + suffix;
}

/* se(v): Exp-Golomb signed, per spec 9.1.1 - derived directly from ue(v). */
static int br_se(BitReader *br) {
    unsigned int code = br_ue(br);
    int sign = (code & 1) ? 1 : -1;
    return sign * (int)((code + 1) / 2);
}

/* ------------------------------------------------------------------ *
 * SPS/PPS/slice-header parsing - only the fields this baseline-profile,
 * single-slice, no-cropping, no-interlacing spike actually needs to fill
 * VAPictureParameterBufferH264/VASliceParameterBufferH264. A real decoder
 * needs the rest (VUI, scaling lists, cropping, field pictures, ...); this
 * one deliberately doesn't, matching Tier 2's own "prove the mechanism,
 * not full spec coverage" scope.
 * ------------------------------------------------------------------ */
typedef struct {
    int profile_idc;
    int seq_parameter_set_id;
    int log2_max_frame_num_minus4;
    int pic_order_cnt_type;
    int log2_max_pic_order_cnt_lsb_minus4; /* only valid if type==0 */
    int max_num_ref_frames;
    int pic_width_in_mbs_minus1;
    int pic_height_in_map_units_minus1;
    int frame_mbs_only_flag;
} SpsInfo;

typedef struct {
    int pic_parameter_set_id;
    int seq_parameter_set_id;
    int entropy_coding_mode_flag;
    int pic_order_present_flag;
    int num_ref_idx_l0_active_minus1;
    int num_ref_idx_l1_active_minus1;
    int weighted_pred_flag;
    int weighted_bipred_idc;
    int pic_init_qp_minus26;
    int chroma_qp_index_offset;
    int deblocking_filter_control_present_flag;
    int redundant_pic_cnt_present_flag;
} PpsInfo;

static void parse_sps(const uint8_t *rbsp, size_t size, SpsInfo *sps) {
    BitReader br = { rbsp, size, 0 };
    sps->profile_idc = br_bits(&br, 8);
    br_bits(&br, 8); /* constraint flags + reserved */
    br_bits(&br, 8); /* level_idc */
    sps->seq_parameter_set_id = br_ue(&br);
    /* High/etc profiles have a chroma_format_idc block here - baseline
     * doesn't, and that's the only profile this spike's test stream uses. */
    sps->log2_max_frame_num_minus4 = br_ue(&br);
    sps->pic_order_cnt_type = br_ue(&br);
    if (sps->pic_order_cnt_type == 0) {
        sps->log2_max_pic_order_cnt_lsb_minus4 = br_ue(&br);
    } else if (sps->pic_order_cnt_type == 1) {
        fprintf(stderr, "pic_order_cnt_type 1 not handled by this spike\n");
        exit(1);
    }
    sps->max_num_ref_frames = br_ue(&br);
    br_bit(&br); /* gaps_in_frame_num_value_allowed_flag */
    sps->pic_width_in_mbs_minus1 = br_ue(&br);
    sps->pic_height_in_map_units_minus1 = br_ue(&br);
    sps->frame_mbs_only_flag = br_bit(&br);
    if (!sps->frame_mbs_only_flag) {
        fprintf(stderr, "interlaced SPS not handled by this spike\n");
        exit(1);
    }
    /* direct_8x8_inference_flag, frame_cropping_flag(+offsets), VUI: not
     * needed for this spike (no B-frames, generated with no cropping). */
}

static void parse_pps(const uint8_t *rbsp, size_t size, PpsInfo *pps) {
    BitReader br = { rbsp, size, 0 };
    pps->pic_parameter_set_id = br_ue(&br);
    pps->seq_parameter_set_id = br_ue(&br);
    pps->entropy_coding_mode_flag = br_bit(&br);
    pps->pic_order_present_flag = br_bit(&br);
    unsigned int num_slice_groups_minus1 = br_ue(&br);
    if (num_slice_groups_minus1 != 0) {
        fprintf(stderr, "slice groups not handled by this spike\n");
        exit(1);
    }
    pps->num_ref_idx_l0_active_minus1 = br_ue(&br);
    pps->num_ref_idx_l1_active_minus1 = br_ue(&br);
    pps->weighted_pred_flag = br_bit(&br);
    pps->weighted_bipred_idc = br_bits(&br, 2);
    pps->pic_init_qp_minus26 = br_se(&br);
    br_se(&br); /* pic_init_qs_minus26 */
    pps->chroma_qp_index_offset = br_se(&br);
    pps->deblocking_filter_control_present_flag = br_bit(&br);
    br_bit(&br); /* constrained_intra_pred_flag */
    pps->redundant_pic_cnt_present_flag = br_bit(&br);
}

typedef struct {
    unsigned int first_mb_in_slice;
    unsigned int slice_type;
    unsigned int pic_parameter_set_id;
    unsigned int frame_num;
    unsigned int idr_pic_id;         /* only if IDR */
    unsigned int pic_order_cnt_lsb;  /* only if pic_order_cnt_type==0 */
    int slice_qp_delta;
    unsigned int header_bits;        /* bits consumed, in the DE-ESCAPED rbsp -
                                       * VA-API wants this position translated
                                       * back into the original, still-escaped
                                       * bitstream (see main()'s own comment). */
} SliceHeader;

/* Full I-slice header for the baseline/CAVLC/single-slice-group/no-
 * redundant-pics case this spike's test stream uses - reads all the way
 * through to the first macroblock's own data, not just the fields this
 * program itself needs the values of. VA-API's slice_data_bit_offset
 * needs the real end-of-header bit position; stopping early (this
 * function's own earlier version) left the hardware decoder starting
 * entropy decoding from inside the slice header itself, producing
 * exactly the kind of clean-looking-but-fake flat/uniform output an error
 * concealment path falls back to on a badly desynced bitstream. */
static void parse_slice_header(const uint8_t *rbsp, size_t size, int nal_unit_type,
                                int nal_ref_idc, const SpsInfo *sps, const PpsInfo *pps,
                                SliceHeader *sh) {
    BitReader br = { rbsp, size, 0 };
    sh->first_mb_in_slice = br_ue(&br);
    sh->slice_type = br_ue(&br);
    sh->pic_parameter_set_id = br_ue(&br);
    sh->frame_num = br_bits(&br, sps->log2_max_frame_num_minus4 + 4);
    /* frame_mbs_only_flag==1 => no field_pic_flag/bottom_field_flag bits */
    int is_idr = (nal_unit_type == 5);
    if (is_idr)
        sh->idr_pic_id = br_ue(&br);
    if (sps->pic_order_cnt_type == 0)
        sh->pic_order_cnt_lsb = br_bits(&br, sps->log2_max_pic_order_cnt_lsb_minus4 + 4);
    /* ref_pic_list_modification(): only present for P/SP/B slices (7.3.3.1) -
     * our test stream's I-slices (slice_type % 5 == 2) never reach this. */
    unsigned int base_type = sh->slice_type % 5;
    if (base_type != 2 && base_type != 4) {
        fprintf(stderr, "non-I slice_type %u not handled by this spike's header parser\n",
                sh->slice_type);
        exit(1);
    }
    /* dec_ref_pic_marking(): present whenever nal_ref_idc != 0 (7.3.3.3) -
     * for an IDR picture specifically, exactly two flag bits, no loop. */
    if (nal_ref_idc != 0) {
        if (is_idr) {
            br_bit(&br); /* no_output_of_prior_pics_flag */
            br_bit(&br); /* long_term_reference_flag */
        } else {
            fprintf(stderr, "non-IDR reference dec_ref_pic_marking not handled by this spike\n");
            exit(1);
        }
    }
    /* cabac_init_idc: CABAC-only (entropy_coding_mode_flag==1) - our test
     * stream is baseline profile, CAVLC only, never reaches this. */
    if (pps->entropy_coding_mode_flag) {
        fprintf(stderr, "CABAC slices not handled by this spike\n");
        exit(1);
    }
    sh->slice_qp_delta = br_se(&br);
    if (pps->deblocking_filter_control_present_flag) {
        unsigned int disable_idc = br_ue(&br);
        if (disable_idc != 1) {
            br_se(&br); /* slice_alpha_c0_offset_div2 */
            br_se(&br); /* slice_beta_offset_div2 */
        }
    }
    /* num_slice_groups_minus1 == 0 (confirmed in parse_pps) => no
     * slice_group_change_cycle field. Header is now fully consumed - the
     * first macroblock's own data starts at the very next bit. */
    sh->header_bits = (unsigned int)br.bit_pos;
}

/* Walks the ORIGINAL (still-escaped) NAL bytes, replicating deescape_rbsp's
 * own emulation-prevention-byte removal logic, until it has accounted for
 * `deescaped_bits` bits' worth of de-escaped content - returns the
 * corresponding bit position in the ORIGINAL bytes, which is what VA-API's
 * slice_data_bit_offset actually wants (see VASliceParameterBufferH264's
 * own field comment: the header is parsed from de-escaped RBSP, but the
 * slice data buffer handed to hardware is the original, escaped bitstream). */
static unsigned int escaped_bit_offset(const uint8_t *escaped, size_t escaped_size,
                                       unsigned int deescaped_bits) {
    unsigned int deescaped_bits_seen = 0;
    int zero_run = 0;
    for (size_t i = 0; i < escaped_size && deescaped_bits_seen < deescaped_bits; i++) {
        if (zero_run >= 2 && escaped[i] == 0x03) {
            zero_run = 0;
            continue; /* this whole byte is skipped in the de-escaped count */
        }
        unsigned int bits_left = deescaped_bits - deescaped_bits_seen;
        if (bits_left >= 8) {
            deescaped_bits_seen += 8;
        } else {
            deescaped_bits_seen = deescaped_bits; /* partial byte - header ends mid-byte */
            return (unsigned int)(i * 8 + bits_left);
        }
        zero_run = (escaped[i] == 0) ? zero_run + 1 : 0;
        if (deescaped_bits_seen >= deescaped_bits)
            return (unsigned int)((i + 1) * 8);
    }
    return (unsigned int)(escaped_size * 8);
}

/* ------------------------------------------------------------------ */

static uint8_t *read_file(const char *path, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror("fopen input"); exit(1); }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(size);
    if (fread(buf, 1, size, f) != (size_t)size) { perror("fread"); exit(1); }
    fclose(f);
    *out_size = (size_t)size;
    return buf;
}

/* ------------------------------------------------------------------ *
 * Vulkan-based de-tiling: import a tiled dma-buf plane (via its real,
 * driver-assigned DRM format modifier) and vkCmdCopyImage it into a
 * genuinely VK_IMAGE_TILING_LINEAR destination this program can just
 * memcpy out of - the same technique redroid-nvidia's own NVENC work
 * used successfully for the mirror-image problem (there: a linear
 * *source* wrongly imported as tiled; here: a tiled surface this VA-API
 * driver won't hand back as linear no matter what's asked of it,
 * confirmed above). NVIDIA's real Vulkan driver (unlike the community
 * VA-API shim) is known-good at honoring explicit modifiers/layouts -
 * this sidesteps needing to hand-decode the proprietary block-linear
 * scheme at all.
 * ------------------------------------------------------------------ */
typedef struct {
    VkInstance instance;
    VkPhysicalDevice phys;
    VkDevice dev;
    VkQueue queue;
    VkCommandPool pool;
    VkCommandBuffer cmd;
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr GetDeviceProcAddr;
    PFN_vkCreateImage CreateImage;
    PFN_vkDestroyImage DestroyImage;
    PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements;
    PFN_vkGetImageSubresourceLayout GetImageSubresourceLayout;
    PFN_vkAllocateMemory AllocateMemory;
    PFN_vkFreeMemory FreeMemory;
    PFN_vkBindImageMemory BindImageMemory;
    PFN_vkMapMemory MapMemory;
    PFN_vkUnmapMemory UnmapMemory;
    PFN_vkCreateCommandPool CreateCommandPool;
    PFN_vkAllocateCommandBuffers AllocateCommandBuffers;
    PFN_vkBeginCommandBuffer BeginCommandBuffer;
    PFN_vkEndCommandBuffer EndCommandBuffer;
    PFN_vkCmdPipelineBarrier CmdPipelineBarrier;
    PFN_vkCmdCopyImage CmdCopyImage;
    PFN_vkQueueSubmit QueueSubmit;
    PFN_vkQueueWaitIdle QueueWaitIdle;
    PFN_vkGetDeviceQueue GetDeviceQueue;
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties;
    VkPhysicalDeviceMemoryProperties mem_props;
} VkDetiler;

static VkDetiler g_vk;

static void vk_check(VkResult r, const char *msg) {
    if (r != VK_SUCCESS) { fprintf(stderr, "%s failed: %d\n", msg, r); exit(1); }
}

static void vk_init(void) {
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!lib) { fprintf(stderr, "dlopen libvulkan.so.1 failed\n"); exit(1); }
    g_vk.GetInstanceProcAddr = dlsym(lib, "vkGetInstanceProcAddr");
#define GET_GLOBAL(name) PFN_vk##name name = (PFN_vk##name)g_vk.GetInstanceProcAddr(NULL, "vk" #name)
#define GET_INST(name) PFN_vk##name name = (PFN_vk##name)g_vk.GetInstanceProcAddr(g_vk.instance, "vk" #name)
    GET_GLOBAL(CreateInstance);
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                              .pApplicationName = "tier6-detile", .apiVersion = VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ii = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
    vk_check(CreateInstance(&ii, NULL, &g_vk.instance), "vkCreateInstance");

    GET_INST(EnumeratePhysicalDevices);
    GET_INST(GetPhysicalDeviceProperties);
    GET_INST(CreateDevice);
    g_vk.GetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)g_vk.GetInstanceProcAddr(g_vk.instance, "vkGetDeviceProcAddr");
    g_vk.GetPhysicalDeviceMemoryProperties =
        (PFN_vkGetPhysicalDeviceMemoryProperties)g_vk.GetInstanceProcAddr(
            g_vk.instance, "vkGetPhysicalDeviceMemoryProperties");

    VkPhysicalDevice devs[8];
    uint32_t count = 8;
    EnumeratePhysicalDevices(g_vk.instance, &count, devs);
    VkPhysicalDevice best = VK_NULL_HANDLE;
    for (uint32_t i = 0; i < count; i++) {
        VkPhysicalDeviceProperties p;
        GetPhysicalDeviceProperties(devs[i], &p);
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) { best = devs[i]; break; }
        if (best == VK_NULL_HANDLE) best = devs[i];
    }
    g_vk.phys = best;
    g_vk.GetPhysicalDeviceMemoryProperties(g_vk.phys, &g_vk.mem_props);

    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qi = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                   .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &prio };
    const char *exts[] = { "VK_KHR_external_memory_fd", "VK_EXT_external_memory_dma_buf",
                           "VK_EXT_image_drm_format_modifier" };
    VkDeviceCreateInfo di = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                              .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi,
                              .enabledExtensionCount = 3, .ppEnabledExtensionNames = exts };
    vk_check(CreateDevice(g_vk.phys, &di, NULL, &g_vk.dev), "vkCreateDevice");

#define GET_DEV(name) g_vk.name = (PFN_vk##name)g_vk.GetDeviceProcAddr(g_vk.dev, "vk" #name)
    GET_DEV(CreateImage);
    GET_DEV(DestroyImage);
    GET_DEV(GetImageMemoryRequirements);
    GET_DEV(GetImageSubresourceLayout);
    GET_DEV(AllocateMemory);
    GET_DEV(FreeMemory);
    GET_DEV(BindImageMemory);
    GET_DEV(MapMemory);
    GET_DEV(UnmapMemory);
    GET_DEV(CreateCommandPool);
    GET_DEV(AllocateCommandBuffers);
    GET_DEV(BeginCommandBuffer);
    GET_DEV(EndCommandBuffer);
    GET_DEV(CmdPipelineBarrier);
    GET_DEV(CmdCopyImage);
    GET_DEV(QueueSubmit);
    GET_DEV(QueueWaitIdle);
    GET_DEV(GetDeviceQueue);
#undef GET_DEV

    g_vk.GetDeviceQueue(g_vk.dev, 0, 0, &g_vk.queue);
    VkCommandPoolCreateInfo pi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                   .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT };
    vk_check(g_vk.CreateCommandPool(g_vk.dev, &pi, NULL, &g_vk.pool), "vkCreateCommandPool");
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                        .commandPool = g_vk.pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                        .commandBufferCount = 1 };
    vk_check(g_vk.AllocateCommandBuffers(g_vk.dev, &cai, &g_vk.cmd), "vkAllocateCommandBuffers");
}

static uint32_t vk_find_mem_type(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < g_vk.mem_props.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (g_vk.mem_props.memoryTypes[i].propertyFlags & want) == want)
            return i;
    return UINT32_MAX;
}

/* Imports dma-buf `fd` (format `fmt`, driver-assigned `modifier`, `width`x
 * `height`) and copies it into a freshly-malloc'd, tightly-packed linear
 * buffer the caller owns (`*out_data`, `*out_size` = width*height*bpp). */
static void vk_detile(int fd, VkFormat fmt, uint32_t width, uint32_t height, uint64_t modifier,
                      uint64_t real_object_size, uint8_t **out_data, size_t *out_size) {
    /* Probe the modifier's real layout, the same way redroid-nvidia's own
     * discover_modifier_locked() does - don't trust any externally
     * reported pitch for a non-linear modifier. */
    VkImageDrmFormatModifierListCreateInfoEXT probe_mod_list = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT,
        .drmFormatModifierCount = 1, .pDrmFormatModifiers = &modifier,
    };
    VkImageCreateInfo probe_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext = &probe_mod_list,
        .imageType = VK_IMAGE_TYPE_2D, .format = fmt, .extent = { width, height, 1 },
        .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkImage probe;
    vk_check(g_vk.CreateImage(g_vk.dev, &probe_info, NULL, &probe), "vkCreateImage(probe)");
    VkImageSubresource sub = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT };
    VkSubresourceLayout real_layout;
    g_vk.GetImageSubresourceLayout(g_vk.dev, probe, &sub, &real_layout);
    g_vk.DestroyImage(g_vk.dev, probe, NULL);
    fprintf(stderr, "    probed real layout: offset=%llu rowPitch=%llu size=%llu\n",
            (unsigned long long)real_layout.offset, (unsigned long long)real_layout.rowPitch,
            (unsigned long long)real_layout.size);

    /* Import the real dma-buf with that probed layout. */
    VkExternalMemoryImageCreateInfo ext_info = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    VkImageDrmFormatModifierExplicitCreateInfoEXT explicit_mod = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
        .pNext = &ext_info, .drmFormatModifier = modifier,
        .drmFormatModifierPlaneCount = 1, .pPlaneLayouts = &real_layout,
    };
    VkImageCreateInfo import_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext = &explicit_mod,
        .imageType = VK_IMAGE_TYPE_2D, .format = fmt, .extent = { width, height, 1 },
        .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkImage imported;
    vk_check(g_vk.CreateImage(g_vk.dev, &import_info, NULL, &imported), "vkCreateImage(import)");
    VkMemoryRequirements imp_reqs;
    g_vk.GetImageMemoryRequirements(g_vk.dev, imported, &imp_reqs);
    int fd_dup = dup(fd); /* Vulkan takes ownership on success */
    VkImportMemoryFdInfoKHR import_fd = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, .fd = fd_dup,
    };
    VkMemoryDedicatedAllocateInfo imp_ded = { .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
                                              .pNext = &import_fd, .image = imported };
    /* Use the dma-buf's own real reported size, not this probe image's own
     * computed requirement - they don't have to agree (the real object was
     * allocated by the decoder's own internal logic, not by an image
     * created fresh with this program's own USAGE flags), and dma-buf
     * import needs the allocation to match the real underlying object. */
    VkMemoryAllocateInfo imp_alloc = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                       .pNext = &imp_ded, .allocationSize = real_object_size,
                                       .memoryTypeIndex = vk_find_mem_type(imp_reqs.memoryTypeBits, 0) };
    VkDeviceMemory imported_mem;
    vk_check(g_vk.AllocateMemory(g_vk.dev, &imp_alloc, NULL, &imported_mem), "vkAllocateMemory(import)");
    vk_check(g_vk.BindImageMemory(g_vk.dev, imported, imported_mem, 0), "vkBindImageMemory(import)");

    /* Genuinely linear, host-visible destination. */
    VkImageCreateInfo lin_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = fmt,
        .extent = { width, height, 1 }, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_LINEAR,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED,
    };
    VkImage linear_img;
    vk_check(g_vk.CreateImage(g_vk.dev, &lin_info, NULL, &linear_img), "vkCreateImage(linear)");
    VkMemoryRequirements lin_reqs;
    g_vk.GetImageMemoryRequirements(g_vk.dev, linear_img, &lin_reqs);
    VkMemoryAllocateInfo lin_alloc = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = lin_reqs.size,
        .memoryTypeIndex = vk_find_mem_type(lin_reqs.memoryTypeBits,
                                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
    };
    VkDeviceMemory linear_mem;
    vk_check(g_vk.AllocateMemory(g_vk.dev, &lin_alloc, NULL, &linear_mem), "vkAllocateMemory(linear)");
    vk_check(g_vk.BindImageMemory(g_vk.dev, linear_img, linear_mem, 0), "vkBindImageMemory(linear)");

    VkImageSubresource lin_sub = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT };
    VkSubresourceLayout lin_layout;
    g_vk.GetImageSubresourceLayout(g_vk.dev, linear_img, &lin_sub, &lin_layout);

    g_vk.BeginCommandBuffer(g_vk.cmd, &(VkCommandBufferBeginInfo){
                                          .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO });
    VkImageMemoryBarrier to_src = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = imported,
                                    .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
                                    .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT };
    VkImageMemoryBarrier to_dst = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                    .oldLayout = VK_IMAGE_LAYOUT_PREINITIALIZED,
                                    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = linear_img,
                                    .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
                                    .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT };
    VkImageMemoryBarrier both[2] = { to_src, to_dst };
    g_vk.CmdPipelineBarrier(g_vk.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                            0, 0, NULL, 0, NULL, 2, both);
    VkImageCopy region = { .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                          .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                          .extent = { width, height, 1 } };
    g_vk.CmdCopyImage(g_vk.cmd, imported, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, linear_img,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    g_vk.EndCommandBuffer(g_vk.cmd);
    VkSubmitInfo submit = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1,
                            .pCommandBuffers = &g_vk.cmd };
    vk_check(g_vk.QueueSubmit(g_vk.queue, 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit");
    g_vk.QueueWaitIdle(g_vk.queue);

    uint32_t bpp = (fmt == VK_FORMAT_R8_UNORM) ? 1 : 2;
    *out_size = (size_t)width * height * bpp;
    *out_data = malloc(*out_size);
    void *mapped;
    vk_check(g_vk.MapMemory(g_vk.dev, linear_mem, 0, lin_reqs.size, 0, &mapped), "vkMapMemory");
    for (uint32_t y = 0; y < height; y++)
        memcpy(*out_data + (size_t)y * width * bpp,
               (uint8_t *)mapped + lin_layout.offset + (size_t)y * lin_layout.rowPitch,
               (size_t)width * bpp);
    g_vk.UnmapMemory(g_vk.dev, linear_mem);

    g_vk.DestroyImage(g_vk.dev, imported, NULL);
    g_vk.DestroyImage(g_vk.dev, linear_img, NULL);
    g_vk.FreeMemory(g_vk.dev, imported_mem, NULL);
    g_vk.FreeMemory(g_vk.dev, linear_mem, NULL);
}

int main(void) {
    size_t file_size;
    uint8_t *file_buf = read_file(INPUT_FILE, &file_size);
    fprintf(stderr, "read %zu bytes from %s\n", file_size, INPUT_FILE);

    NalUnit nals[8];
    int nal_count = extract_nals(file_buf, file_size, nals, 8);
    fprintf(stderr, "found %d NAL units (looking for SPS/PPS/IDR slice)\n", nal_count);

    SpsInfo sps = {0};
    PpsInfo pps = {0};
    NalUnit *idr_nal = NULL;
    int have_sps = 0, have_pps = 0;

    for (int i = 0; i < nal_count; i++) {
        fprintf(stderr, "  NAL[%d] type=%d ref_idc=%d size=%zu\n", i,
                nals[i].nal_unit_type, nals[i].nal_ref_idc, nals[i].size);
        size_t rbsp_size;
        uint8_t *rbsp = deescape_rbsp(nals[i].data, nals[i].size, &rbsp_size);
        if (nals[i].nal_unit_type == 7 && !have_sps) {
            parse_sps(rbsp, rbsp_size, &sps);
            have_sps = 1;
        } else if (nals[i].nal_unit_type == 8 && !have_pps) {
            parse_pps(rbsp, rbsp_size, &pps);
            have_pps = 1;
        } else if (nals[i].nal_unit_type == 5 && !idr_nal) {
            idr_nal = &nals[i];
        }
        free(rbsp);
        if (have_sps && have_pps && idr_nal)
            break;
    }
    if (!have_sps || !have_pps || !idr_nal) {
        fprintf(stderr, "missing SPS(%d)/PPS(%d)/IDR(%p) - can't continue\n",
                have_sps, have_pps, (void *)idr_nal);
        return 1;
    }

    unsigned int width_mbs = sps.pic_width_in_mbs_minus1 + 1;
    unsigned int height_map_units = sps.pic_height_in_map_units_minus1 + 1;
    unsigned int height_mbs = height_map_units * (2 - sps.frame_mbs_only_flag);
    unsigned int width = width_mbs * 16;
    unsigned int height = height_mbs * 16;
    fprintf(stderr, "SPS: profile_idc=%d width=%u height=%u max_num_ref_frames=%d "
                     "pic_order_cnt_type=%d log2_max_frame_num=%d\n",
            sps.profile_idc, width, height, sps.max_num_ref_frames,
            sps.pic_order_cnt_type, sps.log2_max_frame_num_minus4 + 4);
    fprintf(stderr, "PPS: entropy_coding_mode=%d pic_init_qp=%d "
                     "num_ref_idx_l0_active=%d\n",
            pps.entropy_coding_mode_flag, pps.pic_init_qp_minus26 + 26,
            pps.num_ref_idx_l0_active_minus1 + 1);

    size_t slice_rbsp_size;
    uint8_t *slice_rbsp = deescape_rbsp(idr_nal->data, idr_nal->size, &slice_rbsp_size);
    SliceHeader sh = {0};
    parse_slice_header(slice_rbsp, slice_rbsp_size, idr_nal->nal_unit_type, idr_nal->nal_ref_idc,
                        &sps, &pps, &sh);
    free(slice_rbsp);
    /* +8 for the NAL header byte that goes back in front of the slice data
     * buffer below (see that buffer's own comment) - this function's own
     * bit-offset math only knows about the RBSP payload after that byte. */
    unsigned int slice_data_bit_offset =
        8 + escaped_bit_offset(idr_nal->data, idr_nal->size, sh.header_bits);
    fprintf(stderr, "Slice: first_mb=%u slice_type=%u frame_num=%u idr_pic_id=%u "
                     "pic_order_cnt_lsb=%u slice_qp_delta=%d header_bits(deescaped)=%u "
                     "slice_data_bit_offset(escaped)=%u\n",
            sh.first_mb_in_slice, sh.slice_type, sh.frame_num, sh.idr_pic_id,
            sh.pic_order_cnt_lsb, sh.slice_qp_delta, sh.header_bits, slice_data_bit_offset);

    /* ------------------------------------------------------------------
     * VA-API: init, decode config/context, surfaces, single-frame decode.
     * ------------------------------------------------------------------ */
    int drm_fd = open(DRM_DEVICE, O_RDWR);
    if (drm_fd < 0) { perror("open drm device"); return 1; }
    VADisplay dpy = vaGetDisplayDRM(drm_fd);
    if (!dpy) { fprintf(stderr, "vaGetDisplayDRM failed\n"); return 1; }
    int major, minor;
    CHECK_VA(vaInitialize(dpy, &major, &minor), "vaInitialize");
    fprintf(stderr, "VA-API %d.%d, vendor: %s\n", major, minor, vaQueryVendorString(dpy));

    VAProfile profile = (sps.profile_idc == 66) ? VAProfileH264ConstrainedBaseline
                                                 : VAProfileH264Main;
    VAConfigAttrib attrib = { .type = VAConfigAttribRTFormat };
    CHECK_VA(vaGetConfigAttributes(dpy, profile, VAEntrypointVLD, &attrib, 1),
             "vaGetConfigAttributes");
    if (!(attrib.value & VA_RT_FORMAT_YUV420)) {
        fprintf(stderr, "driver doesn't support YUV420 for this profile/entrypoint\n");
        return 1;
    }
    VAConfigID config;
    VAConfigAttrib cfg_attrib = { .type = VAConfigAttribRTFormat, .value = VA_RT_FORMAT_YUV420 };
    CHECK_VA(vaCreateConfig(dpy, profile, VAEntrypointVLD, &cfg_attrib, 1, &config),
             "vaCreateConfig");

    /* One decode-target surface, plus a couple of extras as the driver's
     * own internal reference slots for the context (unused by this
     * single-IDR-frame spike, but vaCreateContext historically wants a
     * real surface array to size the DPB against).
     *
     * Force DRM_FORMAT_MOD_LINEAR explicitly - left to its own defaults,
     * this driver hands back a real block-linear tiled surface
     * (confirmed via vaExportSurfaceHandle: drm_format_modifier=
     * 0x3000000004fe014, the same non-linear family redroid-nvidia's own
     * NVENC investigation found), and reading tiled memory back with a
     * plain linear pitch (whether via vaGetImage or a raw mmap) produces
     * exactly the "large uniform bands" corruption this spike hit before
     * this fix - not noise, because a coarse tiling scheme's own
     * granularity makes broad, uniform-looking regions the natural
     * failure shape. Asking for LINEAR here sidesteps needing this
     * program to understand the tiling scheme at all. */
    uint64_t linear_modifier = 0; /* DRM_FORMAT_MOD_LINEAR */
    VADRMFormatModifierList modifier_list = { .num_modifiers = 1, .modifiers = &linear_modifier };
    VASurfaceAttrib surface_attrib = {
        .type = VASurfaceAttribDRMFormatModifiers,
        .flags = VA_SURFACE_ATTRIB_SETTABLE,
        .value = { .type = VAGenericValueTypePointer, .value = { .p = &modifier_list } },
    };
    VASurfaceID surfaces[3];
    CHECK_VA(vaCreateSurfaces(dpy, VA_RT_FORMAT_YUV420, align16(width), align16(height),
                              surfaces, 3, &surface_attrib, 1),
             "vaCreateSurfaces");

    VAContextID context;
    CHECK_VA(vaCreateContext(dpy, config, (int)align16(width), (int)align16(height),
                             VA_PROGRESSIVE, surfaces, 3, &context),
             "vaCreateContext");

    CHECK_VA(vaBeginPicture(dpy, context, surfaces[0]), "vaBeginPicture");

    VAPictureParameterBufferH264 pic_param;
    memset(&pic_param, 0, sizeof(pic_param));
    pic_param.CurrPic.picture_id = surfaces[0];
    pic_param.CurrPic.frame_idx = sh.frame_num;
    pic_param.CurrPic.flags = 0; /* frame, not a field */
    pic_param.CurrPic.TopFieldOrderCnt = (int)sh.pic_order_cnt_lsb;
    pic_param.CurrPic.BottomFieldOrderCnt = (int)sh.pic_order_cnt_lsb;
    for (int i = 0; i < 16; i++)
        pic_param.ReferenceFrames[i].picture_id = VA_INVALID_SURFACE;
    pic_param.picture_width_in_mbs_minus1 = sps.pic_width_in_mbs_minus1;
    pic_param.picture_height_in_mbs_minus1 = sps.pic_height_in_map_units_minus1;
    pic_param.bit_depth_luma_minus8 = 0;
    pic_param.bit_depth_chroma_minus8 = 0;
    pic_param.num_ref_frames = sps.max_num_ref_frames;
    pic_param.seq_fields.bits.chroma_format_idc = 1; /* 4:2:0 */
    pic_param.seq_fields.bits.frame_mbs_only_flag = sps.frame_mbs_only_flag;
    pic_param.seq_fields.bits.pic_order_cnt_type = sps.pic_order_cnt_type;
    pic_param.seq_fields.bits.log2_max_frame_num_minus4 = sps.log2_max_frame_num_minus4;
    pic_param.seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4 =
        sps.log2_max_pic_order_cnt_lsb_minus4;
    pic_param.num_slice_groups_minus1 = 0;
    pic_param.pic_init_qp_minus26 = pps.pic_init_qp_minus26;
    /* Parsed from PPS but never carried through to here (this program's
     * own earlier version) - defaulted to 0 via memset instead of the
     * real encoded value, dequantizing chroma coefficients at the wrong
     * effective QP. Explains exactly the symptom that was left after the
     * chroma plane-width fix: luma bit-exact (this field doesn't touch
     * luma at all), chroma close but consistently off by a real, non-
     * random amount (PSNR ~25dB, not noise-level). Baseline profile only
     * ever had one chroma QP offset field; second_chroma_qp_index_offset
     * (High profile onward) isn't present in this bitstream's own PPS at
     * all, so mirroring the same value here matches the decoder's own
     * required fallback when only the first is signaled. */
    pic_param.chroma_qp_index_offset = pps.chroma_qp_index_offset;
    pic_param.second_chroma_qp_index_offset = pps.chroma_qp_index_offset;
    pic_param.pic_fields.bits.entropy_coding_mode_flag = pps.entropy_coding_mode_flag;
    pic_param.pic_fields.bits.weighted_pred_flag = pps.weighted_pred_flag;
    pic_param.pic_fields.bits.weighted_bipred_idc = pps.weighted_bipred_idc;
    pic_param.pic_fields.bits.deblocking_filter_control_present_flag =
        pps.deblocking_filter_control_present_flag;
    pic_param.pic_fields.bits.redundant_pic_cnt_present_flag =
        pps.redundant_pic_cnt_present_flag;
    pic_param.pic_fields.bits.reference_pic_flag = (idr_nal->nal_ref_idc != 0);
    pic_param.pic_fields.bits.pic_order_present_flag = pps.pic_order_present_flag;

    VABufferID pic_param_buf;
    CHECK_VA(vaCreateBuffer(dpy, context, VAPictureParameterBufferType, sizeof(pic_param), 1,
                            &pic_param, &pic_param_buf),
             "vaCreateBuffer(pic_param)");
    CHECK_VA(vaRenderPicture(dpy, context, &pic_param_buf, 1), "vaRenderPicture(pic_param)");

    VAIQMatrixBufferH264 iq_matrix;
    memset(&iq_matrix, 0, sizeof(iq_matrix));
    memset(iq_matrix.ScalingList4x4, 16, sizeof(iq_matrix.ScalingList4x4));
    memset(iq_matrix.ScalingList8x8, 16, sizeof(iq_matrix.ScalingList8x8));
    VABufferID iq_matrix_buf;
    CHECK_VA(vaCreateBuffer(dpy, context, VAIQMatrixBufferType, sizeof(iq_matrix), 1,
                            &iq_matrix, &iq_matrix_buf),
             "vaCreateBuffer(iq_matrix)");
    CHECK_VA(vaRenderPicture(dpy, context, &iq_matrix_buf, 1), "vaRenderPicture(iq_matrix)");

    unsigned int num_mbs = width_mbs * height_mbs;
    VASliceParameterBufferH264 slice_param;
    memset(&slice_param, 0, sizeof(slice_param));
    /* +1 for the NAL header byte - see the slice data buffer below for why
     * it has to go back in front of idr_nal->data/size (which, everywhere
     * else in this program, deliberately exclude it). */
    slice_param.slice_data_size = (unsigned int)idr_nal->size + 1;
    slice_param.slice_data_offset = 0;
    slice_param.slice_data_flag = VA_SLICE_DATA_FLAG_ALL;
    slice_param.slice_data_bit_offset = (uint16_t)slice_data_bit_offset;
    slice_param.first_mb_in_slice = sh.first_mb_in_slice;
    slice_param.slice_type = sh.slice_type % 5; /* 5-9 alias 0-4 per spec */
    slice_param.direct_spatial_mv_pred_flag = 0;
    slice_param.num_ref_idx_l0_active_minus1 = pps.num_ref_idx_l0_active_minus1;
    slice_param.num_ref_idx_l1_active_minus1 = pps.num_ref_idx_l1_active_minus1;
    slice_param.cabac_init_idc = 0;
    slice_param.slice_qp_delta = sh.slice_qp_delta;
    slice_param.disable_deblocking_filter_idc = 0;
    slice_param.slice_alpha_c0_offset_div2 = 0;
    slice_param.slice_beta_offset_div2 = 0;
    slice_param.luma_log2_weight_denom = 0;
    slice_param.chroma_log2_weight_denom = 0;
    for (int i = 0; i < 32; i++) {
        slice_param.RefPicList0[i].picture_id = VA_INVALID_SURFACE;
        slice_param.RefPicList1[i].picture_id = VA_INVALID_SURFACE;
    }
    (void)num_mbs;

    VABufferID slice_param_buf;
    CHECK_VA(vaCreateBuffer(dpy, context, VASliceParameterBufferType, sizeof(slice_param), 1,
                            &slice_param, &slice_param_buf),
             "vaCreateBuffer(slice_param)");
    CHECK_VA(vaRenderPicture(dpy, context, &slice_param_buf, 1), "vaRenderPicture(slice_param)");

    /* Slice DATA buffer: the driver wants this still Annex-B-escaped, and -
     * the actual bug this spike spent a long time chasing before finding
     * it - it wants the full NAL unit INCLUDING its own 1-byte header
     * (forbidden_zero_bit/nal_ref_idc/nal_unit_type), not just the RBSP
     * payload after it. This program's own NalUnit.data/size deliberately
     * exclude that header byte everywhere else (it's already been parsed
     * out into nal_ref_idc/nal_unit_type), so it has to be added back in
     * right here: `idr_nal->data - 1`, size `+ 1`. Skipping it (this
     * program's own earlier version) fed the driver a buffer whose first
     * byte is really the start of first_mb_in_slice's own ue(v) coding -
     * every VA-API implementation's own internal parser (confirmed by
     * reading nvidia-vaapi-driver's source: it re-wraps this exact buffer
     * with its own 00 00 01 start code before handing it to cuvidDecode
     * Picture(), which then parses the NAL header itself) would decode a
     * bogus nal_unit_type/nal_ref_idc from real slice-header bits and
     * silently produce nothing real - not a NVIDIA-specific bug, this is
     * the standard VA-API slice-data convention every backend expects. */
    VABufferID slice_data_buf;
    CHECK_VA(vaCreateBuffer(dpy, context, VASliceDataBufferType, (unsigned int)idr_nal->size + 1, 1,
                            (void *)(idr_nal->data - 1), &slice_data_buf),
             "vaCreateBuffer(slice_data)");
    CHECK_VA(vaRenderPicture(dpy, context, &slice_data_buf, 1), "vaRenderPicture(slice_data)");

    CHECK_VA(vaEndPicture(dpy, context), "vaEndPicture");
    CHECK_VA(vaSyncSurface(dpy, surfaces[0]), "vaSyncSurface");
    fprintf(stderr, "decode submitted and synced\n");

    /* NVDEC's own hardware decode target came back tiled regardless of the
     * DRM_FORMAT_MOD_LINEAR request above (same non-zero modifier either
     * way) - the hardware decoder itself apparently can't write linear
     * directly, matching how real playback pipelines never read a decode
     * target's raw memory either. Do what they do: blit through VA-API's
     * own VPP (VAEntrypointVideoProc) entrypoint into a second, genuinely
     * plain surface, then read *that* one back instead. */
    VAConfigID vpp_config;
    CHECK_VA(vaCreateConfig(dpy, VAProfileNone, VAEntrypointVideoProc, NULL, 0, &vpp_config),
             "vaCreateConfig(vpp)");
    VASurfaceID vpp_out;
    CHECK_VA(vaCreateSurfaces(dpy, VA_RT_FORMAT_YUV420, align16(width), align16(height),
                              &vpp_out, 1, &surface_attrib, 1),
             "vaCreateSurfaces(vpp_out)");
    VAContextID vpp_context;
    CHECK_VA(vaCreateContext(dpy, vpp_config, (int)align16(width), (int)align16(height),
                             VA_PROGRESSIVE, &vpp_out, 1, &vpp_context),
             "vaCreateContext(vpp)");

    CHECK_VA(vaBeginPicture(dpy, vpp_context, vpp_out), "vaBeginPicture(vpp)");
    VABufferID vpp_buf;
    VAProcPipelineParameterBuffer *vpp_param;
    CHECK_VA(vaCreateBuffer(dpy, vpp_context, VAProcPipelineParameterBufferType,
                            sizeof(VAProcPipelineParameterBuffer), 1, NULL, &vpp_buf),
             "vaCreateBuffer(vpp)");
    CHECK_VA(vaMapBuffer(dpy, vpp_buf, (void **)&vpp_param), "vaMapBuffer(vpp)");
    memset(vpp_param, 0, sizeof(*vpp_param));
    vpp_param->surface = surfaces[0]; /* the tiled decode target, as input */
    vpp_param->surface_region = NULL;
    vpp_param->output_region = NULL;
    vpp_param->output_background_color = 0;
    vpp_param->filter_flags = VA_FRAME_PICTURE;
    CHECK_VA(vaUnmapBuffer(dpy, vpp_buf), "vaUnmapBuffer(vpp)");
    CHECK_VA(vaRenderPicture(dpy, vpp_context, &vpp_buf, 1), "vaRenderPicture(vpp)");
    CHECK_VA(vaEndPicture(dpy, vpp_context), "vaEndPicture(vpp)");
    CHECK_VA(vaSyncSurface(dpy, vpp_out), "vaSyncSurface(vpp_out)");
    fprintf(stderr, "VPP blit to plain surface done - reading that back\n");

    /* elFarto/nvidia-vaapi-driver is a community shim built primarily for
     * video *playback* (handing decoded frames straight into EGL/GL for a
     * media player) - vaGetImage (a CPU-readback path that shim's own
     * primary use case never needs) came back with a suspiciously clean,
     * perfectly uniform two-region placeholder pattern (flat 128 for 256
     * rows, flat 16 for the rest) rather than real or even garbled decoded
     * content, on a driver call that itself reported success. Matches
     * exactly the "accepts the call, doesn't do the real work" shape this
     * whole investigation has run into repeatedly elsewhere in this
     * community NVIDIA stack (see redroid-nvidia's own fence-import
     * finding). Going straight to the real memory instead, the same way
     * that investigation did: export the decoded surface as a dma-buf via
     * vaExportSurfaceHandle and mmap it directly, bypassing vaGetImage's
     * own copy path entirely. */
    VADRMPRIMESurfaceDescriptor prime_desc;
    CHECK_VA(vaExportSurfaceHandle(dpy, vpp_out, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                                   VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS,
                                   &prime_desc),
             "vaExportSurfaceHandle");
    fprintf(stderr, "exported surface: fourcc=%.4s width=%u height=%u num_objects=%u "
                     "num_layers=%u\n",
            (char *)&prime_desc.fourcc, prime_desc.width, prime_desc.height,
            prime_desc.num_objects, prime_desc.num_layers);
    for (uint32_t i = 0; i < prime_desc.num_layers; i++) {
        fprintf(stderr, "  layer[%u]: drm_format=0x%x num_planes=%u offset[0]=%u pitch[0]=%u "
                        "object_index[0]=%u\n",
                i, prime_desc.layers[i].drm_format, prime_desc.layers[i].num_planes,
                prime_desc.layers[i].offset[0], prime_desc.layers[i].pitch[0],
                prime_desc.layers[i].object_index[0]);
    }
    for (uint32_t i = 0; i < prime_desc.num_objects; i++)
        fprintf(stderr, "  object[%u]: fd=%d size=%u drm_format_modifier=0x%llx\n", i,
                prime_desc.objects[i].fd, prime_desc.objects[i].size,
                (unsigned long long)prime_desc.objects[i].drm_format_modifier);

    /* Both planes came back with a real, non-zero DRM format modifier
     * (confirmed above) - de-tile each through Vulkan rather than trust a
     * raw mmap, exactly the lesson from redroid-nvidia's own investigation. */
    vk_init();
    FILE *out = fopen(OUTPUT_FILE, "wb");
    if (!out) { perror("fopen output"); return 1; }
    for (uint32_t li = 0; li < prime_desc.num_layers; li++) {
        uint32_t obj_idx = prime_desc.layers[li].object_index[0];
        int fd = prime_desc.objects[obj_idx].fd;
        uint64_t modifier = prime_desc.objects[obj_idx].drm_format_modifier;
        unsigned int plane_height = (li == 0) ? height : height / 2;
        /* NV12 chroma has half as many *texels* across as luma, not the
         * same width - each R8G8 texel here is one interleaved U,V pair
         * covering a 2x2 luma block. Passing the full luma width (this
         * program's own earlier version) told Vulkan the image was twice
         * as many texels wide as the real plane, which happened to
         * compute a plausible-looking rowPitch anyway (real chroma tiling
         * padding and "2x the real width at half the depth" aren't far
         * apart numerically) while actually reading each real row's bytes
         * at the wrong stride - exactly the washed-out/ghosted look this
         * produced, structure present but wrong (PSNR ~10dB) rather than
         * random. */
        unsigned int plane_width = (li == 0) ? width : width / 2;
        VkFormat vk_fmt = (prime_desc.layers[li].drm_format == 0x20203852) ? VK_FORMAT_R8_UNORM
                                                                            : VK_FORMAT_R8G8_UNORM;
        fprintf(stderr, "  de-tiling layer %u via Vulkan: drm_format=0x%x modifier=0x%llx "
                        "plane_width=%u plane_height=%u\n",
                li, prime_desc.layers[li].drm_format, (unsigned long long)modifier, plane_width,
                plane_height);
        uint8_t *plane_data;
        size_t plane_size;
        vk_detile(fd, vk_fmt, plane_width, plane_height, modifier, prime_desc.objects[obj_idx].size,
                  &plane_data, &plane_size);
        fwrite(plane_data, 1, plane_size, out);
        free(plane_data);
    }
    fclose(out);
    fprintf(stderr, "wrote %ux%u NV12 (from real dma-buf mmap) to %s\n", width, height,
            OUTPUT_FILE);

    for (uint32_t i = 0; i < prime_desc.num_objects; i++)
        close(prime_desc.objects[i].fd);
    vaDestroyContext(dpy, context);
    vaDestroySurfaces(dpy, surfaces, 3);
    vaDestroyConfig(dpy, config);
    vaTerminate(dpy);
    close(drm_fd);
    return 0;
}
