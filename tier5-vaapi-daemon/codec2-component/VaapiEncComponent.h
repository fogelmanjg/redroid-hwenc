/*
 * Tier 5.5: a real C2Component for c2.hardware.encoder.h264. Wires the
 * host-side VA-API encode daemon (tier5-vaapi-daemon, proven end to end in
 * Tiers 5.2-5.4) into process(), replacing the Tier 4 skeleton's
 * createComponent() stub that always returned C2_NOT_FOUND.
 *
 * Structural reference: external/v4l2_codec2/components/EncodeComponent.cpp
 * (Tier 1's mapping) shows the *shape* of a real Codec2 encoder component,
 * but reusing it verbatim would pull in its whole async VideoEncoder
 * callback machinery (built for V4L2's interrupt-driven hardware) plus a
 * large dependency closure (libchrome, its own VideoFramePool, etc.) that
 * doesn't fit our daemon's simple synchronous request/response model. This
 * is a purpose-built SimpleC2Component instead -- frameworks/av's own
 * software encoders (e.g. C2SoftAvcEnc) use exactly this simpler base
 * class, and its fully-synchronous process(work, pool) contract matches a
 * blocking round trip to the daemon perfectly.
 */

#ifndef VAAPI_CODEC2_COMPONENT_H
#define VAAPI_CODEC2_COMPONENT_H

#include <SimpleC2Component.h>
#include <SimpleC2Interface.h>
#include <C2Config.h>

namespace android {

struct VaapiEncInterface : public SimpleInterface<void>::BaseParams {
    explicit VaapiEncInterface(const std::shared_ptr<C2ReflectorHelper> &helper);

    uint32_t width() const { return mSize->width; }
    uint32_t height() const { return mSize->height; }

private:
    static C2R SizeSetter(bool mayBlock, const C2P<C2StreamPictureSizeInfo::input> &oldMe,
                           C2P<C2StreamPictureSizeInfo::input> &me);

    std::shared_ptr<C2StreamPictureSizeInfo::input> mSize;
};

class VaapiEncComponent : public SimpleC2Component {
public:
    VaapiEncComponent(const char *name, c2_node_id_t id,
                       const std::shared_ptr<VaapiEncInterface> &intf);
    ~VaapiEncComponent() override = default;

    // SimpleC2Component
    c2_status_t onInit() override;
    c2_status_t onStop() override;
    void onReset() override;
    void onRelease() override;
    c2_status_t onFlush_sm() override;
    void process(const std::unique_ptr<C2Work> &work,
                 const std::shared_ptr<C2BlockPool> &pool) override;
    c2_status_t drain(uint32_t drainMode, const std::shared_ptr<C2BlockPool> &pool) override;

private:
    // Sends the dma-buf fd (via SCM_RIGHTS) + frame params to the daemon over
    // its Unix socket and returns the malloc'd Annex-B H.264 bytes (caller
    // frees), or a negative value on any failure. Same protocol as
    // tier5-vaapi-daemon/protocol.h and every test client built against it.
    long encodeViaDaemon(int dmabufFd, uint32_t width, uint32_t height, uint32_t strideY,
                          uint32_t strideUv, uint32_t offsetUv, uint32_t dmabufSize,
                          unsigned char **outBuf);

    std::shared_ptr<VaapiEncInterface> mIntf;
};

}  // namespace android

#endif
