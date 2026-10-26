#!/bin/sh
# SPDX-License-Identifier: MIT

set -eu

if [ "$#" -ne 3 ]; then
   echo "usage: $0 GPUUM_SOURCE SCRATCH_SOURCE BUILD_DIR" >&2
   exit 2
fi

gpuum_source=$1
scratch_source=$2
build_dir=$3
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

if [ ! -d "$gpuum_source/.git" ]; then
   echo "guest source is not a git checkout: $gpuum_source" >&2
   exit 2
fi
if [ -e "$scratch_source" ] || [ -e "$build_dir" ]; then
   echo "scratch source or build directory already exists" >&2
   exit 2
fi

mkdir -p "$(dirname -- "$scratch_source")" "$(dirname -- "$build_dir")"
cp -a -- "$gpuum_source" "$scratch_source"
git -C "$scratch_source" apply "$script_dir/gpuum-vtest-import.patch"

meson setup "$build_dir" "$scratch_source" \
   -Dvulkan-drivers=virtio \
   -Dgallium-drivers= \
   -Dplatforms= \
   -Dglx=disabled \
   -Degl=disabled \
   -Dgbm=disabled \
   -Dllvm=disabled \
   -Dshared-llvm=disabled \
   -Dbuild-tests=false \
   -Dlibunwind=disabled \
   -Dvalgrind=disabled
ninja -C "$build_dir" \
   src/virtio/vulkan/libvulkan_virtio.so \
   src/virtio/vulkan/virtio_devenv_icd.x86_64.json
