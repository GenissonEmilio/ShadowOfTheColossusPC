# Launcher portability probe: no system SDL2 or Wayland server libraries.
# Keep the ordinary X11/OpenGL driver baseline; the frozen app supplies GTK/SDL.
FROM ubuntu:22.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    python3 libgl1 libdrm2 libx11-6 xvfb xauth fonts-dejavu-core \
    && rm -rf /var/lib/apt/lists/*
RUN if ldconfig -p | grep -Eq 'libSDL2|libwayland-server'; then \
      echo 'Clean launcher test must not inherit SDL2/Wayland server from the host' >&2; exit 1; fi
WORKDIR /src
