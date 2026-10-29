# Build environment for the Switch 2 (HBLauncher) package (hblauncher/docker/run.sh): arm64 Ubuntu with Clang 22
# (the version upstream WiiCompiled ships), .NET 8 for the translator (it emits host-format
# assembly, so Linux builds translate on Linux), plus what musl-cross-make and Dawn need.
# libvulkan-dev: HBLauncher's hbxl-build.py takes the (architecture-neutral) Vulkan headers
# from /usr/include.
# HBLauncher's own build (run.sh launcher -> tools/build-all.sh): Pillow and DejaVu for the font
# atlas, glslang and SPIR-V Tools for the UI shaders, Node.js for its host tests. Its AArch64
# compiler and binutils are the native ones on arm64 (aarch64-linux-gnu-gcc).
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential bison flex texinfo gawk file wget curl ca-certificates gnupg \
        git python3 cmake ninja-build pkg-config xz-utils bzip2 rsync patch unzip \
        libicu74 libvulkan-dev \
        python3-pil fonts-dejavu-core glslang-tools spirv-tools nodejs \
    && rm -rf /var/lib/apt/lists/*

# Clang 22 from LLVM's apt repository.
RUN curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key | gpg --dearmor -o /usr/share/keyrings/llvm.gpg \
    && echo "deb [signed-by=/usr/share/keyrings/llvm.gpg] http://apt.llvm.org/noble/ llvm-toolchain-noble-22 main" \
        > /etc/apt/sources.list.d/llvm.list \
    && apt-get update && apt-get install -y --no-install-recommends clang-22 lld-22 llvm-22 \
    && rm -rf /var/lib/apt/lists/* \
    && ln -s /usr/bin/clang-22 /usr/local/bin/clang \
    && ln -s /usr/bin/clang++-22 /usr/local/bin/clang++ \
    && ln -s /usr/bin/ld.lld-22 /usr/local/bin/ld.lld

# .NET 8 SDK (8.0.4xx feature band, as on the Mac): Ubuntu's 8.0.1xx compiler is too old for the
# translator's Tomlyn source generator. Installed with Microsoft's official dotnet-install script.
RUN curl -fsSL https://dot.net/v1/dotnet-install.sh -o /tmp/dotnet-install.sh \
    && bash /tmp/dotnet-install.sh --channel 8.0 --install-dir /usr/share/dotnet \
    && ln -s /usr/share/dotnet/dotnet /usr/local/bin/dotnet && rm /tmp/dotnet-install.sh
ENV DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1

WORKDIR /opt/work
