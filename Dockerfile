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
      curl \
      gnupg \
    && curl -fsSL https://deb.nodesource.com/setup_18.x | bash - \
    && apt-get install -y --no-install-recommends \
      nodejs \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY meson.build /app/meson.build
COPY src /app/src
COPY frontend /app/frontend

RUN meson setup /app/build /app \
    && meson compile -C /app/build

EXPOSE 5173
EXPOSE 5173

# ENTRYPOINT ["/app/build/streamer"]
