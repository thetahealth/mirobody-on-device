# syntax=docker/dockerfile:1

# ==============================================================================
# Mirobody server image for Kubernetes.
#
# Multi-stage build:
#   1. builder  — compiles the standalone `mirobody` executable from the system
#                 package set (vcpkg is Windows-only; Linux uses find_package).
#   2. runtime  — debian-slim with ONLY the shared libraries the binary actually
#                 links (extracted via ldd) plus CA certificates for outbound TLS.
#
# Build:
#   docker build -t mirobody:latest .
#   docker build --build-arg MIROBODY_DATABASE_BACKEND=POSTGRESQL_LEGACY -t mirobody:legacy .
#
# Run:
#   docker run -p 80:80 -v $PWD/config.yml:/app/config.yml mirobody:latest
#
# The image bakes config.example.yml (the defaults layer). Real settings come
# from remote config / env vars in managed environments, or from a config.yml you
# mount at /app (as above) for self-hosting. Precedence (high -> low): env vars >
# config.yml > remote config > the baked config.example.yml. In managed
# environments set ENV=PROD (or TEST/GRAY) to skip the in-process schema
# migration — see src/main.cpp.
# ==============================================================================

# ------------------------------------------------------------------------------
# Stage 1: builder
# ------------------------------------------------------------------------------
# Ubuntu 24.04 LTS provides modern hiredis / yaml-cpp / libpq / image codecs.
# libwebsockets is built from source below instead of the apt package: Ubuntu's
# libwebsockets-dev is compiled WITHOUT LWS_WITH_HTTP_STREAM_COMPRESSION, so it
# would serve every response uncompressed (verified: lws_config.h #undef's it).
FROM ubuntu:24.04 AS builder

# SQL backend linked into the binary. POSTGRESQL is the CMake default for server
# builds; both PG variants use the libpq client installed below (use
# POSTGRESQL_LEGACY for the legacy res/sql/pg_legacy schema). Pick MYSQL/DUCKDB/
# etc. only after adding the matching -dev package to the apt line.
ARG MIROBODY_DATABASE_BACKEND=POSTGRESQL

# Core build toolchain + dev headers for every dependency in CMakeLists.txt.
# libpq-dev provides the PostgreSQL client; libopenblas-dev is optional (enables
# MIROBODY_HAS_BLAS) and pulled in here so the embedding code can use it.
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        ninja-build \
        pkg-config \
        ca-certificates \
        curl \
        git \
        unzip \
        libcurl4-openssl-dev \
        libssl-dev \
        zlib1g-dev \
        rapidjson-dev \
        libyaml-cpp-dev \
        libhiredis-dev \
        libpq-dev \
        libopenblas-dev \
        libjpeg-dev \
        libpng-dev \
        libtiff-dev \
        libwebp-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src

# sentry-native is not packaged for Ubuntu (vcpkg supplies it on Windows), so
# build it from the official release bundle, which vendors crashpad, and install
# into /usr/local so the project's `find_package(sentry CONFIG REQUIRED)`
# resolves it. The crashpad backend links the curl/ssl/zlib dev set from the apt
# line above and builds the crashpad_handler the runtime stage ships next to the
# binary. Upstream exports the crashpad_handler CMake target but has no install()
# rule for the executable, so cmake --install does not place it on a stable path;
# we copy it out of the build tree to /usr/local/bin before removing the tree.
ARG SENTRY_NATIVE_VERSION=0.14.2
RUN curl -fsSL -o /tmp/sentry-native.zip \
        "https://github.com/getsentry/sentry-native/releases/download/${SENTRY_NATIVE_VERSION}/sentry-native.zip" \
    && mkdir -p /tmp/sentry-native \
    && unzip -q /tmp/sentry-native.zip -d /tmp/sentry-native \
    && cmake -S /tmp/sentry-native -B /tmp/sentry-native/build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DSENTRY_BACKEND=crashpad \
        -DSENTRY_BUILD_TESTS=OFF \
        -DSENTRY_BUILD_EXAMPLES=OFF \
    && cmake --build /tmp/sentry-native/build --parallel "$(nproc)" \
    && cmake --install /tmp/sentry-native/build \
    && ldconfig \
    && cp "$(find /tmp/sentry-native/build -type f -name crashpad_handler | head -n1)" \
          /usr/local/bin/crashpad_handler \
    && rm -rf /tmp/sentry-native /tmp/sentry-native.zip

# xlnt gives the document transcoder its .xlsx reader. It is not packaged for
# Ubuntu (vcpkg supplies it on Windows), so build it from the xlnt-community
# release (the C++11-clean fork) and install into /usr/local where the project's
# `find_package(xlnt CONFIG)` resolves it and defines MIROBODY_ENABLE_XLSX. Built
# STATIC so it links into the binary and the runtime stage needs no libxlnt .so.
# Cloned --recurse-submodules because xlnt's third-party deps (libstudxml, utfcpp)
# are git submodules NOT included in GitHub's release tarball. Without this step
# the build still succeeds, but .xlsx degrades to a runtime "not built" error
# (CSV and the gated PDF path are unaffected).
ARG XLNT_VERSION=1.6.1
# NB: no --shallow-submodules -- the submodules are pinned to specific commits a
# shallow fetch cannot request ("unadvertised object"); a full submodule clone
# (they are tiny) resolves the pinned SHAs.
RUN git clone --depth 1 --branch "v${XLNT_VERSION}" --recurse-submodules \
        https://github.com/xlnt-community/xlnt.git /tmp/xlnt \
    && cmake -S /tmp/xlnt -B /tmp/xlnt/build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DSTATIC=ON \
        -DTESTS=OFF \
        -DSAMPLES=OFF \
        -DBENCHMARKS=OFF \
    && cmake --build /tmp/xlnt/build --parallel "$(nproc)" \
    && cmake --install /tmp/xlnt/build \
    && ldconfig \
    && rm -rf /tmp/xlnt

# libwebsockets from source, WITH http stream compression. Ubuntu's
# libwebsockets-dev ships with LWS_WITH_HTTP_STREAM_COMPRESSION #undef'd, so it
# would gzip/deflate NOTHING -- every static asset (assets/index.js is ~520 KB)
# and API response would go out uncompressed. Building it here with the flag (and
# LWS_WITH_ZLIB, satisfied by zlib1g-dev above) makes the server deflate responses
# in prod, matching the dev (vcpkg) build which has it on. Installed to /usr/local
# so find_package(libwebsockets CONFIG) resolves this build and links its
# `websockets_shared` target (see CMakeLists.txt); the .so flows through the ldd
# step into the runtime image. Pinned to a 4.3.x tag: matches the API the code is
# written to (needs >= 4.1 for LWS_PROTOCOL_LIST_TERM; 4.0 fails to compile
# websocket_client). Must be >= 4.3.4: CVE-2025-1866 is an out-of-bounds pointer
# bug in the stream-compression path (the flag we enable here), fixed in 4.3.4;
# v4.3.10 is the current 4.3 patch tag, well past it. (The bug is Win32-only, so
# this Linux image wouldn't hit it regardless, but pin a patched tag anyway.)
ARG LWS_REF=v4.3.10
RUN git clone --depth 1 --branch "${LWS_REF}" \
        https://github.com/warmcat/libwebsockets.git /tmp/lws \
    && cmake -S /tmp/lws -B /tmp/lws/build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DLWS_WITH_HTTP_STREAM_COMPRESSION=ON \
        -DLWS_WITH_ZLIB=ON \
        -DLWS_WITHOUT_TESTAPPS=ON \
    && cmake --build /tmp/lws/build --parallel "$(nproc)" \
    && cmake --install /tmp/lws/build \
    && ldconfig \
    && rm -rf /tmp/lws

# Optional document formats are OFF by default to keep this server image lean.
# To enable them, add the deps and the matching CMake flags below:
#   PDF  (-DMIROBODY_ENABLE_PDF=ON): download a prebuilt no-V8 PDFium release
#        (https://github.com/bblanchon/pdfium-binaries), unpack it, and add
#        -DCMAKE_PREFIX_PATH=<pdfium-dir> to the configure step.
#   OCR  (-DMIROBODY_ENABLE_OCR=ON, implies PDF): apt-get install
#        tesseract-ocr libtesseract-dev libleptonica-dev tesseract-ocr-eng, and
#        copy the *.traineddata into res/ (and set Options::ocr_datapath to it).
#   .xls (-DMIROBODY_ENABLE_XLS=ON): vendor libxls (no apt package).
# The OCR/PDF runtime .so deps would then flow through the ldd step below.

# Only the inputs the executable target needs. The MCP tools and agents under
# res/ are globbed into the binary at configure time, so res/ must be present
# before cmake runs. Tests (need catch2) and the CLI/shared/python/JNI targets
# are switched off below, so their sources are not required.
COPY CMakeLists.txt ./
COPY src/ ./src/
COPY res/ ./res/

# Configure + build just the `mirobody` executable in Release.
RUN cmake -S . -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DMIROBODY_DATABASE_BACKEND=${MIROBODY_DATABASE_BACKEND} \
        -DMIROBODY_BUILD_TESTS=OFF \
        -DMIROBODY_BUILD_TOOLS=OFF \
        -DMIROBODY_BUILD_SHARED=OFF \
        -DMIROBODY_BUILD_PYTHON=OFF \
        -DMIROBODY_BUILD_JNI=OFF \
    && cmake --build build --target mirobody -j "$(nproc)"

# Collect exactly the shared libraries the binary links into a single flat /deps
# directory (a version-agnostic set, so the runtime stage needn't guess Ubuntu's
# soname-versioned / t64-renamed runtime package names, e.g. libwebsockets19).
# cp -L dereferences each soname symlink to its real file but keeps the soname as
# the name, so ldconfig can resolve it in the runtime stage. Flattening (no
# --parents) avoids re-creating /lib as a real dir, which would clash with the
# runtime image's usrmerge symlink (/lib -> /usr/lib).
# Scan both the server binary AND the crashpad_handler — the handler is a
# separate process spawned at runtime and links its own curl/ssl set, so its
# libraries must be present in the runtime image too.
RUN mkdir -p /deps \
    && ldd build/mirobody /usr/local/bin/crashpad_handler \
        | awk 'NF>=3 && $2=="=>" {print $3} NF==2 {print $1}' \
        | grep '^/' | sort -u \
        | xargs -I '{}' cp -L -v '{}' /deps/

# ------------------------------------------------------------------------------
# Stage 2: runtime
# ------------------------------------------------------------------------------
# Same base as the builder so the glibc the ldd-copied .so files were linked
# against matches at runtime.
FROM ubuntu:24.04 AS runtime

# CA certificates for outbound HTTPS (LLM providers, storage, vendor APIs), and
# nodejs for Tanka signer auto-discovery: the server runs res/tanka/discover.cjs
# at boot + on a timer to find/verify Tanka's current signer build (see
# src/user/tanka.cpp). node uses only built-in modules here (no npm/packages).
# Tanka login is ON by default; if you don't use it, this is dead weight — drop
# nodejs and set TANKA_WASM_AUTODISCOVER=false (the server then serves the pinned
# signer build), or TANKA_LOGIN_ENABLED=false to turn the feature off entirely.
# Everything else the binary needs is the .so set copied from the builder.
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates \
        nodejs \
    && rm -rf /var/lib/apt/lists/*

# Shared libraries the binary links, dropped onto the default loader search path
# (same base as builder -> glibc-compatible). Into /usr/local/lib rather than
# overlaying / so we don't collide with Ubuntu's usrmerge symlinks; ldconfig then
# builds the soname symlinks and cache. The binary's ELF interpreter (ld-linux)
# is resolved from this base image, so a copy of it in here is just unused.
COPY --from=builder /deps/ /usr/local/lib/
RUN ldconfig

# Unprivileged user for the container (good hygiene under Kubernetes; pair with
# a restrictive securityContext in the Pod spec).
RUN groupadd --system --gid 10001 mirobody \
    && useradd --system --uid 10001 --gid mirobody --no-create-home mirobody

# Sentry's crashpad backend needs a writable database directory. The default
# (.sentry-native, relative to the /app working dir) is not writable by the
# unprivileged user, so sentry_init fails with "failed to create database
# directory". Point it under /tmp, the conventional writable scratch location
# (commonly a tmpfs/emptyDir, so it stays writable even under a read-only root
# filesystem). The crash DB is ephemeral, so losing it on restart is fine.
RUN install -d -o mirobody -g mirobody /tmp/mirobody/sentry
ENV SENTRY_DATABASE_PATH=/tmp/mirobody/sentry

WORKDIR /app

# The binary resolves res/htdoc (HTTP_ROOT) and res/sql (SQL_DIR) relative to the
# working directory, so res/ and config.yml live next to it under /app.
COPY --from=builder /src/build/mirobody /app/mirobody
# crashpad_handler must sit next to the binary: src/main.cpp points
# sentry_options_set_handler_path at <exe dir>/crashpad_handler. Without it the
# crashpad backend fails to start and Sentry is disabled (logged, non-fatal).
COPY --from=builder /usr/local/bin/crashpad_handler /app/crashpad_handler
COPY --from=builder /src/res/ /app/res/
# Bake the committed template as config.example.yml — the LOWEST-precedence
# defaults layer. It must NOT be named config.yml: precedence is env > config.yml
# > remote config > config.example.yml, so a baked config.yml would override the
# remote config server's real values (PG_HOST, secrets, …) with the template
# placeholders and the server would fail to connect. Real settings come from
# remote config / env vars in managed envs, or from a config.yml an operator
# mounts at /app (which then correctly takes top file precedence).
COPY config.example.yml /app/config.example.yml

USER mirobody

ENV HTTP_PORT=80

EXPOSE 80

# No Docker HEALTHCHECK: the runtime image is libraries-only (no curl/wget), and
# under Kubernetes liveness/readiness are owned by the Pod's httpGet probes
# against /api/health on port 80.

# SIGTERM triggers a graceful shutdown (see src/main.cpp). Use exec form so the
# binary is PID 1 and receives the signal directly.
ENTRYPOINT ["/app/mirobody"]
