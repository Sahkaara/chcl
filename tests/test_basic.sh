#!/usr/bin/env bash
# CLI regressions. Core API and allocation failures are tested by test_core.c.
set -euo pipefail
BIN="${BIN:-./chcl}"
BIN="$(cd "$(dirname "$BIN")" && pwd)/$(basename "$BIN")"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

expect_failure() {
    local expected=$1
    shift
    local result=0
    "$@" >"$tmp/stdout" 2>"$tmp/stderr" || result=$?
    if [[ $result != "$expected" ]]; then
        echo "FAIL: expected exit $expected, got $result: $*" >&2
        exit 1
    fi
}

"$BIN" lint examples/sample.hcl
"$BIN" to-json examples/sample.hcl >"$tmp/sample.json"
[[ -s "$tmp/sample.json" ]]
"$BIN" fmt examples/sample.hcl >"$tmp/once.hcl"
"$BIN" fmt "$tmp/once.hcl" >"$tmp/twice.hcl"
cmp "$tmp/once.hcl" "$tmp/twice.hcl"
"$BIN" fmt --check "$tmp/once.hcl"
"$BIN" lint "$tmp/once.hcl"

printf 'x=1\nlonger=2\n' >"$tmp/messy.hcl"
expect_failure 1 "$BIN" fmt --check "$tmp/messy.hcl"
# An existing candidate temporary must never be overwritten.
printf 'keep me\n' >"$tmp/messy.hcl.chcl-tmp-0"
"$BIN" fmt -w "$tmp/messy.hcl"
[[ $(cat "$tmp/messy.hcl.chcl-tmp-0") == 'keep me' ]]
printf 'x      = 1\nlonger = 2\n' >"$tmp/expected.hcl"
cmp "$tmp/messy.hcl" "$tmp/expected.hcl"
"$BIN" fmt --check "$tmp/messy.hcl"

printf 'resource {\n' >"$tmp/bad.hcl"
expect_failure 2 "$BIN" lint "$tmp/bad.hcl"
printf 'x = 1 @ trailing\n' >"$tmp/bad.hcl"
cp "$tmp/bad.hcl" "$tmp/original.hcl"
expect_failure 1 "$BIN" fmt -w "$tmp/bad.hcl"
cmp "$tmp/bad.hcl" "$tmp/original.hcl"
printf 'x = "trailing\\' >"$tmp/bad.hcl"
expect_failure 2 "$BIN" lint "$tmp/bad.hcl"
expect_failure 1 "$BIN" fmt "$tmp/bad.hcl"

: >"$tmp/empty.hcl"
"$BIN" fmt --check "$tmp/empty.hcl"
"$BIN" fmt -w "$tmp/empty.hcl"
[[ ! -s "$tmp/empty.hcl" ]]
expect_failure 1 "$BIN" lint "$tmp/empty.hcl" ignored.hcl
expect_failure 1 "$BIN" fmt --check -w "$tmp/empty.hcl"
expect_failure 1 "$BIN" to-json "$tmp/missing.hcl"
"$BIN" fmt -- "$tmp/empty.hcl" >"$tmp/empty.out"

printf 'x=1\0trailing\n' >"$tmp/nul.hcl"
expect_failure 1 "$BIN" lint "$tmp/nul.hcl"
expect_failure 1 "$BIN" fmt "$tmp/nul.hcl"
printf 'x=1\n' >"$tmp/-leading.hcl"
(cd "$tmp" && "$BIN" fmt -- -leading.hcl) >"$tmp/leading.out"
printf 'x = 1\n' >"$tmp/leading.expected"
cmp "$tmp/leading.out" "$tmp/leading.expected"

case "$(uname -s)" in
    Linux|Darwin)
        cp "$tmp/expected.hcl" "$tmp/link-target.hcl"
        ln -s "$tmp/link-target.hcl" "$tmp/symlink.hcl"
        expect_failure 1 "$BIN" fmt -w "$tmp/symlink.hcl"
        [[ -L "$tmp/symlink.hcl" ]]
        ln "$tmp/link-target.hcl" "$tmp/hardlink.hcl"
        expect_failure 1 "$BIN" fmt -w "$tmp/hardlink.hcl"
        cmp "$tmp/link-target.hcl" "$tmp/expected.hcl"
        printf 'private=1\n' >"$tmp/private.hcl"
        chmod 600 "$tmp/private.hcl"
        "$BIN" fmt -w "$tmp/private.hcl"
        case "$(uname -s)" in
            Linux) [[ $(stat -c '%a' "$tmp/private.hcl") == 600 ]] ;;
            Darwin) [[ $(stat -f '%Lp' "$tmp/private.hcl") == 600 ]] ;;
        esac
        if [[ -c /dev/full ]]; then
            result=0
            "$BIN" fmt "$tmp/expected.hcl" >/dev/full 2>"$tmp/stderr" || result=$?
            [[ $result == 1 ]]
        fi
        ;;
esac

# Golden fixtures are optional; discover any added fixture pairs.
for input in tests/cases/*.hcl; do
    [[ -e "$input" ]] || continue
    [[ -f "$input.expected" ]] || { echo "Missing $input.expected" >&2; exit 1; }
    "$BIN" fmt "$input" >"$tmp/golden.hcl"
    cmp "$input.expected" "$tmp/golden.hcl"
done

echo "CLI regressions: OK"
