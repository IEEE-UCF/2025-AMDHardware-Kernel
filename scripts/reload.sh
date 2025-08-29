#!/usr/bin/env bash
set -euo pipefail
bash scripts/rmmod.sh || true
bash scripts/insmod.sh
