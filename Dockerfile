# refractir:latest — Experiment environment for RefractIR
#
# Build:
#   docker build -t refractir:latest \
#     --build-arg UID=$(id -u) \
#     --build-arg GID=$(id -g) \
#     .
#
# Run:
#   docker run -it --rm \
#     -v $(pwd):/workspace \
#     refractir:latest \
#     bash
#
# The build context is the repo root.

FROM ubuntu:24.04 AS base

ARG DEBIAN_FRONTEND=noninteractive
ARG UID=1000
ARG GID=1000

# — System packages ——————————————————————————————————————————
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential g++ cmake ninja-build pkg-config \
      m4 python3-mesonpy libmpfr6 libmpfr-dev \
      git curl wget ca-certificates \
      python3 python3-pip python3-venv \
      jq file ripgrep \
    && apt-get autoremove -y && apt-get clean -y \
    && rm -rf /var/lib/apt/lists/*

# — SMT Solvers ——————————————————————————————————————————————

# Z3
RUN git clone --depth 1 --branch z3-4.16.0 \
      https://github.com/Z3Prover/z3.git /tmp/z3-src \
    && cd /tmp/z3-src \
    && python3 scripts/mk_make.py --prefix=/usr/local \
    && cd build && make -j$(nproc) && make install \
    && rm -rf /tmp/z3-src

# CVC5
RUN git clone --depth 1 --branch cvc5-1.3.4 \
      https://github.com/cvc5/cvc5.git /tmp/cvc5-src \
    && cd /tmp/cvc5-src \
    && ./configure.sh --prefix=/usr/local --auto-download \
    && cd build && make -j$(nproc) && make install \
    && rm -rf /tmp/cvc5-src

# Bitwuzla
RUN git clone --depth 1 --branch 0.9.1 \
      https://github.com/bitwuzla/bitwuzla.git /tmp/bitwuzla-src \
    && cd /tmp/bitwuzla-src \
    && python3 ./configure.py --prefix /usr/local \
    && cd build && ninja install \
    && rm -rf /tmp/bitwuzla-src

# — Install Clang ———————————————————————————————————————————
RUN apt-get update && apt-get install -y --no-install-recommends \
      clang libclang-rt-18-dev \
    && apt-get clean && rm -rf /var/lib/apt/lists/*

# — User (matches host UID/GID) ————————————————————
RUN groupadd -g ${GID} refractir 2>/dev/null || true \
    && useradd -m -u ${UID} -g ${GID} -s /bin/bash refractir

# — Workspace —————————————————————————————————————————————
RUN mkdir -p /workspace && chown refractir:refractir /workspace
WORKDIR /workspace
USER refractir
