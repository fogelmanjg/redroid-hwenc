//#define LOG_NDEBUG 0
#define LOG_TAG "VaapiDecComponent"

#include "VaapiDecComponent.h"

#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <system/graphics.h>

#include <log/log.h>
#include <media/stagefright/MediaDefs.h>
#include <util/C2InterfaceHelper.h>

#include "protocol.h"

namespace android {

namespace {
// Rounds up to the next multiple of 16, matching this daemon's own
// align16() (decode_h264.c) - a gralloc allocation for the decoded frame
// needs to be at least as large as what the daemon's macroblock-aligned
// decode surface actually produced.
uint32_t align16(uint32_t v) {
    return (v + 15) & ~15u;
}
}  // namespace

VaapiDecInterface::VaapiDecInterface(const std::shared_ptr<C2ReflectorHelper> &helper)
    : SimpleInterface<void>::BaseParams(helper, "c2.hardware.decoder.h264",
                                         C2Component::KIND_DECODER, C2Component::DOMAIN_VIDEO,
                                         MEDIA_MIMETYPE_VIDEO_AVC) {
    noPrivateBuffers();
    noInputReferences();
    noOutputReferences();
    noTimeStretch();
    setDerivedInstance(this);

    // Coded and output picture size are the same for this decoder (see this
    // struct's own header comment) - an ::output param, same as
    // C2SoftAvcDec's own IntfImpl, since it's the decoder that determines
    // this from the bitstream, not something a client configures going in.
    addParameter(DefineParam(mSize, C2_PARAMKEY_PICTURE_SIZE)
                         .withDefault(new C2StreamPictureSizeInfo::output(0u, 320, 240))
                         .withFields({
                                 C2F(mSize, width).inRange(2, 2560, 2),
                                 C2F(mSize, height).inRange(2, 2560, 2),
                         })
                         .withSetter(SizeSetter)
                         .build());

    // ::input here (not ::output like the encoder's own mProfileLevel) -
    // for a decoder the profile/level describe the incoming bitstream, not
    // something this component produces. Constrained Baseline / Level 3
    // only, matching exactly what decode_h264.c/tier6-vaapi-decode's own
    // parser actually supports (baseline profile, CAVLC, one slice per
    // picture - see its own header comment for the full scope).
    addParameter(DefineParam(mProfileLevel, C2_PARAMKEY_PROFILE_LEVEL)
                         .withDefault(new C2StreamProfileLevelInfo::input(
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

C2R VaapiDecInterface::ProfileLevelSetter(bool mayBlock, C2P<C2StreamProfileLevelInfo::input> &me) {
    (void)mayBlock;
    (void)me;
    return C2R::Ok();
}

C2R VaapiDecInterface::SizeSetter(bool mayBlock, const C2P<C2StreamPictureSizeInfo::output> &oldMe,
                                   C2P<C2StreamPictureSizeInfo::output> &me) {
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

VaapiDecComponent::VaapiDecComponent(const char *name, c2_node_id_t id,
                                      const std::shared_ptr<VaapiDecInterface> &intf)
    : SimpleC2Component(std::make_shared<SimpleInterface<VaapiDecInterface>>(name, id, intf)),
      mIntf(intf) {}

c2_status_t VaapiDecComponent::onInit() {
    return C2_OK;
}

c2_status_t VaapiDecComponent::onStop() {
    return C2_OK;
}

void VaapiDecComponent::onReset() {}

void VaapiDecComponent::onRelease() {}

c2_status_t VaapiDecComponent::onFlush_sm() {
    return C2_OK;
}

c2_status_t VaapiDecComponent::drain(uint32_t drainMode, const std::shared_ptr<C2BlockPool> &pool) {
    (void)drainMode;
    (void)pool;
    return C2_OK;
}

int VaapiDecComponent::decodeViaDaemon(uint32_t widthHint, uint32_t heightHint,
                                        const uint8_t *bitstream, size_t bitstreamSize,
                                        uint8_t **outData, size_t *outSize, uint32_t *outWidth,
                                        uint32_t *outHeight) {
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

    VaapiCommand cmd = VAAPI_CMD_DECODE;
    if (write(sockFd, &cmd, sizeof(cmd)) != (ssize_t)sizeof(cmd)) {
        ALOGE("write(command tag) failed: %s", strerror(errno));
        close(sockFd);
        return -1;
    }

    DecodeRequest req = {};
    req.width = widthHint;
    req.height = heightHint;
    req.bitstream_size = (uint32_t)bitstreamSize;
    if (write(sockFd, &req, sizeof(req)) != (ssize_t)sizeof(req)) {
        ALOGE("write(DecodeRequest) failed: %s", strerror(errno));
        close(sockFd);
        return -1;
    }
    size_t sent = 0;
    while (sent < bitstreamSize) {
        ssize_t n = write(sockFd, bitstream + sent, bitstreamSize - sent);
        if (n <= 0) {
            ALOGE("write(bitstream) failed: %s", strerror(errno));
            close(sockFd);
            return -1;
        }
        sent += (size_t)n;
    }

    DecodeResponse resp;
    if (read(sockFd, &resp, sizeof(resp)) != (ssize_t)sizeof(resp)) {
        ALOGE("read(response header) failed: %s", strerror(errno));
        close(sockFd);
        return -1;
    }
    if (resp.status != 0) {
        ALOGE("daemon reported a decode error: %d", resp.status);
        close(sockFd);
        return -1;
    }

    uint8_t *frame = (uint8_t *)malloc(resp.frame_size);
    size_t got = 0;
    while (got < resp.frame_size) {
        ssize_t n = read(sockFd, frame + got, resp.frame_size - got);
        if (n <= 0) {
            ALOGE("read(response body) failed: %s", strerror(errno));
            free(frame);
            close(sockFd);
            return -1;
        }
        got += (size_t)n;
    }
    close(sockFd);

    // decode_h264.c derives width/height from the stream's own SPS (the
    // only authoritative source - see decode_h264.h) rather than trusting
    // this component's hints, and the daemon forwards them back rather than
    // just the raw NV12 bytes precisely so this component doesn't have to
    // duplicate that parsing just to know how to lay the frame into a
    // gralloc buffer below.
    *outData = frame;
    *outSize = resp.frame_size;
    return 0;
}

void VaapiDecComponent::process(const std::unique_ptr<C2Work> &work,
                                 const std::shared_ptr<C2BlockPool> &pool) {
    work->result = C2_OK;
    work->workletsProcessed = 0u;
    work->worklets.front()->output.flags = work->input.flags;

    if (work->input.buffers.empty()) {
        work->workletsProcessed = 1u;
        return;
    }

    C2ReadView rView = work->input.buffers[0]->data().linearBlocks().front().map().get();
    size_t inSize = rView.capacity();
    if (inSize && rView.error()) {
        ALOGE("input read view map failed: %d", rView.error());
        work->result = rView.error();
        work->workletsProcessed = 1u;
        return;
    }
    if (inSize == 0) {
        work->workletsProcessed = 1u;
        return;
    }

    uint8_t *frame = nullptr;
    size_t frameSize = 0;
    uint32_t width = mIntf->width();
    uint32_t height = mIntf->height();
    int rc = decodeViaDaemon(width, height, rView.data(), inSize, &frame, &frameSize, &width,
                              &height);
    if (rc != 0) {
        work->result = C2_CORRUPTED;
        work->workletsProcessed = 1u;
        return;
    }

    std::shared_ptr<C2GraphicBlock> outBlock;
    uint32_t format = HAL_PIXEL_FORMAT_YV12;
    C2MemoryUsage usage = {C2MemoryUsage::CPU_READ, C2MemoryUsage::CPU_WRITE};
    c2_status_t err = pool->fetchGraphicBlock(align16(width), height, format, usage, &outBlock);
    if (err != C2_OK) {
        ALOGE("fetchGraphicBlock for output failed: %d", err);
        free(frame);
        work->result = err;
        work->workletsProcessed = 1u;
        return;
    }

    {
        C2GraphicView wView = outBlock->map().get();
        if (wView.error() != C2_OK) {
            ALOGE("output graphic view map failed: %d", wView.error());
            free(frame);
            work->result = wView.error();
            work->workletsProcessed = 1u;
            return;
        }

        // frame is tightly-packed NV12 from the daemon: width*height luma
        // bytes, then width*height/2 interleaved-UV bytes (see
        // decode_h264.h's own contract). The destination block's real
        // layout is whatever gralloc actually allocated for
        // HAL_PIXEL_FORMAT_YV12 on this stack - not necessarily planar with
        // no padding - so this copies plane-by-plane using the layout's own
        // rowInc/colInc rather than assuming any particular stride or
        // planar-vs-semiplanar arrangement. Same technique
        // C2SoftVpxDec::outputBuffer() uses to go from a foreign decoder's
        // own buffer into a C2GraphicView, generalized to also cover a
        // semi-planar (colInc=2) destination, which YV12 never is but a
        // future NV12-capable allocation might be.
        const uint8_t *srcY = frame;
        const uint8_t *srcUv = frame + (size_t)width * height;

        uint8_t *dstY = const_cast<uint8_t *>(wView.data()[C2PlanarLayout::PLANE_Y]);
        uint8_t *dstU = const_cast<uint8_t *>(wView.data()[C2PlanarLayout::PLANE_U]);
        uint8_t *dstV = const_cast<uint8_t *>(wView.data()[C2PlanarLayout::PLANE_V]);
        C2PlanarLayout layout = wView.layout();
        size_t dstYStride = layout.planes[C2PlanarLayout::PLANE_Y].rowInc;
        size_t dstUStride = layout.planes[C2PlanarLayout::PLANE_U].rowInc;
        size_t dstVStride = layout.planes[C2PlanarLayout::PLANE_V].rowInc;
        int32_t dstUColInc = layout.planes[C2PlanarLayout::PLANE_U].colInc;
        int32_t dstVColInc = layout.planes[C2PlanarLayout::PLANE_V].colInc;

        for (uint32_t y = 0; y < height; y++) {
            memcpy(dstY + y * dstYStride, srcY + (size_t)y * width, width);
        }
        uint32_t chromaWidth = width / 2;
        uint32_t chromaHeight = height / 2;
        for (uint32_t y = 0; y < chromaHeight; y++) {
            const uint8_t *srcRow = srcUv + (size_t)y * width;
            uint8_t *dstURow = dstU + y * dstUStride;
            uint8_t *dstVRow = dstV + y * dstVStride;
            for (uint32_t x = 0; x < chromaWidth; x++) {
                dstURow[x * dstUColInc] = srcRow[x * 2 + 0];
                dstVRow[x * dstVColInc] = srcRow[x * 2 + 1];
            }
        }
    }
    free(frame);

    std::shared_ptr<C2Buffer> outBuffer = createGraphicBuffer(outBlock, C2Rect(width, height));

    work->worklets.front()->output.flags = (C2FrameData::flags_t)0;
    work->worklets.front()->output.buffers.clear();
    work->worklets.front()->output.buffers.push_back(outBuffer);
    work->worklets.front()->output.ordinal = work->input.ordinal;
    work->workletsProcessed = 1u;

    ALOGD("decoded %ux%u via the VA-API daemon", width, height);
}

}  // namespace android
