# Linux builder for the ZeroG port (phase 0: JSCOnly, static WTF + JSC).
# Release builds will later target an older glibc baseline (ZeroG.md,
# "Cosa resta dinamico a runtime"); this image only pins a known toolchain.
FROM debian:trixie

RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        clang-19 lld-19 llvm-19 libc++-19-dev libc++abi-19-dev \
        cmake ninja-build python3 perl ruby gperf unifdef pkg-config \
        libicu-dev git ca-certificates file \
    && rm -rf /var/lib/apt/lists/* \
    && ln -s /usr/bin/clang-19 /usr/local/bin/clang \
    && ln -s /usr/bin/clang++-19 /usr/local/bin/clang++ \
    && ln -s /usr/bin/ld.lld-19 /usr/local/bin/ld.lld

# WebKit code generators (preprocess.pl, preprocessor.pm) shell out to $CC,
# defaulting to /usr/bin/gcc, which this image does not ship.
ENV CC=clang CXX=clang++

WORKDIR /src
