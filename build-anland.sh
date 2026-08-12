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
if [ ! -f "$root/debian/control" ] || [ ! -f "$root/aquamarine-0.14.0/debian/control" ]; then
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
        build-essential ca-certificates ccache cmake cpio curl debhelper-compat devscripts meson ninja-build quilt \
        dpkg-dev fakeroot g++-16 git pkg-config pkgconf hwdata \
        hyprland-protocols hyprwayland-scanner hyprwire-scanner \
        libcairo-dev libdisplay-info-dev libdrm-dev libegl-dev libegl1-mesa-dev \
        libgbm-dev libgles-dev libglaze-dev libhyprcursor-dev libhyprgraphics-dev \
        libhyprlang-dev libhyprutils-dev libhyprwire-dev libinput-dev liblcms2-dev libglm-dev \
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
# multiarch library in libaquamarine13.
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
    # CCACHE_BASEDIR is set to the native staging tree below.  The workspace
    # itself is copied to a fresh /tmp directory for every build, so using the
    # workspace path here prevents cache keys from matching staged sources.
    export CCACHE_COMPILERCHECK=content
    # Debian's generated build flags contain the absolute staging path. Strip
    # it from preprocessor output as well as the usual debug-path rewrite.
    export CCACHE_BASEDIR
    export CCACHE_NOHASHDIR=true
    export CCACHE_MAXSIZE="${CCACHE_MAXSIZE:-2G}"
    mkdir -p "$CCACHE_DIR"
    # When CI installs its verification wrapper at /usr/local/bin/ccache, keep
    # the compiler commands as `ccache <compiler>` so the wrapper can prove
    # that dpkg-buildpackage actually routed compilation through ccache.
    export PATH="/usr/local/bin:$PATH"
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
build_anland_xwayland() {

    # Keep Xwayland under the same per-build staging root as Aquamarine and
    # Hyprland.  CCACHE_BASEDIR is that root, so all three projects receive
    # stable relative compiler paths across CI runs.  The old $root/.cache
    # default was outside CCACHE_BASEDIR and made Xwayland a full cache miss.
    xwayland_workdir="${XWAYLAND_WORKDIR:-$stage/xwayland-anland-build}"
    xwayland_patch="$xwayland_workdir/xwayland.patch"
    xwayland_url="${XWAYLAND_PATCH_URL:-https://raw.githubusercontent.com/superturtlee/anland/main/producers/kde/Debian13_v5/xwayland.patch}"

    if ! command -v curl >/dev/null 2>&1 && ! command -v wget >/dev/null 2>&1; then
        echo "build-anland: curl or wget is required to fetch xwayland.patch" >&2
        return 2
    fi

    # APT source definitions vary across Debian and Ubuntu. Enable deb-src in
    # deb822 files first (notably Debian's minimal-container debian.sources),
    # then derive source entries from any traditional deb lines.
    if ! grep -rqsE '^[[:space:]]*Types:[[:space:]].*deb-src|^[[:space:]]*deb-src[[:space:]]+' \
            /etc/apt/sources.list /etc/apt/sources.list.d/ 2>/dev/null; then
        echo "build-anland: enabling deb-src repositories for Xwayland" >&2
        sudo find /etc/apt/sources.list.d -maxdepth 1 -type f -name '*.sources' \
            -exec sed -i 's/^[[:space:]]*Types:[[:space:]]*deb$/Types: deb deb-src/' {} +

        # Write traditional-source output directly as root.  A temporary file made
        # by the unprivileged builder may be unreadable to a rootless/container
        # sudo implementation.
        sudo sh -c '
            grep -rhsE "^[[:space:]]*deb[[:space:]]+" /etc/apt/sources.list /etc/apt/sources.list.d 2>/dev/null \
                | sed "s/^[[:space:]]*deb[[:space:]]\\+/deb-src /" \
                > /etc/apt/sources.list.d/anland-xwayland-deb-src.list
        '

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

# Use a stable native staging path. Debian-generated compiler flags can embed
# this root, so a random mktemp suffix prevents ccache direct hits even when
# CCACHE_BASEDIR is set. The workspace itself is not used because Android/FUSE
# mounts can reject chmod during dpkg builds.
stage="${ANLAND_STAGE_DIR:-${TMPDIR:-/tmp}/anland-build-staging}"
rm -rf "$stage"
mkdir -p "$stage"
cleanup() { rm -rf "$stage"; }
trap cleanup EXIT HUP INT TERM

if [ "$cache_enabled" -eq 1 ]; then
    export CCACHE_BASEDIR="$stage"
fi

cp -a "$root/." "$stage/"
# The mounted workspace may preserve stale executable bits on debhelper
# configuration files.  Only rules and hwdata.sh are scripts.
# All debian/* files except rules are declarative configuration files.
# The mounted workspace can mark them executable, causing debhelper to run
# control files as shell scripts.
find "$stage/debian" "$stage/aquamarine-0.14.0/debian" -type f -exec chmod -x {} + 2>/dev/null || true
chmod +x "$stage/debian/rules" "$stage/aquamarine-0.14.0/debian/rules" \
     "$stage/aquamarine-0.14.0/data/hwdata.sh" \
     "$stage/scripts/generateShaderIncludes.sh"

# Keep the Aquamarine upstream source clean. Anland functionality is carried
# exclusively by the Debian quilt series and is applied only in native staging.
(
    cd "$stage/aquamarine-0.14.0"
    QUILT_PATCHES=debian/patches quilt push -a
    grep -q "return static_cast<bool>(session);" src/backend/Backend.cpp
    grep -q "^#include <unistd.h>" src/backend/anland/AnlandInput.cpp
    grep -q "return static_cast<bool>(primary);" src/backend/drm/DRM.cpp
    ! grep -q "add_executable(attachments" CMakeLists.txt
    dpkg-buildpackage -us -uc -b
)


sudo apt-get install -y --allow-downgrades "$stage"/libaquamarine13_*.deb "$stage"/libaquamarine-dev_*.deb

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
# Hyprgrass is compiled during the Hyprland Debian install phase and is therefore
# embedded in hyprland_<version>_<arch>.deb, never delivered as a loose plugin.
mkdir -p "$root/artifacts"
rm -f "$root/artifacts"/*.deb "$root/artifacts/SHA256SUMS"
rm -rf "$root/artifacts/hyprgrass"
copy_one_deb() {
    pattern=$1
    package=$(find "$(dirname "$stage")" "$stage" -maxdepth 1 -type f \
        -name "$pattern" -print | head -n 1)
    if [ -z "$package" ]; then
        echo "build-anland: expected package was not produced: $pattern" >&2
        exit 2
    fi
    cp -f "$package" "$root/artifacts/"
}

# Do not bake upstream versions into artifact collection. The Debian changelog
# determines the package version, so a future source refresh remains coherent.
copy_one_deb "hyprland_*_${deb_arch}.deb"
copy_one_deb "hyprland-anland-desktop_*_${deb_arch}.deb"
copy_one_deb "libaquamarine13_*_${deb_arch}.deb"
# DMS is installed by install-anland-desktop.sh from its official APT source.
# Keep the build artifact focused on the locally built Debian packages.
# Xwayland needs the matching kgsl/turnip patch for X11 clients to use the
# Android GPU path. It is a separate Debian source package, built by the same
# apt-source/build-dep/patch/dpkg-buildpackage flow as anland-main's producer.
build_anland_xwayland
# Ship the installer alongside the packages so a downloaded CI artifact is
# self-contained.  Explicit chmod also protects against source mounts that do
# not preserve executable bits.
install_script="$root/install-anland-desktop.sh"
if [ ! -f "$install_script" ]; then
    echo "build-anland: missing artifact installer: $install_script" >&2
    exit 2
fi
cp -f "$install_script" "$root/artifacts/install-anland-desktop.sh"
chmod 0755 "$root/artifacts/install-anland-desktop.sh"
(
    cd "$root/artifacts"
    sha256sum ./*.deb ./install-anland-desktop.sh > SHA256SUMS
)
if [ "$cache_enabled" -eq 1 ]; then
    ccache --show-stats
fi
printf '%s\n' "Packages collected in $root/artifacts"