# Debian Bookworm (arm64) verification image for amilisp.
#
# Deliberately minimal: amilisp's host build needs gcc/make/libc and
# libffi (platform_posix.c).  ca-certificates/curl for scripts that
# fetch (e.g. quicklisp-based integration runs), tzdata for local-time.
FROM debian:bookworm

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        libffi-dev \
        ca-certificates \
        tzdata \
        curl \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /work
