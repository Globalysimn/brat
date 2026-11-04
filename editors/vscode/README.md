# BRAT for Visual Studio Code

Syntax highlighting (`.brat`, `.bri`), bracket/indentation configuration, and native `brat-cxx lsp` integration (diagnostics, quickfixes, hover, completion, go-to-definition, cross-file rename, references, inlay hints, semantic tokens, formatting, and folding).

## Setup

1. Build `brat-cxx` in your BRAT checkout (or place `brat-cxx` on `PATH`).
2. Install dependencies inside `editors/vscode/`:
   ```sh
   npm install
   ```
3. Launch an Extension Development Host (`F5` in VS Code) or package with `vsce package`.
