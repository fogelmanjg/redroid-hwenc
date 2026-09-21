//#define LOG_NDEBUG 0
#define LOG_TAG "VaapiEncComponent"

#include "VaapiEncComponent.h"

#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <string>

#include <log/log.h>
#include <media/stagefright/MediaDefs.h>
#include <util/C2InterfaceHelper.h>

#include "protocol.h"

namespace android {

VaapiEncInterface::VaapiEncInterface(const std::shared_ptr<C2ReflectorHelper> &helper)
    : SimpleInterface<void>::BaseParams(helper, "c2.hardware.encoder.h264",
                                         C2Component::KIND_ENCODER, C2Component::DOMAIN_VIDEO,
                                         MEDIA_MIMETYPE_VIDEO_AVC) {
    noPrivateBuffers();
    noInputReferences();
    noOutputReferences();
    noTimeStretch();
    setDerivedInstance(this);

    addParameter(DefineParam(mSize, C2_PARAMKEY_PICTURE_SIZE)
                         .withDefault(new C2StreamPictureSizeInfo::input(0u, 320, 240))
                         .withFields({
                                 C2F(mSize, width).inRange(2, 2560, 2),
                                 C2F(mSize, height).inRange(2, 2560, 2),
                         })
                         .withSetter(SizeSetter)
                         .build());

    addParameter(DefineParam(mInputUsage, C2_PARAMKEY_INPUT_STREAM_USAGE)
                         .withConstValue(new C2StreamUsageTuning::input(
                                 0u,
                                 static_cast<uint64_t>(
                                         android::hardware::graphics::common::V1_0::BufferUsage::
                                                 VIDEO_ENCODER)))
                         .build());

    addParameter(DefineParam(mProfileLevel, C2_PARAMKEY_PROFILE_LEVEL)
                         .withDefault(new C2StreamProfileLevelInfo::output(
                                 0u, PROFILE_AVC_CONSTRAINED_BASELINE, LEVEL_AVC_3))
                         .withFields({
                                 C2F(mProfileLevel, profile).oneOf({
                                         PROFILE_AVC_CONSTRAINED_BASELINE,
                                 }),
                                 C2F(mProfileLevel, level).oneOf({
                                         LEVEL_AVC_3,
                                 }),
                         })
                         .withSetter(ProfileLevelSetter)
                         .build());
}

C2R VaapiEncInterface::ProfileLevelSetter(bool mayBlock, C2P<C2StreamProfileLevelInfo::output> &me) {
    (void)mayBlock;
    (void)me;
    return C2R::Ok();
}

C2R VaapiEncInterface::SizeSetter(bool mayBlock, const C2P<C2StreamPictureSizeInfo::input> &oldMe,
                                   C2P<C2StreamPictureSizeInfo::input> &me) {
    (void)mayBlock;
    C2R res = C2R::Ok();
    if (!me.F(me.v.width).supportsAtAll(me.v.width)) {
        res = res.plus(C2SettingResultBuilder::BadValue(me.F(me.v.width)));
        me.set().width = oldMe.v.width;
    }
    if (!me.F(me.v.height).supportsAtAll(me.v.height)) {
        res = res.plus(C2SettingResultBuilder::BadValue(me.F(me.v.height)));
        me.set().height = oldMe.v.height;
    }
    return res;
}

VaapiEncComponent::VaapiEncComponent(const char *name, c2_node_id_t id,
                                      const std::shared_ptr<VaapiEncInterface> &intf)
    : SimpleC2Component(std::make_shared<SimpleInterface<VaapiEncInterface>>(name, id, intf)),
      mIntf(intf) {}

c2_status_t VaapiEncComponent::onInit() {
    return C2_OK;
}

c2_status_t VaapiEncComponent::onStop() {
    return C2_OK;
}

void VaapiEncComponent::onReset() {}

void VaapiEncComponent::onRelease() {}

c2_status_t VaapiEncComponent::onFlush_sm() {
    return C2_OK;
}

c2_status_t VaapiEncComponent::drain(uint32_t drainMode,
                                      const std::shared_ptr<C2BlockPool> &pool) {
    (void)drainMode;
    (void)pool;
    return C2_OK;
}

long VaapiEncComponent::encodeViaDaemon(int dmabufFd, uint32_t width, uint32_t height,
                                         uint32_t strideY, uint32_t strideUv, uint32_t offsetUv,
                                         uint32_t dmabufSize, uint64_t drmFormatModifier,
                                         unsigned char **outBuf) {
    int sockFd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sockFd < 0) {
        ALOGE("socket() failed: %s", strerror(errno));
        return -1;
    }
    struct sockaddr_un addr = {};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, VAAPI_DAEMON_SOCKET_PATH, sizeof(addr.sun_path) - 1);
    if (connect(sockFd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ALOGE("connect(%s) failed: %s", VAAPI_DAEMON_SOCKET_PATH, strerror(errno));
        close(sockFd);
        return -1;
    }

    EncodeRequest req = {};
    req.width = width;
    req.height = height;
    req.stride_y = strideY;
    req.stride_uv = strideUv;
    req.offset_uv = offsetUv;
    req.dmabuf_size = dmabufSize;
    req.drm_format_modifier = drmFormatModifier;

    char cmsgBuf[CMSG_SPACE(sizeof(int))];
    struct iovec iov = {.iov_base = &req, .iov_len = sizeof(req)};
    struct msghdr msg = {};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cmsgBuf;
    msg.msg_controllen = sizeof(cmsgBuf);
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &dmabufFd, sizeof(int));

    if (sendmsg(sockFd, &msg, 0) < 0) {
        ALOGE("sendmsg to daemon failed: %s", strerror(errno));
        close(sockFd);
        return -1;
    }

    EncodeResponse resp;
    if (read(sockFd, &resp, sizeof(resp)) != (ssize_t)sizeof(resp)) {
        ALOGE("read(response header) failed: %s", strerror(errno));
        close(sockFd);
        return -1;
    }
    if (resp.status != 0) {
        ALOGE("daemon reported an error: %d", resp.status);
        close(sockFd);
        return -1;
    }

    unsigned char *coded = (unsigned char *)malloc(resp.coded_size);
    size_t got = 0;
    while (got < resp.coded_size) {
        ssize_t n = read(sockFd, coded + got, resp.coded_size - got);
        if (n <= 0) {
            ALOGE("read(response body) failed: %s", strerror(errno));
            free(coded);
            close(sockFd);
            return -1;
        }
        got += n;
    }
    close(sockFd);
    *outBuf = coded;
    return (long)resp.coded_size;
}

void VaapiEncComponent::process(const std::unique_ptr<C2Work> &work,
                                 const std::shared_ptr<C2BlockPool> &pool) {
    work->result = C2_OK;
    work->workletsProcessed = 0u;
    work->worklets.front()->output.flags = work->input.flags;

    if (work->input.buffers.empty()) {
        work->workletsProcessed = 1u;
        return;
    }

    std::shared_ptr<C2Buffer> inputBuffer = work->input.buffers[0];
    if (inputBuffer->data().graphicBlocks().empty()) {
        ALOGE("input C2Buffer has no graphic block");
        work->result = C2_CORRUPTED;
        work->workletsProcessed = 1u;
        return;
    }

    // Same fd-extraction pattern as external/v4l2_codec2's createInputFrame()
    // (Tier 1's finding): a C2ConstGraphicBlock's native handle just *is* a
    // small struct of dma-buf fds, no mapper call needed. Deliberately does
    // NOT call block.map()/layout() -- this build's Mapper HAL doesn't
    // implement the flexible-layout verb that needs (confirmed ENOSYS in the
    // Tier 5.4 gralloc probe).
    C2ConstGraphicBlock block = inputBuffer->data().graphicBlocks().front();
    const C2Handle *const handle = block.handle();
    if (!handle || handle->numFds < 1) {
        ALOGE("input graphic block has no dma-buf fd");
        work->result = C2_CORRUPTED;
        work->workletsProcessed = 1u;
        return;
    }
    // Tier 5.8 finding: a real Surface-sourced frame (scrcpy's virtual
    // display capture via GraphicBufferSource) does NOT arrive as a
    // cros_gralloc_handle at all -- confirmed authoritatively via
    // cros_gralloc's own cros_gralloc_convert_handle() validation logic
    // (cros_gralloc_helpers.cc), which requires the handle's total size to
    // exactly equal sizeof(cros_gralloc_handle) (156 bytes); this one
    // measures 108 (numFds=1, numInts=23). It's some other, more generic
    // native_handle_t this pipeline uses when a buffer crosses into this
    // process a different way, not this vendor's own gralloc wire format.
    // Reading it structurally isn't possible without knowing that format;
    // these offsets are empirical (dumped the raw ints and matched them
    // against the buffer's known config: 600x1000, and a 2560-byte stride
    // that lines up with a 640px-wide RGBA plane -- exactly 4 bytes/px).
    // The buffer is RGBA, not NV12/YUV, because CCodec left the input as
    // OMX_COLOR_FormatAndroidOpaque (confirmed in logcat:
    // color-format = 2130708361): this Mesa/minigbm stack can't allocate a
    // buffer that's both GPU-renderable and real YUV at once (see DEVLOG),
    // so GraphicBufferSource hands over SurfaceFlinger's raw GL-composited
    // RGBA output instead. drm_format_modifier is left at 0 (LINEAR) since
    // this handle format has no identifiable modifier field at all, so the
    // real modifier can't be read from it. Determined it a different way
    // instead: minigbm's amdgpu.c registers one combination per modifier
    // Mesa reports via dri_query_modifiers() for a GPU-render-target ABGR
    // buffer, all sharing the same priority -- drv_get_combination() (in
    // drv.c) breaks priority ties by taking the FIRST-registered match, so
    // whichever modifier Mesa returns first for this format/GPU is the one
    // that wins. Queried Mesa directly for that order (eglQueryDmaBufModifiersEXT
    // for DRM_FORMAT_ABGR8888 on this exact GPU, with AMD_DEBUG=nodcc set the
    // same way surfaceflinger has it): 4 modifiers, all DCC=0 (confirms nodcc
    // affects this list, not just the encode-time DCC check), first one
    // 0x0200000000401a01 (AMD_FMT_MOD_TILE_VER_GFX9, tile=GFX9_64K_D_X).
    int dmabufFd = handle->data[0];
    const int32_t *ints = &handle->data[handle->numFds];
    uint32_t width = (uint32_t)ints[2];
    uint32_t height = (uint32_t)ints[3];
    uint32_t strideY = (uint32_t)ints[6];
    uint32_t dmabufSize = strideY * height;
    uint64_t drmFormatModifier = 0x0200000000401a01ULL; /* GFX9_64K_D_X, DCC=0 */

    unsigned char *coded = nullptr;
    long codedSize = encodeViaDaemon(dmabufFd, width, height, strideY, /*strideUv=*/0,
                                      /*offsetUv=*/0, dmabufSize, drmFormatModifier, &coded);
    if (codedSize < 0) {
        work->result = C2_CORRUPTED;
        work->workletsProcessed = 1u;
        return;
    }

    std::shared_ptr<C2LinearBlock> outBlock;
    C2MemoryUsage usage = {C2MemoryUsage::CPU_READ, C2MemoryUsage::CPU_WRITE};
    c2_status_t err = pool->fetchLinearBlock((size_t)codedSize, usage, &outBlock);
    if (err != C2_OK) {
        ALOGE("fetchLinearBlock failed: %d", err);
        free(coded);
        work->result = err;
        work->workletsProcessed = 1u;
        return;
    }
    {
        C2WriteView wView = outBlock->map().get();
        if (wView.error() != C2_OK) {
            ALOGE("output block map failed: %d", wView.error());
            free(coded);
            work->result = wView.error();
            work->workletsProcessed = 1u;
            return;
        }
        memcpy(wView.base(), coded, codedSize);
    }
    free(coded);

    std::shared_ptr<C2Buffer> outBuffer = createLinearBuffer(outBlock, 0, codedSize);
    outBuffer->setInfo(
            std::make_shared<C2StreamPictureTypeMaskInfo::output>(0u, C2Config::SYNC_FRAME));

    work->worklets.front()->output.flags = (C2FrameData::flags_t)0;
    work->worklets.front()->output.buffers.clear();
    work->worklets.front()->output.buffers.push_back(outBuffer);
    work->worklets.front()->output.ordinal = work->input.ordinal;
    work->workletsProcessed = 1u;

    ALOGD("encoded %ld bytes via the VA-API daemon", codedSize);
}

}  // namespace android
