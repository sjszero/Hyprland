#!/bin/sh
# Configure the official AvengeMedia repositories and install a downloaded
# Anland Hyprland artifact set. Run from the artifact directory, or pass it as
# the first argument:
#   ./install-anland-desktop.sh /path/to/anland-debian-packages-arm64
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
artifact_dir=${1:-$script_dir}
artifact_dir=$(CDPATH= cd -- "$artifact_dir" && pwd)

if [ "$(dpkg --print-architecture)" != arm64 ]; then
    echo "install-anland-desktop: this artifact set requires an arm64 Debian system" >&2
    exit 2
fi

find_one() {
    pattern=$1
    matches=$(find "$artifact_dir" -maxdepth 1 -type f -name "$pattern" -print)
    count=$(printf '%s\n' "$matches" | sed '/^$/d' | wc -l)
    if [ "$count" -ne 1 ]; then
        echo "install-anland-desktop: expected exactly one $pattern in $artifact_dir (found $count)" >&2
        exit 2
    fi
    printf '%s\n' "$matches"
}

hyprland_deb=$(find_one 'hyprland_*_arm64.deb')
dms_deb=$(find_one 'dms_*_arm64.deb')
desktop_deb=$(find_one 'hyprland-anland-desktop_*_arm64.deb')
aquamarine_deb=$(find_one 'libaquamarine11_*_arm64.deb')
xwayland_deb=$(find_one 'xwayland_*_arm64.deb')

if [ -f "$artifact_dir/SHA256SUMS" ]; then
    (
        cd "$artifact_dir"
        sha256sum -c SHA256SUMS
    )
fi

# DMS itself is from the dms repository; its DankLinux runtime components
# (including danksearch, dgop and matugen) are from the second repository.
sudo env DEBIAN_FRONTEND=noninteractive apt-get update
sudo env DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    ca-certificates curl
sudo install -d -m 0755 /etc/apt/keyrings
sudo curl --fail --location --retry 3 \
    --output /etc/apt/keyrings/avengemedia-dms.asc \
    https://download.opensuse.org/repositories/home:/AvengeMedia:/dms/Debian_Unstable/Release.key
sudo curl --fail --location --retry 3 \
    --output /etc/apt/keyrings/avengemedia-danklinux.asc \
    https://download.opensuse.org/repositories/home:/AvengeMedia:/danklinux/Debian_Unstable/Release.key

sudo tee /etc/apt/sources.list.d/avengemedia-dms.sources >/dev/null <<'EOF'
Types: deb
URIs: https://download.opensuse.org/repositories/home:/AvengeMedia:/dms/Debian_Unstable/
Suites: /
Signed-By: /etc/apt/keyrings/avengemedia-dms.asc
EOF
sudo tee /etc/apt/sources.list.d/avengemedia-danklinux.sources >/dev/null <<'EOF'
Types: deb
URIs: https://download.opensuse.org/repositories/home:/AvengeMedia:/danklinux/Debian_Unstable/
Suites: /
Signed-By: /etc/apt/keyrings/avengemedia-danklinux.asc
EOF

sudo env DEBIAN_FRONTEND=noninteractive apt-get update
sudo env DEBIAN_FRONTEND=noninteractive apt-get install -y \
    "$aquamarine_deb" \
    "$hyprland_deb" \
    "$xwayland_deb" \
    "$dms_deb" \
    "$desktop_deb"

echo "Anland Hyprland desktop installed. Start it with: start-hyprland-anland"