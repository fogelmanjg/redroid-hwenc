# Devlog

Session-by-session log — what was tried, what worked, what didn't, and why. See the [README](README.md)
for the tier-based roadmap.

## 2026-09-19 — Kickoff

Repo created. The 6-tier roadmap comes from actually investigating the state of the problem: the
official redroid-doc issues (#126, #172, #168, #535) have been open between 1 and 4 years, all
with the same answer from the maintainer ("the VA-API drivers are already there, you write the
Codec2 component") and no published result.

Important starting point that changes the difficulty calculus relative to what the community
seems to assume: redroid runs as a privileged Docker container, not a VM. That means there's no
virtio-gpu in the way at the container boundary — the kernel/DRM is literally the host's. The
Tier 3 spike (does a gralloc buffer export a dma-buf that VA-API can import zero-copy?) starts
from a better position than if redroid ran virtualized.

Next step: Tier 0 — confirm how redroid/scrcpy pick the encoder today, and which VA-API encode
entrypoints the real hardware actually supports.
