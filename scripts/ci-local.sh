#!/usr/bin/env bash
# Everything CI would run, locally: PII check, licence check, upstream-main guard, unit
# tests, simulator scenarios (mock screenshots and real-MeshCore), firmware
# builds and size budgets. Run inside pixi: `pixi run ci`.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
export ASAN_OPTIONS=detect_leaks=0

step() { printf '\n== %s\n' "$*"; }

step "PII check";                  scripts/pii-check.sh --all
step "licences";                   python -I scripts/check-licenses.py
step "theme contrast";             python -I scripts/check-contrast.py
step "version label";              python -I scripts/version.py --self-test
step "upstream main.cpp";          scripts/check-upstream-main.sh
step "patches apply";              scripts/prepare-deps.sh --force
step "unit tests";                 pio test -e test -e test_tdeck -e simnet_test
step "mock simulator scenarios";   pio run -e sim -e sim_tdeck && python -I scripts/screenshots.py && python -I scripts/screenshots.py --board tdeck
step "real MeshCore scenarios";    pio run -e simnet && .pio/build/simnet/program --remote-smoke && python -I scripts/simnet_tests.py
step "firmware";                   pio run -e cardputer_adv -e cardputer_adv_usb -e tdeck -e tdeck_usb
step "size budgets";               python -I scripts/check-size.py
printf '\nall checks passed\n'
