# kakoune-lsp-cpp

A small, dependency-light **Language Server Protocol client for
[Kakoune](https://kakoune.org)**, written in C++20.

It is a from-scratch alternative to the excellent (Rust)
[`kakoune-lsp`](https://github.com/kakoune-lsp/kakoune-lsp). Because LSP
language support comes from the *language servers* (`clangd`,
`rust-analyzer`, `gopls`, `pylsp`, …), a single generic client supports every
language — there is no per-language code.

## Status

Early, but usable:

- [x] LSP transport over stdio (`Content-Length` framing, `nlohmann/json`)
- [x] Language server subprocess management (threaded reader)
- [x] `initialize` / `initialized` / `shutdown` handshake
- [x] `--daemon` mode + Kakoune integration (`rc/kakoune-lsp-cpp.kak`)
- [x] `didOpen` / `didChange` (full sync) and `publishDiagnostics`
- [x] `textDocument/hover` and `textDocument/definition`, `textDocument/references`
- [ ] rename, formatting, completion, code actions, document symbols

Verified end-to-end against `clangd`: opening a C++ buffer syncs the document,
diagnostics are shown on the status line, `:lsp-hover` shows real hover docs,
and `:lsp-definition` jumps to the definition.

The `--probe` mode is a standalone smoke test against any server:

```sh
cmake -S . -B build
cmake --build build -j

./build/kakoune-lsp-cpp --probe --server clangd --file test/sample.cc --line 5 --col 13
# initialize serverInfo: {"name":"clangd", ...}
# hover: {"contents": { ... "int add(int a, int b)" ... }}
```

### Using it in Kakoune

1. Install the binary (`sudo cmake --install build`).
2. Drop `rc/kakoune-lsp-cpp.kak` into `~/.config/kak/autoload/`, or `source` it
   from `kakrc`.
3. Open a supported file: LSP auto-enables for `c`, `cpp`, `objc`, `rust`,
   `go`, `python` and `lua`.

Commands: `:lsp-enable`, `:lsp-disable`, `:lsp-hover`, `:lsp-definition`,
`:lsp-references`, `:lsp-stop`.

The filetype → server mapping lives in `src/daemon.cc` (defaults: `clangd`,
`rust-analyzer`, `gopls`, `pylsp`, `lua-language-server`). Set the `klsp_cmd`
option if the binary is not on `PATH`.

## Design

```
  Kakoune  ──(%sh / hooks)──▶  rc/kakoune-lsp-cpp.kak
                                      │ writes requests to a FIFO
                                      ▼
                              kakoune-lsp-cpp --daemon
                                      │  LSP (JSON-RPC over stdio)
                                      ▼
                              clangd / rust-analyzer / gopls / …

  kakoune-lsp-cpp ──(kak -p <session>)──▶ Kakoune   (display / edits / options)
```

- `src/lsp.{hh,cc}` — process management + JSON-RPC transport.
- `src/main.cc` — CLI (`--probe`, `--daemon`).
- `rc/kakoune-lsp-cpp.kak` — Kakoune glue (next milestone).

The daemon talks to Kakoune two ways, mirroring how `kak-lsp` works:
- **Kakoune → daemon**: commands written to a request FIFO; the current buffer
  is copied out with `write -force <fifo>` so unsaved edits are included.
- **daemon → Kakoune**: `kak -p <session>`, e.g. to echo hover markup, set a
  diagnostics option, or apply a rename.

## Building

Requires a C++20 compiler and CMake ≥ 3.16. `nlohmann/json` is vendored in
`third_party/`, so there are no external dependencies.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
sudo cmake --install build      # optional
```

## License

[MIT](LICENSE)
