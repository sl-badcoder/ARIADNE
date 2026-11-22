#!/usr/bin/env bash
# Build only. Installation and module replacement are deliberately separate.
set -euo pipefail

root_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
driver_dir="$root_dir/ARIADNE-615.71.09"

if [[ "$(uname -s)" != Linux ]]; then
    echo "Building NVIDIA kernel modules requires Linux and configured Linux kernel headers." >&2
    exit 1
fi

kernel_headers="${KERNEL_HEADERS:-/lib/modules/$(uname -r)/build}"
if [[ ! -f "$kernel_headers/Makefile" ]]; then
    echo "Kernel headers not found: $kernel_headers (set KERNEL_HEADERS to a configured header tree)." >&2
    exit 1
fi

if ! grep -qx 'NVIDIA_VERSION = 615.71.09' "$driver_dir/version.mk"; then
    echo "Unexpected NVIDIA source version; expected 615.71.09." >&2
    exit 1
fi

make -C "$driver_dir" modules -j"${JOBS:-$(getconf _NPROCESSORS_ONLN)}" \
    SYSSRC="$kernel_headers" SYSOUT="$kernel_headers" "$@"
