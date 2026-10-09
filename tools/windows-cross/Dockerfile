# Cross-compiles the Windows build from Linux: clang-20 targeting
# x86_64-pc-windows-msvc, with the MSVC CRT and Windows SDK fetched by xwin.
FROM ubuntu:24.04

ARG XWIN_VERSION=0.10.0
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates curl gnupg lsb-release software-properties-common \
        cmake ninja-build python3 git zip \
    && curl -fsSL https://apt.llvm.org/llvm.sh -o /tmp/llvm.sh \
    && bash /tmp/llvm.sh 20 \
    && apt-get install -y --no-install-recommends lld-20 llvm-20 \
    && rm -rf /var/lib/apt/lists/* /tmp/llvm.sh

RUN arch="$(uname -m)" \
    && curl -fsSL "https://github.com/Jake-Shadle/xwin/releases/download/${XWIN_VERSION}/xwin-${XWIN_VERSION}-${arch}-unknown-linux-musl.tar.gz" \
        | tar -xz -C /usr/local/bin --strip-components=1 "xwin-${XWIN_VERSION}-${arch}-unknown-linux-musl/xwin" \
    && xwin --accept-license --arch x86_64 splat --output /xwin \
    && rm -rf ~/.xwin-cache .xwin-cache

COPY toolchain.cmake /opt/windows-cross/toolchain.cmake

ENV INCLUDE=/xwin/crt/include;/xwin/sdk/include/ucrt;/xwin/sdk/include/um;/xwin/sdk/include/shared;/xwin/sdk/include/winrt
ENV LIB=/xwin/crt/lib/x86_64;/xwin/sdk/lib/um/x86_64;/xwin/sdk/lib/ucrt/x86_64
