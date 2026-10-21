FROM ubuntu:22.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    python3 python3-dev python3-pip python3-gi python3-gi-cairo gir1.2-gtk-3.0 \
    libarchive13 python3-pil binutils gcc libglib2.0-bin fonts-dejavu-core \
    libgl1 xvfb xauth patchelf ca-certificates \
    && rm -rf /var/lib/apt/lists/*
RUN python3 -m pip install --no-cache-dir pyinstaller==6.22.0
RUN apt-get update && apt-get install -y --no-install-recommends \
    git make zlib1g-dev liblzma-dev libbz2-dev libzstd-dev && rm -rf /var/lib/apt/lists/*
# The distro's older RAR decoder fails on the supplied nested CloneCD RAR.
# Pin and verify the modern official decoder instead of relying on host packages.
RUN python3 -c 'import urllib.request,hashlib; p=urllib.request.urlopen("https://github.com/libarchive/libarchive/releases/download/v3.8.10/libarchive-3.8.10.tar.xz").read(); assert hashlib.sha256(p).hexdigest()=="56cad765459d5ee25a3b761f90c47d99379d28297fbfb72d908880d6b875fb96"; open("/tmp/libarchive.tar.xz","wb").write(p)'
RUN python3 -c 'import tarfile; tarfile.open("/tmp/libarchive.tar.xz").extractall("/tmp")' && cd /tmp/libarchive-3.8.10 \
    && ./configure --prefix=/opt/libarchive --disable-static --without-openssl --without-nettle \
       --without-xml2 --without-expat --without-libb2 --disable-bsdtar --disable-bsdcpio --disable-bsdcat --disable-bsdunzip \
    && make -j2 && make install \
    && cp COPYING /opt/libarchive/COPYING && rm -rf /tmp/libarchive*
RUN apt-get update && apt-get install -y --no-install-recommends pkg-config && rm -rf /var/lib/apt/lists/*
WORKDIR /src
