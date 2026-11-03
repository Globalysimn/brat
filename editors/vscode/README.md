# BRAT for Visual Studio Code

Provides TextMate syntax highlighting for `.brat` and `.bri` files, bracket and
comment configuration, and a client for `brat-cxx lsp` (diagnostics, quick
fixes, hover types, completions, go-to-definition, cross-file rename, find
references, inlay hints, semantic tokens, formatting and code folding).

## Setup

1. Build `lang/cxx/bin/brat-cxx` in your BRAT checkout (`make -j2`) or add
   `brat-cxx` to `PATH`.
2. Install extension dependencies in `editors/vscode/`:
   ```sh
   npm install
   ```
3. Press `F5` in VS Code to open an Extension Development Host, or package a
   `.vsix` archive with `npx @vscode/vsce package`.
