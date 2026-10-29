# Linux builder for the ZeroG port. Everything (WebKit, vcpkg dependencies, the
# final link) is compiled with zig's clang against a glibc 2.28 baseline and
# zig's bundled libc++, so binaries run on any distribution with glibc >= 2.28
# (ZeroG.md, "Cosa resta dinamico a runtime"). A newer system libstdc++ cannot
# be used: its static archive references glibc symbols newer than 2.28.
FROM debian:trixie

RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        cmake ninja-build make python3 perl ruby gperf unifdef pkg-config \
        autoconf automake autoconf-archive libtool \
        git ca-certificates curl zip unzip tar xz-utils file binutils \
    && rm -rf /var/lib/apt/lists/*

# Versions come from toolchain.json via `python tools/toolchain.py docker-build`;
# there are no defaults, so the image cannot drift from the manifest.
ARG ZIG_VERSION
ARG ZIG_SHA256
ARG ZEROG_ZIG_TARGET
ARG VCPKG_COMMIT
RUN test -n "$ZIG_VERSION" -a -n "$ZIG_SHA256" -a -n "$ZEROG_ZIG_TARGET" -a -n "$VCPKG_COMMIT" \
    || { echo "build with: python tools/toolchain.py docker-build"; exit 1; }

RUN curl -fsSL -o /tmp/zig.tar.xz https://ziglang.org/download/${ZIG_VERSION}/zig-x86_64-linux-${ZIG_VERSION}.tar.xz \
    && echo "${ZIG_SHA256}  /tmp/zig.tar.xz" | sha256sum -c - \
    && mkdir /opt/zig && tar xJf /tmp/zig.tar.xz -C /opt/zig --strip-components=1 \
    && rm /tmp/zig.tar.xz \
    && ln -s /opt/zig/zig /usr/local/bin/zig

# Compiler wrappers: CMake, vcpkg and autotools see ordinary cc/c++/ar/ranlib.
COPY zcc zc++ zar zranlib /usr/local/bin/
RUN chmod +x /usr/local/bin/zcc /usr/local/bin/zc++ /usr/local/bin/zar /usr/local/bin/zranlib
ENV ZEROG_ZIG_TARGET=${ZEROG_ZIG_TARGET} \
    CC=zcc CXX=zc++ AR=zar RANLIB=zranlib \
    ZIG_GLOBAL_CACHE_DIR=/build/zig-cache

RUN git clone --filter=blob:none https://github.com/microsoft/vcpkg.git /opt/vcpkg \
    && git -C /opt/vcpkg checkout -q ${VCPKG_COMMIT} \
    && ZIG_GLOBAL_CACHE_DIR=/tmp/zig-cache /opt/vcpkg/bootstrap-vcpkg.sh -disableMetrics
ENV ZEROG_VCPKG_ROOT=/opt/vcpkg \
    VCPKG_DEFAULT_BINARY_CACHE=/build/vcpkg-cache \
    VCPKG_DOWNLOADS=/build/vcpkg-downloads \
    VCPKG_KEEP_ENV_VARS=ZEROG_ZIG_TARGET;ZIG_GLOBAL_CACHE_DIR \
    ZEROG_SYSROOT=/build/sysroot

WORKDIR /src
