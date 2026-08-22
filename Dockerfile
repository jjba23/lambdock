# lambdock

# Copyright © Josep Bigorra <jjbigorra@gmail.com>

# lambdock is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.

# lambdock is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.

# You should have received a copy of the GNU General Public License
# along with lambdock.  If not, see <https://www.gnu.org/licenses/>.

# === Build time ===
FROM opensuse/tumbleweed:latest AS builder

ARG VERSION=dev

RUN zypper --non-interactive refresh && \
    zypper --non-interactive install -t pattern devel_basis devel_C_C++ && \
    zypper --non-interactive install --no-recommends \
        meson \
        ninja \
        glib2-devel \
        gtk4-devel \
        gtk4-layer-shell-devel \
        guile-devel \
        wayland-devel \
        wayland-protocols-devel \
        wlroots-devel \
        && \
    zypper clean --all

WORKDIR /src

COPY . .

RUN  meson setup --reconfigure build \
        --buildtype=release \
        -Dstrip=true \
        -Db_lto=true \
        -Db_pie=true \
    && \
    meson compile -C build \
    && \
    DESTDIR=/tmp/lambdock-root meson install -C build


# === Runtime ===
FROM opensuse/tumbleweed:latest

ARG VERSION=dev

LABEL org.opencontainers.image.title="Lambdock"
LABEL org.opencontainers.image.description="Wayland desktop dock customizable with GNU Guile Scheme"
LABEL org.opencontainers.image.url="https://codeberg.org/jjba23/lambdock"
LABEL org.opencontainers.image.licenses="GPL-3.0-or-later"
LABEL org.opencontainers.image.version="${VERSION}"

RUN zypper --non-interactive refresh && \
    zypper --non-interactive install --no-recommends \
        # gtk4 \
        # gtk4-layer-shell \
        guile \
        && \
    zypper clean --all

COPY --from=builder /tmp/lambdock-root/ /

ENV GDK_BACKEND=wayland
ENV GTK_A11Y=none

ENTRYPOINT ["/usr/bin/lambdock"]
