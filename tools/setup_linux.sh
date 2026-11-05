#!/usr/bin/env bash
# One-time setup on x86-64 Linux (Ubuntu 22.04/24.04, GitHub Codespaces, WSL2).
# BabbleSim and the nrf52_bsim board are 32-bit x86 Linux programs, so they do not run on macOS.
# Run from the repo:  bash tools/setup_linux.sh   then:  python3 tools/run.py | tee results.txt
set -euo pipefail
if [ "$(uname -s)" != "Linux" ] || [ "$(uname -m)" != "x86_64" ]; then
  echo "This script is for x86-64 Linux (a Codespace, Ubuntu or WSL2); this machine is $(uname -s) $(uname -m)."
  echo "On a Mac run the unit tests:  make -C tests/unit && ./tests/unit/build/unit_tests"
  echo "The full BLE simulation runs in GitHub Actions on every push (Actions tab -> ci -> ble-sim)."
  exit 1
fi
REPO=$(cd "$(dirname "$0")/.." && pwd)
WS=$(dirname "$REPO")
NAME=$(basename "$REPO")

sudo apt-get update
sudo apt-get install -y --no-install-recommends git cmake ninja-build gcc-multilib g++-multilib \
     device-tree-compiler python3-venv python3-pip make

python3 -m venv "$WS/.venv"
. "$WS/.venv/bin/activate"
pip install -q west

cd "$WS"
[ -d .west ] || west init -l "$NAME"
west update --narrow -o=--depth=1
pip install -q -r zephyr/scripts/requirements-base.txt matplotlib

cd tools/bsim
[ -e Makefile ] || ln -s components/common/Makefile Makefile
make everything -j"$(nproc)" > /dev/null

cat > "$WS/env.sh" <<ENV
. "$WS/.venv/bin/activate"
export ZEPHYR_BASE="$WS/zephyr"
export BSIM_OUT_PATH="$WS/tools/bsim"
export BSIM_COMPONENTS_PATH="$WS/tools/bsim/components"
export ZEPHYR_TOOLCHAIN_VARIANT=host
ENV
echo "Done. Next:  . $WS/env.sh && cd $REPO && python3 tools/run.py | tee results.txt"
