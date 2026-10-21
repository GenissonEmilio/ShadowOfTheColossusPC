FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive LP_NUM_THREADS=2
RUN apt-get update && apt-get install -y --no-install-recommends \
    libgl1 libgl1-mesa-dri libasound2t64 libstdc++6 libx11-6 libxrandr2 libxi6 \
    libxcursor1 libxinerama1 xvfb xauth fonts-dejavu-core \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /tmp
