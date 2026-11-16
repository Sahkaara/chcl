# chcl

**A fast, dependency-free HCL toolkit in pure C — parser, formatter, linter, and JSON converter in a single binary.**

[![CI](https://github.com/Sahkaara/chcl/actions/workflows/ci.yml/badge.svg)](https://github.com/Sahkaara/chcl/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![C11](https://img.shields.io/badge/C-C11-blue.svg)](https://en.cppreference.com/w/c/11)
[![Zero dependencies](https://img.shields.io/badge/dependencies-0-brightgreen.svg)](#)

`chcl` is a lightweight toolkit for working with [HCL](https://github.com/hashicorp/hcl) (HashiCorp Configuration Language), the language behind Terraform, Packer, Vault, and other HashiCorp tools. It's written in portable C11 with **zero external dependencies**, and compiles to a **single executable** in under a second.

Use it from the command line to format or validate `.hcl` files, or link it as a library from your own C project.

---

## Table of contents

- [Why chcl?](#why-chcl)
- [Quick start](#quick-start)
- [Installation](#installation)
- [Usage](#usage)
  - [`chcl fmt`](#chcl-fmt)
  - [`chcl fmt --check`](#chcl-fmt---check)
  - [`chcl lint`](#chcl-lint)
  - [`chcl to-json`](#chcl-to-json)
- [Use as a C library](#use-as-a-c-library)
- [CI integration](#ci-integration)
- [Supported HCL subset](#supported-hcl-subset)
- [Architecture](#architecture)
- [Building from source](#building-from-source)
- [Testing](#testing)
- [Roadmap](#roadmap)
- [Contributing](#contributing)
- [License](#license)

---

## Why chcl?

There are excellent HCL parsers in Go, Rust, and C++ — but almost nothing usable in **pure C** that ships as a **ready-to-use binary**. If you write C code that consumes HCL (a config loader for a service, a Terraform helper, a codegen tool, an embedded agent), you usually end up either:

- shelling out to `terraform fmt` / `terraform validate` and parsing text output, or
- vendoring a heavy C++/Rust dependency, or
- writing your own half-broken parser.

`chcl` fills that gap with three properties that matter:

- **Zero dependencies.** Uses the C runtime plus native file-replacement APIs on Windows. GCC and Clang are supported; MSVC is not yet verified.
- **Single binary.** No shared libraries to install, no version conflicts, no Python/Node/Go runtime.
- **Fast.** Parses and formats a typical Terraform file in microseconds.

It's not a complete HCL implementation — it covers the subset used in the overwhelming majority of real configs (see [Supported HCL subset](#supported-hcl-subset)) and is easy to extend.

---

## Quick start

```bash
git clone https://github.com/Sahkaara/chcl.git
cd chcl
make
./chcl fmt examples/sample.hcl
```

That's it. You should see the file reformatted with aligned `=` signs:

```hcl
variable "region" {
  default = "eu-west-1"
  type    = string
}

resource "aws_instance" "web" {
  ami           = "ami-12345"
  instance_type = "t2.micro"
  count         = 3

  tags = {
    Name = "web"
    Env  = "prod"
  }
}
```

---

## Installation

### From source (recommended)

```bash
git clone https://github.com/Sahkaara/chcl.git
cd chcl
make
sudo make install    # installs to /usr/local/bin/chcl
```

### Manual compile

```bash
cc -std=c11 -O2 -Wall -Wextra -Wpedantic -Iinclude -o chcl src/chcl.c src/main.c
```

### Windows

```powershell
gcc -std=c11 -O2 -Wall -Wextra -Wpedantic -Iinclude -o chcl.exe src\chcl.c src\main.c
```

> **PowerShell users:** prefix the executable with `.\` — Windows does not load executables from the current directory by default:
>
> ```powershell
> .\chcl.exe fmt examples\sample.hcl
> ```

---

## Usage

```
chcl <command> [options] <file>

Commands:
  fmt [-w] [--check] <file.hcl>   Format (or check) the file
  lint <file.hcl>                 Validate the syntax
  to-json <file.hcl>              Convert HCL to JSON

Options:
  --help, -h      Show help
  --version       Print version
```

### `chcl fmt`

Re-emits the file with canonical formatting: 2-space indentation, single spaces around `=`, newlines after `{`, and **aligned `=` within contiguous runs of attribute lines** (mirroring `terraform fmt`).

```bash
# Print formatted output to stdout
chcl fmt examples/sample.hcl

# Rewrite the file in place
chcl fmt -w examples/sample.hcl
```

`-w` writes an exclusive temporary beside the destination, checks the write,
flush, disk synchronization and close, then replaces the original. On POSIX the
temporary starts with private permissions (0600). It requires write access to the directory.
Replacement is not a power-loss durability guarantee. POSIX permission bits are
preserved; Windows uses `ReplaceFileA` to preserve destination metadata. POSIX
ACLs and extended attributes are not preserved. On POSIX, `-w` refuses symbolic
links, multiply-linked files and special files instead of replacing their identities.
Use `fmt -- <path>` for paths beginning with `-`. Extra file arguments and the
combination `-w --check` are rejected.

The formatter works **directly on the token stream**, so it succeeds even on files that the parser would reject. Lexical errors (including unknown characters and unterminated literals) fail without producing a replacement. This is a subset formatter, not a complete HCL validator. Multiline literal and comment contents are excluded from alignment.

### `chcl fmt --check`

The CI-friendly mode. Behaves like `gofmt -l`:

- Exit **0** if the file is already canonically formatted.
- Exit **1** and print the filename if formatting would change it.
- Exit **1** and print an error on read, formatting or allocation failure.

```bash
chcl fmt --check infra/main.hcl
# (no output, exit 0)  → file is clean
# infra/main.hcl       → file needs formatting
```

This is designed to be your CI gate:

```bash
# In a shell script or CI step:
find . -name '*.hcl' -print0 | xargs -0 -n1 chcl fmt --check
```

### `chcl lint`

Parses the file into an AST. Prints a `file:line:column: error: message` diagnostic if anything is wrong, and exits with code **2**. Silent and exit **0** on success.

```bash
chcl lint examples/sample.hcl
# (no output, exit 0)
```

The error format follows the [GNU error conventions](https://www.gnu.org/prep/standards/html_node/Errors.html), so it's directly clickable in `vim`, `emacs`, VS Code, and every modern CI annotation system.

### `chcl to-json`

Serializes the parsed AST to compact JSON. Useful for:

- debugging (`chcl to-json file.hcl | jq .`)
- bridging HCL into a JSON-consuming pipeline
- writing golden tests for your own HCL files

```bash
$ chcl to-json examples/sample.hcl
{"variable": [{"labels": ["region"], "body": {"default": "eu-west-1", "type": {"traversal": "string"}}}], "resource": [{"labels": ["aws_instance", "web"], "body": {"ami": "ami-12345", "count": 3}}]}
```

**JSON schema used:**
- **Attributes** → direct keys on the enclosing object. Duplicate keys and attribute/block name collisions are rejected.
- **Numbers** → emitted from validated source spelling, without floating-point rounding or locale conversion.
- **Blocks** → keyed by block type, always as an array (schema change), with `{"labels": [...], "body": {...}}` as each element.
- **Expressions** → nested objects with a specific structure:
  - Unary ops: `{"unary_op": "!", "operand": false}`
  - Binary ops: `{"op": "+", "left": 5, "right": 3}`
  - Traversals: `{"traversal": "var.ami_id"}`
  - Function calls: `{"call": "max", "args": [1, 2]}`
  - Conditionals: `{"cond": ..., "true": ..., "false": ...}`

---

## Use as a C library

Link `src/chcl.c` into your project and include `include/chcl.h`.

```c
#include "chcl.h"
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    chcl_document_t *doc = chcl_parse_file("config.hcl");
    if (!doc) {
        fprintf(stderr, "cannot read config.hcl\n");
        return 1;
    }

    /* Always check for parse errors before using the AST. */
    if (chcl_document_error(doc)) {
        fprintf(stderr, "config.hcl:%d:%d: %s\n",
                chcl_document_error_line(doc),
                chcl_document_error_column(doc),
                chcl_document_error(doc));
        chcl_free_document(doc);
        return 1;
    }

    /* Do something with the parsed document... */
    char *json = chcl_to_json(doc);
    if (json) {
        puts(json);
        free(json);              /* json is malloc'd, free() it */
    }

    chcl_free_document(doc);     /* releases the AST and all node memory */
    return 0;
}
```

### Memory ownership

| Returned type | How to free it |
|---|---|
| `chcl_document_t *` (from `chcl_parse_*`) | `chcl_free_document()` |
| `char *` (from `chcl_format*`, `chcl_to_json`) | `free()` |
| `const char *` (from `chcl_document_error`) | Do **not** free — borrowed from the document |

The document owns all AST memory through an arena allocator, so freeing a document is a **single call**, regardless of how large the input was.

### Public API summary

```c
/* Parsing */
chcl_document_t *chcl_parse_string(const char *src);
chcl_document_t *chcl_parse_file(const char *path);
void             chcl_free_document(chcl_document_t *doc);

/* Formatting */
char *chcl_format(const char *src);
char *chcl_format_file(const char *path);
int   chcl_format_is_canonical(const char *path);  /* 1=clean, 0=needs fmt, -1=error */

/* Serialization */
char *chcl_to_json(chcl_document_t *doc);

/* Error reporting */
const char *chcl_document_error(chcl_document_t *doc);
int         chcl_document_error_line(chcl_document_t *doc);
int         chcl_document_error_column(chcl_document_t *doc);
```

---

## CI integration

### GitHub Actions

Add `.github/workflows/ci.yml`:

```yaml
name: CI
on: [push, pull_request]
jobs:
  build:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - name: Build
        run: make
      - name: Test
        run: make test
      - name: Check HCL formatting
        run: find . -name '*.hcl' -print0 | xargs -0 -n1 ./chcl fmt --check
```

### Pre-commit hook

Create `.git/hooks/pre-commit`:

```bash
#!/bin/sh
git diff --cached --name-only --diff-filter=ACM -z -- '*.hcl' |
  xargs -0 -r -n1 chcl fmt --check --
```

Make it executable (`chmod +x .git/hooks/pre-commit`). This checks working-tree contents for staged paths; it does not inspect staged blobs when files are partially staged. Any selected file that is not canonically formatted blocks the commit.

---

## Supported HCL subset

`chcl` implements a practical subset of HCL — the part that appears in the vast majority of real-world configs. It is **not** a complete implementation of the HCL spec.

| Feature | Supported | Notes |
|---|:---:|---|
| Attributes (`key = value`) | ✅ | |
| Blocks (`resource "x" "y" { ... }`) | ✅ | With any number of labels |
| Strings (`"..."`) | ✅ | UTF-8 validated; escapes decoded for JSON; source spelling preserved by fmt |
| Numbers (int/float) | ✅ | Validated decimal spelling retained, including exponents |
| Booleans (`true` / `false`) | ✅ | |
| `null` | ✅ | |
| Lists (`[a, b, c]`) | ✅ | Trailing comma tolerated |
| Object literals (`{ k = v }`) | ✅ | |
| Line comments (`#`, `//`) | ✅ | Preserved by `fmt` |
| Block comments (`/* ... */`) | ✅ | Preserved by `fmt` |
| Bare identifiers as values (`type = string`) | ✅ | Serialized as traversal nodes |
| Interpolations (`${...}`) | ✅ | Preserved verbatim inside strings |
| Heredocs (`<<EOF ... EOF`) | ✅ | Parsed as multiline strings |
| `for` expressions | 🟨 | List comprehensions retained as opaque text; not evaluated |
| Conditionals / functions | ✅ | Parsed as AST nodes (Conditional, Call, Binary) |

---

## Architecture

`chcl` is organized as a single translation unit (`src/chcl.c`) with five well-isolated components:

```
┌──────────┐   ┌──────────┐   ┌──────────┐
│  Lexer   │──▶│  Parser  │──▶│   AST    │
└──────────┘   └──────────┘   └────┬─────┘
       │                           │
       │                           ▼
       │                     ┌──────────┐
       │                     │  to-json │
       │                     └──────────┘
       │
       ▼
┌──────────┐   ┌──────────────┐
│ formatter│──▶│ align_equals │
└──────────┘   └──────────────┘
```

- **Lexer** — hand-written scanner, one pass, tokens point into the source buffer (no copies).
- **Parser** — recursive descent, one-token lookahead. First error wins; the parser unwinds cleanly.
- **Arena** — bump allocator. All AST nodes come from a single arena owned by the document.
- **Formatter** — re-emits the token stream with canonical whitespace. Does **not** build an AST, so it tolerates malformed input.
- **`align_equals`** — post-processor over the formatter's output. Pads keys so `=` aligns within contiguous runs.
- **JSON** — walks the AST and emits compact JSON.

The design trade-offs are documented at the top of each section in `src/chcl.c`.

---

## Building from source

**Requirements:** a C11 compiler (GCC or Clang), GNU Make, Bash and basic POSIX test utilities. The C library itself has no third-party dependencies. MSVC and Windows Unicode paths are not yet verified.

```bash
make              # builds ./chcl
make test         # runs the smoke tests
make install      # installs to /usr/local/bin (override with PREFIX=...)
make clean        # removes build artifacts
```

Override the compiler or flags:

```bash
make CC=clang CFLAGS="-O3 -march=native -Wall -Wextra"
```

---

## Testing

```bash
make test
```

Runs native API/allocation-failure regressions in `tests/test_core.c`, then CLI regressions in `tests/test_basic.sh`, covering:

- `lint` on valid and invalid input
- `fmt` output stability
- `fmt --check` exit codes
- `to-json` on the sample file

Optional golden fixtures can be added under `tests/cases/`: each `foo.hcl` needs
`foo.hcl.expected`. The shell suite discovers these pairs; basic formatting and
heredoc-preservation fixtures are bundled.
Core checks also cover malformed literals, operators, nesting limits, Unicode
escapes, invalid UTF-8, heredoc preservation, repeated blocks and failure at each
allocation. Generated checks compare ASTs before/after formatting for 960 expression
cases; 5,000 deterministic malformed byte inputs exercise parser/formatter cleanup.
CI is configured for GCC and Clang, plus AddressSanitizer/UndefinedBehaviorSanitizer
and leak checks on Linux. These sanitizer runs require compiler runtime support.
Builds and regressions have also been verified locally on Windows/MinGW and on
Linux/GCC with all three sanitizer checks enabled.

Limits: input and generated output are bounded by `INT_MAX / 4` bytes; parsing
uses at most 128 recursive parser frames and JSON serialization at most 512
node levels. Embedded NUL bytes and escaped U+0000 are rejected. Interpolations
and list `for` expressions are preserved as text, not evaluated or fully
validated. This remains an HCL subset, not a replacement for Terraform validation.

---

## Roadmap

- [x] HCL subset parser (attributes, blocks, strings, numbers, lists, objects)
- [x] `fmt`, `lint`, `to-json`
- [x] `fmt --check` CI mode
- [x] Alignment of `=` within blocks
- [x] Basic expressions: binary operators, conditionals, function calls
- [x] Heredoc support (`<<EOF ... EOF`)
- [x] Interpolation support (preserved as strings)
- [x] Preserve blank lines and comments in `fmt` output
- [ ] Configurable lint rules via `.chcllint` file
- [ ] `chcl get <path>` — jq-style query on the AST
- [ ] Pre-built binaries via GitHub Releases
- [ ] Homebrew formula, `.deb`, `.rpm`
- [ ] MSVC / Windows CI target

See the [issues](https://github.com/Sahkaara/chcl/issues) for the live roadmap.

---

## Contributing

Contributions are very welcome — bug reports, feature requests, docs, tests, code.

1. **Open an issue first** for non-trivial changes. It saves you from writing code that won't be merged.
2. **Fork, branch, PR.** Keep PRs focused: one feature or fix per PR.
3. **Follow the existing style.** 4-space indent, snake_case functions, English comments.
4. **Add a test.** If you're fixing a bug, add a test that fails before your fix.
5. **Run `make test` before pushing.**

Good first issues are labeled [`good first issue`](https://github.com/Sahkaara/chcl/labels/good%20first%20issue).

---

## License

MIT — see [LICENSE](LICENSE).

---

## Acknowledgments

- Inspired by [`terraform fmt`](https://developer.hashicorp.com/terraform/cli/commands/fmt) and [`gofmt`](https://pkg.go.dev/cmd/gofmt).
- HCL grammar reference: [hashicorp/hcl](https://github.com/hashicorp/hcl).
- Design ideas borrowed from [`tomlc17`](https://github.com/cktan/tomlc17) (single-file C parsers done right).