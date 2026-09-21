/*
 * Tier 5.6 diagnostic: calls the exact client-side API Codec2InfoBuilder
 * (the code MediaCodecList uses to build the list scrcpy/apps see) uses --
 * Codec2Client::CreateInterfaceByName() -- directly, to find out whether it
 * succeeds or fails for our component specifically. Deliberately a *system*
 * binary (not vendor), matching where the real caller (libstagefright)
 * lives, to rule out any cross-partition client library restriction.
 */
#include <stdio.h>
#include <codec2/hidl/client.h>
#include <C2Config.h>

int main() {
    printf("Calling Codec2Client::CreateInterfaceByName(\"c2.hardware.encoder.h264\")...\n");
    std::shared_ptr<android::Codec2Client> owner;
    auto intf = android::Codec2Client::CreateInterfaceByName("c2.hardware.encoder.h264", &owner);
    if (!intf) {
        fprintf(stderr, "FAILED: CreateInterfaceByName returned null\n");
    } else {
        printf("SUCCESS: got an interface, name=%s\n", intf->getName().c_str());

        C2StreamUsageTuning::input usage(0u, 0u);
        std::vector<std::unique_ptr<C2Param>> heapParams;
        c2_status_t err = intf->query({&usage}, {}, C2_DONT_BLOCK, &heapParams);
        printf("query(C2StreamUsageTuning::input) -> err=%d usage.value=%llu\n",
               err, (unsigned long long)usage.value);
    }

    printf("\nCalling Codec2Client::ListComponents()...\n");
    const auto &traits = android::Codec2Client::ListComponents();
    printf("ListComponents() returned %zu traits:\n", traits.size());
    for (const auto &t : traits) {
        printf("  name=%s domain=%u kind=%u mediaType=%s owner=%s\n",
               t.name.c_str(), t.domain, t.kind, t.mediaType.c_str(), t.owner.c_str());
    }
    return intf ? 0 : 1;
}
