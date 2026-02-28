# ============================================================
# Asterisk + DeepFilterNet — multi-stage build
# Stage 1: Build Asterisk, DeepFilterNet libdf, and our module
# Stage 2: Minimal runtime image
# ============================================================
FROM debian:bookworm-slim AS builder

ENV DEBIAN_FRONTEND=noninteractive

# -- System build dependencies --
RUN apt-get update && apt-get install -y \
    build-essential \
    git \
    curl \
    wget \
    pkg-config \
    # Asterisk build deps
    libjansson-dev \
    libxml2-dev \
    libsqlite3-dev \
    libssl-dev \
    libedit-dev \
    uuid-dev \
    libspeex-dev \
    libspeexdsp-dev \
    libcurl4-openssl-dev \
    # Build tools
    autoconf \
    automake \
    libtool \
    && rm -rf /var/lib/apt/lists/*

# -- Install Rust toolchain + cargo-c (for C API generation) --
RUN curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | \
    sh -s -- -y --default-toolchain stable
ENV PATH="/root/.cargo/bin:${PATH}"
RUN cargo install cargo-c

# -- Build Asterisk from source --
WORKDIR /usr/src
RUN wget -q https://downloads.asterisk.org/pub/telephony/asterisk/asterisk-22-current.tar.gz \
    && tar xzf asterisk-22-current.tar.gz \
    && rm asterisk-22-current.tar.gz \
    && mv asterisk-22.* asterisk

WORKDIR /usr/src/asterisk
RUN ./configure --with-jansson-bundled --with-pjproject-bundled \
    && make menuselect.makeopts \
    && menuselect/menuselect \
        --enable func_speex \
        --enable codec_speex \
        --enable res_rtp_asterisk \
        menuselect.makeopts \
    && make -j$(nproc) \
    && make install \
    && make install-headers \
    && make samples

# -- Clone and build DeepFilterNet C API (libdeepfilter) --
WORKDIR /usr/src
RUN git clone --depth 1 https://github.com/Rikorose/DeepFilterNet.git

WORKDIR /usr/src/DeepFilterNet
RUN cargo cinstall --package deep_filter --release \
    --features capi \
    --prefix /usr/local \
    && ldconfig

# -- Download pretrained DeepFilterNet3 ONNX model --
RUN mkdir -p /usr/share/asterisk/deepfilter && \
    wget -q -O /usr/share/asterisk/deepfilter/DeepFilterNet3.tar.gz \
    "https://github.com/Rikorose/DeepFilterNet/raw/refs/heads/main/models/DeepFilterNet3_onnx.tar.gz"

# -- Build our Asterisk module --
COPY module/ /usr/src/func_deepfilter/
WORKDIR /usr/src/func_deepfilter

# Debug: show installed files and exported symbols
RUN echo "=== Installed deepfilter files ===" && \
    find /usr/local -name "*deep_filter*" -o -name "*deepfilter*" 2>/dev/null && \
    echo "=== Exported symbols ===" && \
    nm -D /usr/local/lib/aarch64-linux-gnu/libdeepfilter.so 2>/dev/null | grep " T " | head -30 && \
    echo "=== deep_filter.h contents ===" && \
    cat /usr/local/include/deep_filter/deep_filter.h 2>/dev/null | head -80

RUN gcc -shared -fPIC -o func_deepfilter.so func_deepfilter.c \
    -I/usr/local/include \
    -L/usr/local/lib/aarch64-linux-gnu \
    -Wl,-rpath,/usr/local/lib/aarch64-linux-gnu \
    -lspeexdsp \
    -ldeepfilter \
    -lpthread -lm \
    -Wall -O2 \
    -DAST_MODULE_SELF_SYM=__internal_func_deepfilter_self \
    && cp func_deepfilter.so /usr/lib/asterisk/modules/

# ============================================================
# Stage 2: Runtime image
# ============================================================
FROM debian:bookworm-slim

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    libjansson4 \
    libxml2 \
    libsqlite3-0 \
    libssl3 \
    libedit2 \
    uuid-runtime \
    libspeex1 \
    libspeexdsp1 \
    libcurl4 \
    # Debugging/diagnostics tools
    tcpdump \
    net-tools \
    iputils-ping \
    sngrep \
    && rm -rf /var/lib/apt/lists/*

# Copy Asterisk binaries and libraries
COPY --from=builder /usr/sbin/asterisk /usr/sbin/
COPY --from=builder /usr/lib/libasterisk* /usr/lib/
COPY --from=builder /usr/lib/asterisk/ /usr/lib/asterisk/
COPY --from=builder /var/lib/asterisk/ /var/lib/asterisk/
COPY --from=builder /var/spool/asterisk/ /var/spool/asterisk/
COPY --from=builder /var/log/asterisk/ /var/log/asterisk/
COPY --from=builder /etc/asterisk/ /etc/asterisk/

# Copy DeepFilterNet runtime (library + model)
COPY --from=builder /usr/local/lib/aarch64-linux-gnu/libdeepfilter* /usr/local/lib/aarch64-linux-gnu/
COPY --from=builder /usr/share/asterisk/deepfilter/ /usr/share/asterisk/deepfilter/
RUN echo "/usr/local/lib/aarch64-linux-gnu" > /etc/ld.so.conf.d/deepfilter.conf && \
    ldconfig

# Create asterisk user
RUN groupadd -r asterisk && useradd -r -g asterisk asterisk && \
    mkdir -p /var/run/asterisk && \
    chown -R asterisk:asterisk /var/lib/asterisk /var/spool/asterisk \
    /var/log/asterisk /var/run/asterisk /etc/asterisk

# SIP signaling + RTP media ports
EXPOSE 5060/udp 5060/tcp
EXPOSE 10000-10020/udp

CMD ["asterisk", "-f", "-vvv"]
