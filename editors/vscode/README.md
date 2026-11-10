# BRAT for VS Code

Syntax highlighting and language-server client (`brat lsp`) for `.brat` and
`.bri` files.

## Features

- TextMate syntax grammar for BRAT v0.7 (`fn`, `struct`, `enum`, `trait`,
  `impl`, `arena`, `own`, `par for`, `extern`, `export fn`, `ptr[T]`, `move`,
  `mut`, `?`).
- Diagnostics, hover types, go-to-definition, document symbols, completion, and
  formatting backed by `brat-cxx lsp` over stdio.
- Commands: `BRAT: Check Current File` and `BRAT: Run Current File`.

## Setup

1. Build `brat-cxx` (`make -j4` at the repository root) and either add `bin/`
   to `PATH` or set `"brat.serverPath"` in VS Code settings to the path of
   `brat-cxx`.
2. Open `editors/vscode` in VS Code and press `F5` (Extension Development Host),
   or symlink/copy `editors/vscode` into `~/.vscode/extensions/brat-lang`.
