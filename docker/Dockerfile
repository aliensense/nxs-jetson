# Build environment for the l4t-r36.4.4 branch (JetPack 6.2.1, kernel 5.15 jammy).
# Works on arm64 hosts (Apple Silicon Docker) and x86_64 (cross toolchain).
FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential bc bison flex \
        libssl-dev libelf-dev \
        gcc-aarch64-linux-gnu g++-aarch64-linux-gnu \
        kmod rsync curl wget git ca-certificates \
        xz-utils bzip2 python3 dwarves device-tree-compiler \
    && rm -rf /var/lib/apt/lists/*

ENV CROSS_COMPILE=aarch64-linux-gnu-
WORKDIR /build
