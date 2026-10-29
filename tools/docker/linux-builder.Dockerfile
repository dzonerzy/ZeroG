# Linux builder for the ZeroG port. Release builds will later target an older
# glibc baseline (ZeroG.md, "Cosa resta dinamico a runtime"); this image pins
# the toolchain and the vcpkg revision used for the dependency sysroot.
FROM debian:trixie

RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        clang-19 lld-19 llvm-19 libc++-19-dev libc++abi-19-dev \
        cmake ninja-build make python3 perl ruby gperf unifdef pkg-config \
        autoconf automake autoconf-archive libtool \
        git ca-certificates curl zip unzip tar file \
    && rm -rf /var/lib/apt/lists/* \
    && for t in clang clang++; do ln -s /usr/bin/$t-19 /usr/local/bin/$t; done \
    && ln -s /usr/bin/clang-19 /usr/local/bin/cc \
    && ln -s /usr/bin/clang++-19 /usr/local/bin/c++ \
    && for t in ld.lld llvm-ar llvm-ranlib llvm-strip; do ln -s /usr/bin/$t-19 /usr/local/bin/$t; done

# WebKit code generators (preprocess.pl, preprocessor.pm) shell out to $CC,
# defaulting to /usr/bin/gcc, which this image does not ship.
ENV CC=clang CXX=clang++

# Same vcpkg commit as deps/vcpkg.json's builtin-baseline.
ARG VCPKG_COMMIT=0e5eb43501dab17aa055dd54bd7a8c018f330fe2
RUN git clone --filter=blob:none https://github.com/microsoft/vcpkg.git /opt/vcpkg \
    && git -C /opt/vcpkg checkout -q ${VCPKG_COMMIT} \
    && /opt/vcpkg/bootstrap-vcpkg.sh -disableMetrics
ENV ZEROG_VCPKG_ROOT=/opt/vcpkg \
    VCPKG_DEFAULT_BINARY_CACHE=/build/vcpkg-cache \
    VCPKG_DOWNLOADS=/build/vcpkg-downloads \
    ZEROG_SYSROOT=/build/sysroot

WORKDIR /src
