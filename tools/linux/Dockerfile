FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential clang lld cmake ninja-build ccache git ca-certificates python3 \
    pkg-config libarchive-tools libavcodec-dev libavformat-dev libavutil-dev \
    libswresample-dev libswscale-dev libasound2-dev libx11-dev libxrandr-dev \
    libxi-dev libxcursor-dev libxinerama-dev libgl1-mesa-dev xvfb xauth \
    && rm -rf /var/lib/apt/lists/*
ENV CC=clang CXX=clang++ CMAKE_BUILD_PARALLEL_LEVEL=2 LP_NUM_THREADS=2
WORKDIR /src
