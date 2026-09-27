/*
 * Tier 7: a real C2Component for c2.hardware.decoder.h264. The decode-side
 * mirror of VaapiEncComponent.h/.cpp: wires the same host-side VA-API
 * daemon (tier5-vaapi-daemon, now also serving NVIDIA decode via
 * decode_h264.c/protocol.h's VAAPI_CMD_DECODE) into process(), this time
 * sending compressed H.264 bytes over and getting a decoded NV12 frame
 * back, instead of the other way around.
 *
 * Same SimpleC2Component base as the encoder, for the same reason noted
 * there: this daemon's synchronous request/response model fits
 * SimpleC2Component's fully-synchronous process(work, pool) contract
 * directly, without needing the async callback machinery a "real" hardware
 * decoder component (e.g. external/v4l2_codec2) is built around.
 */

#ifndef VAAPI_DEC_CODEC2_COMPONENT_H
#define VAAPI_DEC_CODEC2_COMPONENT_H

#include <SimpleC2Component.h>
#include <SimpleC2Interface.h>
#include <C2Config.h>

namespace android {

struct VaapiDecInterface : public SimpleInterface<void>::BaseParams {
    explicit VaapiDecInterface(const std::shared_ptr<C2ReflectorHelper> &helper);

    uint32_t width() const { return mSize->width; }
    uint32_t height() const { return mSize->height; }

private:
    static C2R SizeSetter(bool mayBlock, const C2P<C2StreamPictureSizeInfo::output> &oldMe,
                           C2P<C2StreamPictureSizeInfo::output> &me);
    static C2R ProfileLevelSetter(bool mayBlock, C2P<C2StreamProfileLevelInfo::input> &me);

    // Coded and output picture size are the same for this decoder (no
    // scaling/cropping stage) - same simplification C2SoftAvcDec's own
    // IntfImpl makes, just without its dynamic-resolution-change machinery
    // (this daemon's decode_h264.c handles that server-side, in
    // decode_ensure_resolution(); the component itself just reports
    // whatever width/height decode_h264_frame() hands back).
    std::shared_ptr<C2StreamPictureSizeInfo::output> mSize;
    std::shared_ptr<C2StreamProfileLevelInfo::input> mProfileLevel;
};

class VaapiDecComponent : public SimpleC2Component {
public:
    VaapiDecComponent(const char *name, c2_node_id_t id,
                       const std::shared_ptr<VaapiDecInterface> &intf);
    ~VaapiDecComponent() override = default;

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
    // Sends VAAPI_CMD_DECODE + a DecodeRequest header + the raw compressed
    // bytes to the daemon (plain stream writes, no SCM_RIGHTS - see
    // protocol.h's own top comment for why decode's wire shape is simpler
    // than encode's), and receives back a malloc'd tightly-packed NV12
    // frame (caller frees) plus the width/height decode_h264.c read out of
    // the stream's own SPS (authoritative - may differ from the
    // width/height hints this function sent, if the interface's current
    // picture size param is stale). Returns 0 on success, negative on
    // failure.
    int decodeViaDaemon(uint32_t widthHint, uint32_t heightHint, const uint8_t *bitstream,
                         size_t bitstreamSize, uint8_t **outData, size_t *outSize,
                         uint32_t *outWidth, uint32_t *outHeight);

    std::shared_ptr<VaapiDecInterface> mIntf;
};

}  // namespace android

#endif
