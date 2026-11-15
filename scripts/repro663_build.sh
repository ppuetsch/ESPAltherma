#!/usr/bin/env bash
# Diagnosis of #663: builds one environment of one branch in the WSL clone and reports the image.
# Usage: repro663_build.sh <branch> <environment>
set -euo pipefail
branch="$1"
environment="$2"
cd ~/code/espaltherma-663
git fetch -q origin "+refs/heads/${branch}:refs/remotes/origin/${branch}"
git checkout -q --detach "origin/${branch}"
if ! ~/.platformio/penv/bin/pio run -e "${environment}" > "build-${environment}.log" 2>&1; then
  grep -n -E "error|Error" "build-${environment}.log" | head -40
  exit 1
fi
grep -E "^RAM|^Flash" "build-${environment}.log"
ls -la ".pio/build/${environment}/firmware.bin"
git log --oneline -1
