# syntax=docker/dockerfile:1.7

# lambdock -- hyper-hackable desktop dock for Wayland
#
# SPDX-License-Identifier: MIT
# Copyright © Josep Bigorra <jjbigorra@gmail.com>

# ==============================================================================
# Build stage
# ==============================================================================

FROM opensuse/tumbleweed:latest AS builder

ARG VERSION=dev

ENV LANG=C.UTF-8 \
    LC_ALL=C.UTF-8

WORKDIR /src

# ------------------------------------------------------------------------------
# System dependencies
# ------------------------------------------------------------------------------

RUN --mount=type=cache,target=/var/cache/zypp \
    set -eu; \
    echo "==> Refreshing openSUSE repositories"; \
    zypper --non-interactive refresh; \
    echo "==> Installing build dependencies"; \
    zypper --non-interactive install --no-recommends \
        gcc \
        gcc-c++ \
        glib2-devel \
        gtk4-devel \
        gtk4-layer-shell-devel \
        guile-devel \
        gettext \
        meson \
        ninja \
        pkg-config \
        wayland-devel \
        wayland-protocols-devel \
        wlroots-devel; \
    echo "==> Cleaning package metadata"; \
    zypper clean --all

# ------------------------------------------------------------------------------
# Source
#
# ------------------------------------------------------------------------------

COPY meson.build ./
COPY data/ ./data/
COPY po/ ./po/
COPY protocols/ ./protocols/
COPY resources/ ./resources/
COPY scheme/ ./scheme/
COPY src/ ./src/
COPY tests/ ./tests/

# ------------------------------------------------------------------------------
# Configure
# ------------------------------------------------------------------------------

RUN --mount=type=cache,target=/root/.cache \
    set -eu; \
    echo "==> Configuring Lambdock ${VERSION}"; \
    meson setup build; \
    echo "==> Meson configuration"; \
    meson configure build

# ------------------------------------------------------------------------------
# Build
# ------------------------------------------------------------------------------

RUN --mount=type=cache,target=/root/.cache \
    set -eu; \
    echo "==> Building Lambdock"; \
    meson compile -C build --verbose; \
    echo "==> Build completed successfully"

# ------------------------------------------------------------------------------
# Tests
# ------------------------------------------------------------------------------

RUN --mount=type=cache,target=/root/.cache \
    set -eu; \
    echo "==> Running Lambdock tests"; \
    meson test \
        -C build \
        --print-errorlogs \
        --verbose; \
    echo "==> Tests completed successfully"

# ------------------------------------------------------------------------------
# Install into staging directory
# ------------------------------------------------------------------------------

RUN set -eu; \
    echo "==> Installing Lambdock into staging directory"; \
    rm -rf /tmp/lambdock-root; \
    DESTDIR=/tmp/lambdock-root meson install -C build; \
    echo "==> Installed files:"; \
    find /tmp/lambdock-root -type f -o -type l | sort


# ==============================================================================
# Runtime stage
# ==============================================================================

FROM opensuse/tumbleweed:latest AS runtime

ARG VERSION=dev

ENV LANG=C.UTF-8 \
    LC_ALL=C.UTF-8 \
    GDK_BACKEND=wayland \
    GTK_A11Y=none

LABEL org.opencontainers.image.title="Lambdock" \
      org.opencontainers.image.description="Wayland desktop dock customizable with GNU Guile Scheme" \
      org.opencontainers.image.url="https://github.com/jjba23/lambdock" \
      org.opencontainers.image.licenses="MIT" \
      org.opencontainers.image.version="${VERSION}"

# ------------------------------------------------------------------------------
# Runtime dependencies
# ------------------------------------------------------------------------------

RUN --mount=type=cache,target=/var/cache/zypp \
    set -eu; \
    echo "==> Refreshing openSUSE repositories"; \
    zypper --non-interactive refresh; \
    echo "==> Installing runtime dependencies"; \
    zypper --non-interactive install --no-recommends \
        libgtk-4-1 \
        libgtk4-layer-shell0 \
        guile; \
    echo "==> Cleaning package metadata"; \
    zypper clean --all

# ------------------------------------------------------------------------------
# Application
# ------------------------------------------------------------------------------

COPY --from=builder /tmp/lambdock-root/ /

# ------------------------------------------------------------------------------
# Runtime diagnostics
# ------------------------------------------------------------------------------

RUN set -eu; \
    echo "==> Lambdock installation"; \
    command -v lambdock; \
    ls -lh /usr/local/bin/lambdock; \
    echo "==> Lambdock version"; \
    /usr/local/bin/lambdock --version || true; \
    echo "==> Runtime libraries"; \
    ldd /usr/local/bin/lambdock || true

ENTRYPOINT ["/usr/local/bin/lambdock"]
