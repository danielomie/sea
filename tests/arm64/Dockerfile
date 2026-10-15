# Ubuntu arm64 image for building and testing Sea on aarch64 (see run.sh).
FROM ubuntu:24.04
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    build-essential libreadline-dev libssl-dev zlib1g-dev libgccjit-13-dev ca-certificates \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /build
