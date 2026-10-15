# kakoune-lsp-cpp

A small, dependency-light **Language Server Protocol client for
[Kakoune](https://kakoune.org)**, written in C++20.

It is a from-scratch alternative to the excellent (Rust)
[`kakoune-lsp`](https://github.com/kakoune-lsp/kakoune-lsp). Because LSP
language support comes from the *language servers* (`clangd`,
`rust-analyzer`, `gopls`, `pylsp`, …), a single generic client supports every
language — there is no per-language code.

## Status

Early, but the core works:

- [x] LSP transport over stdio (`Content-Length` framing, `nlohmann/json`)
- [x] Language server subprocess management
- [x] `initialize` / `initialized` / `shutdown` handshake
- [x] `textDocument/didOpen`, `textDocument/hover`
- [ ] `--daemon` mode + Kakoune FIFO integration (`rc/kakoune-lsp-cpp.kak`)
- [ ] `didChange` + `publishDiagnostics`
- [ ] go-to-definition, references, rename, formatting, completion

The `--probe` mode is a working end-to-end smoke test against a real server:

```sh
cmake -S . -B build
cmake --build build -j

./build/kakoune-lsp-cpp --probe --server clangd --file test/sample.cc --line 5 --col 13
# initialize serverInfo: {"name":"clangd", ...}
# hover: {"contents": { ... "int add(int a, int b)" ... }}
```

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
