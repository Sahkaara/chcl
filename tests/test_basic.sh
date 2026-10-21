#!/usr/bin/env bash
#
# Smoke tests for the chcl CLI.
# Run with:  make test
# Requires:  bash, ./chcl already built.
set -euo pipefail

BIN="./chcl"

echo "== lint (valid input) =="
$BIN lint examples/sample.hcl

echo "== fmt (stdout) =="
$BIN fmt examples/sample.hcl | head -5

echo "== to-json =="
$BIN to-json examples/sample.hcl | head -c 200; echo

echo "== lint on invalid input (must fail) =="
echo "resource {" > /tmp/bad.hcl
if $BIN lint /tmp/bad.hcl; then
    echo "FAIL: lint should have rejected the file" >&2
    exit 1
fi
rm -f /tmp/bad.hcl

echo "OK"