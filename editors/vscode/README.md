# BRAT for Visual Studio Code

Syntax highlighting, snippets, bracket/comment configuration, and native `brat-cxx lsp` integration for `.brat` and `.bri` files.

## Features

- Live borrow, ownership, region, FFI, and type diagnostics (`E0xxx`, `T0xxx`, `P0xxx`) with quick fixes
- Hover signatures, dot completions (`std.`, structs, `impl` methods, `vec`, `map`, `str`), signature help, inlay hints, and semantic tokens
- Go to definition, find references, workspace symbols, `prepareRename` + cross-file rename, folding, and lossless source formatting
- `.bri` module interface parsing with outline symbols and syntax diagnostics
- Commands: `BRAT: Restart Language Server`, `BRAT: Run Current File`, `BRAT: Check Current File`, and `BRAT: Show -O2 Optimization Proofs`

## Setup

1. Build `lang/cxx/bin/brat-cxx` in the repository root (`make -j4`) or place `brat-cxx` on `PATH`.
2. Install extension dependencies in `editors/vscode/`:
   ```sh
   npm install
   ```
3. Press `F5` in VS Code to launch an Extension Development Host, or package a `.vsix` archive:
   ```sh
   npx @vscode/vsce package
   ```
