FROM debian:trixie-slim@sha256:a72b4433de1cf7d6bae0c4cee74d0547f58601a643b3375f070c51600d63b6b2 AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential pkg-config libva-dev libgbm-dev libegl-dev libdrm-dev \
    libvulkan-dev libavcodec-dev libavutil-dev ca-certificates \
    && rm -rf /var/lib/apt/lists/*
WORKDIR build
COPY tier5-vaapi-daemon/ ./
COPY LICENSE ./LICENSE
RUN make HWDEC=1 daemon

FROM debian:trixie-slim@sha256:a72b4433de1cf7d6bae0c4cee74d0547f58601a643b3375f070c51600d63b6b2
RUN apt-get update && apt-get install -y --no-install-recommends \
    libva2 libva-drm2 libgbm1 libegl1 libavcodec61 libavutil59 \
    i965-va-driver vainfo ffmpeg \
    && rm -rf /var/lib/apt/lists/*
WORKDIR app
COPY --from=build /build/daemon ./daemon
COPY --from=build /build/LICENSE ./LICENSE
RUN mkdir run
ENV LIBVA_DRIVER_NAME=i965 \
    REDROID_FORGE_VAAPI_SOCKET=run/socket \
    REDROID_FORGE_ENCODE_BACKEND=ffmpeg \
    REDROID_FORGE_ENCODE_IDR_PERIOD=60 \
    REDROID_VAAPI_LEGACY_PRIME=1 \
    REDROID_VAAPI_FULL_RANGE=1
CMD ["./daemon"]
