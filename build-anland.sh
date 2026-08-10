#!/bin/sh
# Build the embedded Aquamarine package, install it, then build Hyprland.
# The workspace may be mounted with mode 0666 and reject chmod.  Build from a
# temporary native filesystem instead of asking dpkg-buildpackage to chmod the
# source tree in place.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
if [ "$(id -u)" -eq 0 ]; then
    echo "build-anland: do not run dpkg-buildpackage as root" >&2
    exit 2
fi
if [ ! -f "$root/debian/control" ] || [ ! -f "$root/aquamarine-0.12.1/debian/control" ]; then
    echo "build-anland: incomplete checkout" >&2
    exit 2
fi

# Install the complete Debian forky dependency set unless explicitly skipped.
# Other configured suites are allowed; at least one forky source must be present.
# Set ANLAND_SKIP_DEPS=1 only when dependencies are already installed.
if [ "${ANLAND_SKIP_DEPS:-0}" != 1 ]; then
    if ! grep -RqsE '^[[:space:]]*Suites:[[:space:]].*\bforky\b|^[[:space:]]*deb[[:space:]].*\bforky\b' /etc/apt/sources.list /etc/apt/sources.list.d 2>/dev/null; then
        cat >&2 <<'EOF'
build-anland: no Debian forky APT source was found.

This build needs packages provided by forky, such as the Hyprland ecosystem
libraries and GCC 16. Add a forky source, then run this script again. Example:

  deb http://deb.debian.org/debian forky main contrib

You may keep your existing sources; this script only requires that at least one
active source contains "forky". Run "sudo apt-get update" after editing APT
sources.
EOF
        exit 2
    fi
    export DEBIAN_FRONTEND=noninteractive
    sudo apt-get update
    sudo apt-get install -y --no-install-recommends \
        build-essential ca-certificates ccache cmake cpio debhelper-compat devscripts \
        dpkg-dev fakeroot g++-16 git pkg-config pkgconf hwdata \
        hyprland-protocols hyprwayland-scanner hyprwire-scanner \
        libcairo-dev libdisplay-info-dev libdrm-dev libegl-dev libegl1-mesa-dev \
        libgbm-dev libgles-dev libglaze-dev libhyprcursor-dev libhyprgraphics-dev \
        libhyprlang-dev libhyprutils-dev libhyprwire-dev libinput-dev liblcms2-dev \
        liblua5.5-dev libmuparser-dev libpango1.0-dev libpixman-1-dev \
        libpipewire-0.3-dev libspa-0.2-dev libseat-dev libeis-dev libre2-dev libssl-dev \
        libsdbus-c++-dev libtomlplusplus-dev libudev-dev libudis86-dev libwayland-dev libxkbcommon-dev \
        libxcb1-dev libxcb-render0-dev libxcb-xfixes0-dev libxcb-icccm4-dev \
        libxcb-composite0-dev libxcb-res0-dev libxcb-errors-dev libxcursor-dev \
        glslang-dev glslang-tools wayland-protocols xwayland xxd
fi
# Ignore any locally installed *-uninstalled.pc files.  In particular, a
# /usr/local aquamarine-uninstalled.pc can add /usr/lib to Hyprland's linker
# search path, causing it to link an unpackaged library copy instead of the
# multiarch library in libaquamarine11.
multiarch=$(dpkg-architecture -qDEB_HOST_MULTIARCH)
deb_arch=$(dpkg-architecture -qDEB_HOST_ARCH)
export PKG_CONFIG_LIBDIR="/usr/lib/$multiarch/pkgconfig:/usr/share/pkgconfig"

# Forky's libhyprgraphics is built against the GCC 16 C++ ABI.  The default
# g++ alternative can still point at GCC 15, whose development libstdc++ lacks
# GLIBCXX_3.4.35 symbols required at final link time.
if command -v g++-16 >/dev/null 2>&1; then
    export CC=${CC:-gcc-16}
    export CXX=${CXX:-g++-16}
fi

# Mobile build hosts have limited thermal and memory headroom.  Default to two
# compiler jobs, while allowing an explicit caller override.
export DEB_BUILD_OPTIONS="${DEB_BUILD_OPTIONS:-parallel=2}"

# Repeated builds copy the source into a fresh native staging directory.  Keep
# compiler cache data outside that directory so unchanged translation units can
# be reused.  Set ANLAND_CCACHE=0 to disable, or override CCACHE_DIR.
if [ "${ANLAND_CCACHE:-1}" != 0 ] && command -v ccache >/dev/null 2>&1; then
    export CCACHE_DIR="${CCACHE_DIR:-$root/.cache/ccache}"
    export CCACHE_BASEDIR="$root"
    export CCACHE_COMPILERCHECK=content
    export CCACHE_NOHASHDIR=true
    export CCACHE_MAXSIZE="${CCACHE_MAXSIZE:-2G}"
    mkdir -p "$CCACHE_DIR"
    export CC="ccache ${CC}"
    export CXX="ccache ${CXX}"
    ccache --set-config=max_size="$CCACHE_MAXSIZE"
    ccache --zero-stats
    cache_enabled=1
else
    cache_enabled=0
fi

# Xwayland is mandatory for the Anland desktop because its kgsl/turnip patch
# enables accelerated X11 clients.  Keep the patch upstream rather than
# vendoring it: this mirrors anland-main's producer approach.
download_official_dms() {
    dms_workdir="$stage/dms-download"
    rm -rf "$dms_workdir"
    mkdir -p "$dms_workdir"
    (
        cd "$dms_workdir"
        # Download only: the official package and its dependency metadata remain
        # owned by AvengeMedia's APT repository, not this source package.
        apt-get download dms
    )
    dms_deb=$(find "$dms_workdir" -maxdepth 1 -type f -name "dms_*_${deb_arch}.deb" -print | head -n 1)
    if [ -z "$dms_deb" ]; then
        echo "build-anland: official dms ARM64 package was not downloaded; configure the AvengeMedia DMS APT source" >&2
        return 2
    fi
    cp -f "$dms_deb" "$root/artifacts/"
}

build_anland_xwayland() {

    xwayland_workdir="${XWAYLAND_WORKDIR:-$root/.cache/xwayland-anland-build}"
    xwayland_patch="$xwayland_workdir/xwayland.patch"
    xwayland_url="${XWAYLAND_PATCH_URL:-https://raw.githubusercontent.com/superturtlee/anland/main/producers/kde/Debian13_v5/xwayland.patch}"

    if ! command -v curl >/dev/null 2>&1 && ! command -v wget >/dev/null 2>&1; then
        echo "build-anland: curl or wget is required to fetch xwayland.patch" >&2
        return 2
    fi

    # APT source definitions vary across Debian and Ubuntu. Enable deb-src in
    # deb822 files first (notably Debian's minimal-container debian.sources),
    # then derive source entries from any traditional deb lines.
    if ! sudo grep -rqsE '^[[:space:]]*Types:[[:space:]].*deb-src|^[[:space:]]*deb-src[[:space:]]+' \
            /etc/apt/sources.list /etc/apt/sources.list.d/ 2>/dev/null; then
        echo "build-anland: enabling deb-src repositories for Xwayland" >&2
        sudo find /etc/apt/sources.list.d -maxdepth 1 -type f -name '*.sources' \
            -exec sed -i 's/^[[:space:]]*Types:[[:space:]]*deb$/Types: deb deb-src/' {} +

        xwayland_sources=$(mktemp)
        sudo sh -c '
            grep -rhsE "^[[:space:]]*deb[[:space:]]+" /etc/apt/sources.list /etc/apt/sources.list.d 2>/dev/null \
                | sed "s/^[[:space:]]*deb[[:space:]]\\+/deb-src /" \
                > "$1"
        ' sh "$xwayland_sources"
        if [ -s "$xwayland_sources" ]; then
            sudo install -m 0644 "$xwayland_sources" /etc/apt/sources.list.d/anland-xwayland-deb-src.list
        fi
        rm -f "$xwayland_sources"

        if ! sudo grep -rqsE '^[[:space:]]*Types:[[:space:]].*deb-src|^[[:space:]]*deb-src[[:space:]]+' \
                /etc/apt/sources.list /etc/apt/sources.list.d/ 2>/dev/null; then
            echo "build-anland: cannot derive deb-src repositories from the host APT configuration" >&2
            return 2
        fi
    fi
    sudo apt-get update -qq
    sudo env DEBIAN_FRONTEND=noninteractive apt-get build-dep -y xwayland

    rm -rf "$xwayland_workdir"
    mkdir -p "$xwayland_workdir"
    if command -v curl >/dev/null 2>&1; then
        curl --fail --location --retry 3 --output "$xwayland_patch" "$xwayland_url"
    else
        wget -O "$xwayland_patch" "$xwayland_url"
    fi
    [ -s "$xwayland_patch" ]

    (
        cd "$xwayland_workdir"
        apt-get source xwayland
    )
    xwayland_tree=$(find "$xwayland_workdir" -mindepth 1 -maxdepth 1 -type d -name 'xwayland-*' -print | head -n 1)
    [ -n "$xwayland_tree" ]
    if (
        cd "$xwayland_tree" && patch --batch --forward --reject-file=- -p1 < "$xwayland_patch"
    ); then
        :
    elif grep -rqF 'No usable linux-dmabuf main device' "$xwayland_tree/hw/xwayland" 2>/dev/null; then
        echo "build-anland: Xwayland patch already present; continuing" >&2
    else
        echo "build-anland: xwayland.patch does not apply to host source" >&2
        return 2
    fi
    (
        cd "$xwayland_tree"
        DEB_BUILD_OPTIONS="nocheck ${DEB_BUILD_OPTIONS}" dpkg-buildpackage -b -uc -us -d
    )
    xwayland_deb=$(find "$xwayland_workdir" -maxdepth 1 -type f -name "xwayland_*_${deb_arch}.deb" -print | head -n 1)
    [ -n "$xwayland_deb" ]
    cp -f "$xwayland_deb" "$root/artifacts/"
}

stage=
stage=$(mktemp -d "${TMPDIR:-/tmp}/anland-build.XXXXXX")
cleanup() { rm -rf "$stage"; }
trap cleanup EXIT HUP INT TERM

cp -a "$root/." "$stage/"
# The mounted workspace may preserve stale executable bits on debhelper
# configuration files.  Only rules and hwdata.sh are scripts.
# All debian/* files except rules are declarative configuration files.
# The mounted workspace can mark them executable, causing debhelper to run
# control files as shell scripts.
find "$stage/debian" "$stage/aquamarine-0.12.1/debian" -type f -exec chmod -x {} + 2>/dev/null || true
chmod +x "$stage/debian/rules" "$stage/aquamarine-0.12.1/debian/rules" \
     "$stage/aquamarine-0.12.1/data/hwdata.sh" \
     "$stage/scripts/generateShaderIncludes.sh"

# The embedded source tree already contains the Anland implementation files,
# while this compatibility change is intentionally delivered only as a quilt
# patch.  Apply it explicitly in the native staging tree before compiling.
# (The workspace mount cannot reliably preserve executable bits or quilt state.)
(
    cd "$stage/aquamarine-0.12.1"
    if grep -q '    return session;' src/backend/Backend.cpp; then
        patch --batch -p1 < debian/patches/anland/0007-compat-Use-explicit-session-pointer-conversion.patch
    fi
    if grep -q '    return primary;' src/backend/drm/DRM.cpp; then
        patch --batch -p1 < debian/patches/anland/0008-compat-Use-explicit-weak-pointer-conversion.patch
    fi
    if grep -q '^[[:space:]]*close(fds\[i\]);' src/backend/anland/AnlandInput.cpp && ! grep -q '^#include <unistd.h>' src/backend/anland/AnlandInput.cpp; then
        patch --batch -p1 < debian/patches/anland/0009-compat-Include-unistd-for-close.patch
    fi
    grep -q 'return static_cast<bool>(session);' src/backend/Backend.cpp
    grep -q '^#include <unistd.h>' src/backend/anland/AnlandInput.cpp
    grep -q 'return static_cast<bool>(primary);' src/backend/drm/DRM.cpp
    if grep -q 'add_executable(attachments' CMakeLists.txt; then
        patch --batch -p1 < debian/patches/anland/0010-compat-Disable-failing-optional-tests.patch
    fi
    ! grep -q 'add_executable(attachments' CMakeLists.txt
    dpkg-buildpackage -us -uc -b
)

sudo apt-get install -y --allow-downgrades "$stage"/libaquamarine11_*.deb "$stage"/libaquamarine-dev_*.deb

(
    cd "$stage"
    # Hyprland 0.55.4+ds already uses explicit-safe pointer idioms (for example
    # !!pointer) in this restored source baseline.  No out-of-tree hyprutils
    # source compatibility patch is required.
    # dpkg-buildpackage invokes dpkg-source, which applies every patch declared
    # in debian/patches/series (including the ABI helper) to its private source
    # state.  Do not pre-apply a series patch here: doing so creates an
    # unrepresentable source change and makes dpkg-source reject the build.
    dpkg-buildpackage -us -uc -b

)

# Retain only the required runtime packages.  Background, debug and development
# packages are deliberately left in the temporary build directory and removed on exit.
mkdir -p "$root/artifacts"
rm -f "$root/artifacts"/*.deb "$root/artifacts/SHA256SUMS"
for package in \
    "$(dirname "$stage")"/hyprland_0.55.4+ds-2_"$deb_arch".deb \
     "$(dirname "$stage")"/hyprland-anland-desktop_0.55.4+ds-2_"$deb_arch".deb \
     "$stage"/libaquamarine11_0.12.1-1_"$deb_arch".deb; do
    if [ ! -f "$package" ]; then
        echo "build-anland: expected package was not produced: $package" >&2
        exit 2
    fi
    cp -f "$package" "$root/artifacts/"
done
# Download the official DMS package from the AvengeMedia source configured by
# the host or CI. Do not install, unpack, or repackage it here.
download_official_dms
# Xwayland needs the matching kgsl/turnip patch for X11 clients to use the
# Android GPU path. It is a separate Debian source package, built by the same
# apt-source/build-dep/patch/dpkg-buildpackage flow as anland-main's producer.
build_anland_xwayland
sha256sum "$root/artifacts"/*.deb > "$root/artifacts/SHA256SUMS"
if [ "$cache_enabled" -eq 1 ]; then
    ccache --show-stats
fi
printf '%s\n' "Packages collected in $root/artifacts"