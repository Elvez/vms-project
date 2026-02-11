FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
      build-essential \
      meson \
      ninja-build \
      pkg-config \
      libavformat-dev \
      libavcodec-dev \
      libavutil-dev \
      libswscale-dev \
      ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY meson.build /app/meson.build
COPY src /app/src

RUN meson setup /app/build /app \
    && meson compile -C /app/build

# ENTRYPOINT ["/app/build/streamer"]
