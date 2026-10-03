#!/usr/bin/env bash
# Compile the emulator core (core/*.cpp) to web/core/vmu.wasm.
# Only needed after changing something in core/: the built file is part of the repository.
# Needs clang and lld with the WebAssembly target:  sudo apt install clang lld
set -euo pipefail
cd "$(dirname "$0")"
CXX="${CXX:-clang++}"
command -v "$CXX" >/dev/null || { echo "clang++ is not installed (sudo apt install clang lld)"; exit 1; }
"$CXX" --target=wasm32 -std=c++11 -O2 -nostdlib -ffreestanding -fno-exceptions -fno-rtti -fno-threadsafe-statics \
  -fvisibility=hidden -Wall -Wextra \
  -Wl,--no-entry -Wl,--strip-all -Wl,-z,stack-size=65536 \
  -o web/core/vmu.wasm core/vmu.cpp core/vmufs.cpp core/wasm_api.cpp
chmod 644 web/core/vmu.wasm
ls -l web/core/vmu.wasm
