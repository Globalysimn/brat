"use strict";

const cp = require("child_process");
const fs = require("fs");
const path = require("path");
const vscode = require("vscode");
const { LanguageClient, TransportKind } = require("vscode-languageclient/node");

let client = null;
let outputChannel = null;
let runTerminal = null;

function isExecutableFile(candidate) {
  if (!candidate) {
    return false;
  }
  try {
    const st = fs.statSync(candidate);
    if (!st.isFile()) {
      return false;
    }
    if (process.platform !== "win32") {
      fs.accessSync(candidate, fs.constants.X_OK);
    }
    return true;
  } catch (_) {
    return false;
  }
}

function expandWorkspaceVars(raw, folders) {
  if (!raw || !raw.includes("${workspaceFolder}")) {
    return raw;
  }
  const first = folders && folders.length > 0 ? folders[0].uri.fsPath : "";
  if (!first) {
    return raw;
  }
  return raw.replace(/\$\{workspaceFolder\}/g, first);
}

function findInCheckoutAncestors(startDir, isWin) {
  const exeName = isWin ? "brat-cxx.exe" : "brat-cxx";
  let dir = path.resolve(startDir);
  for (let depth = 0; depth < 6; depth++) {
    const cxxBin = path.join(dir, "lang", "cxx", "bin", exeName);
    if (isExecutableFile(cxxBin)) {
      return cxxBin;
    }
    if (!isWin) {
      const unixBin = path.join(dir, "bin", "brat");
      if (isExecutableFile(unixBin)) {
        return unixBin;
      }
    }
    const parent = path.dirname(dir);
    if (parent === dir) {
      break;
    }
    dir = parent;
  }
  return null;
}

function resolveServerBinary() {
  const cfg = vscode.workspace.getConfiguration("brat");
  const folders = vscode.workspace.workspaceFolders || [];
  const inspected = cfg.inspect("lsp.path") || {};
  const globalExplicit = [inspected.globalValue, inspected.globalLanguageValue]
    .find((value) => typeof value === "string" && value.trim());
  if (globalExplicit) {
    return globalExplicit.trim();
  }
  if (!vscode.workspace.isTrusted) {
    return null;
  }
  const explicit = expandWorkspaceVars((cfg.get("lsp.path") || "").trim(), folders);
  if (explicit) {
    return explicit;
  }
  const isWin = process.platform === "win32";
  for (const folder of folders) {
    const found = findInCheckoutAncestors(folder.uri.fsPath, isWin);
    if (found) {
      return found;
    }
  }
  const cliNames = isWin ? ["brat-cxx.exe", "brat.exe"] : ["brat-cxx", "brat"];
  const pathDirs = (process.env.PATH || "").split(path.delimiter).filter(Boolean);
  for (const name of cliNames) {
    for (const dir of pathDirs) {
      const full = path.join(dir, name);
      if (isExecutableFile(full)) {
        return full;
      }
    }
  }
  return null;
}

function getOutputChannel(context) {
  if (!outputChannel) {
    outputChannel = vscode.window.createOutputChannel("BRAT Language Server");
    context.subscriptions.push(outputChannel);
  }
  return outputChannel;
}

async function stopClient() {
  if (!client) {
    return;
  }
  const running = client;
  client = null;
  try {
    if (typeof running.needsStop === "function" ? running.needsStop() : true) {
      await running.stop();
    }
  } catch (err) {
    if (outputChannel) {
      outputChannel.appendLine(`[brat-lsp] stop error: ${err && err.message ? err.message : String(err)}`);
    }
  }
}

async function startClient(context, warnIfUnavailable = false) {
  if (client) {
    return;
  }
  const out = getOutputChannel(context);
  const command = resolveServerBinary();
  if (!command) {
    const reason = vscode.workspace.isTrusted
      ? "BRAT: brat-cxx executable not found. Run `make -j4` in the BRAT checkout or set `brat.lsp.path`."
      : "BRAT: language server not started in Restricted Mode. Trust this workspace or set user-level `brat.lsp.path`.";
    out.appendLine(reason);
    if (warnIfUnavailable) {
      await vscode.window.showWarningMessage(reason);
    }
    return;
  }
  const serverOptions = {
    command,
    args: ["lsp"],
    transport: TransportKind.stdio,
  };
  const clientOptions = {
    documentSelector: [
      { scheme: "file", language: "brat" },
      { scheme: "untitled", language: "brat" },
    ],
    outputChannel: out,
    synchronize: {
      fileEvents: vscode.workspace.createFileSystemWatcher("**/*.{brat,bri}"),
    },
  };
  const nextClient = new LanguageClient("brat-lsp", "BRAT Language Server", serverOptions, clientOptions);
  client = nextClient;
  try {
    await nextClient.start();
  } catch (err) {
    if (client === nextClient) {
      client = null;
    }
    const detail = err && err.message ? err.message : String(err);
    out.appendLine(`[brat-lsp] failed to start '${command} lsp': ${detail}`);
    if (warnIfUnavailable) {
      await vscode.window.showErrorMessage(`BRAT language server failed to start: ${detail}`);
    }
  }
}

function activeBratFilePath() {
  const editor = vscode.window.activeTextEditor;
  if (!editor || editor.document.uri.scheme !== "file") {
    vscode.window.showWarningMessage("Open a saved .brat file first.");
    return null;
  }
  return editor.document.uri.fsPath;
}

function execBratTool(command, args, cwd) {
  return new Promise((resolve) => {
    cp.execFile(command, args, { cwd, maxBuffer: 16 * 1024 * 1024 }, (err, stdout, stderr) => {
      resolve({ err, stdout: stdout || "", stderr: stderr || "" });
    });
  });
}

async function activate(context) {
  context.subscriptions.push(
    vscode.commands.registerCommand("brat.restartServer", async () => {
      await stopClient();
      await startClient(context, true);
    }),
    vscode.commands.registerCommand("brat.runFile", async () => {
      const filePath = activeBratFilePath();
      if (!filePath) {
        return;
      }
      await vscode.window.activeTextEditor.document.save();
      const bin = resolveServerBinary();
      if (!bin) {
        await vscode.window.showWarningMessage("BRAT binary not found. Build `brat-cxx` or set `brat.lsp.path`.");
        return;
      }
      if (!runTerminal || runTerminal.exitStatus !== undefined) {
        runTerminal = vscode.window.createTerminal("BRAT");
      }
      runTerminal.show(true);
      const quote = (s) => (process.platform === "win32" ? `"${s}"` : `'${s.replace(/'/g, "'\\''")}'`);
      runTerminal.sendText(`${quote(bin)} run ${quote(filePath)}`);
    }),
    vscode.commands.registerCommand("brat.checkFile", async () => {
      const filePath = activeBratFilePath();
      if (!filePath) {
        return;
      }
      await vscode.window.activeTextEditor.document.save();
      const bin = resolveServerBinary();
      if (!bin) {
        await vscode.window.showWarningMessage("BRAT binary not found. Build `brat-cxx` or set `brat.lsp.path`.");
        return;
      }
      const out = getOutputChannel(context);
      const { err, stdout, stderr } = await execBratTool(bin, ["check", filePath], path.dirname(filePath));
      out.appendLine(`$ ${bin} check ${filePath}`);
      if (stdout.trim()) {
        out.appendLine(stdout.trimEnd());
      }
      if (stderr.trim()) {
        out.appendLine(stderr.trimEnd());
      }
      out.show(true);
      if (!err) {
        vscode.window.setStatusBarMessage("BRAT check: 0 errors", 3000);
      }
    }),
    vscode.commands.registerCommand("brat.showOptProofs", async () => {
      const filePath = activeBratFilePath();
      if (!filePath) {
        return;
      }
      await vscode.window.activeTextEditor.document.save();
      const bin = resolveServerBinary();
      if (!bin) {
        await vscode.window.showWarningMessage("BRAT binary not found. Build `brat-cxx` or set `brat.lsp.path`.");
        return;
      }
      const { err, stderr } = await execBratTool(
        bin,
        ["cc", "-O2", "--why-opt", filePath],
        path.dirname(filePath)
      );
      if (err && !stderr.trim()) {
        await vscode.window.showErrorMessage(`BRAT -O2 proof run failed: ${err.message}`);
        return;
      }
      const content = stderr.trim()
        ? `// -O2 proof certificates for ${path.basename(filePath)}\n\n${stderr.trimEnd()}\n`
        : `// No division/modulo or bounds checks were elided in ${path.basename(filePath)} at -O2.\n`;
      const doc = await vscode.workspace.openTextDocument({ content, language: "plaintext" });
      await vscode.window.showTextDocument(doc, { preview: true, viewColumn: vscode.ViewColumn.Beside });
    })
  );

  if (vscode.workspace.onDidGrantWorkspaceTrust) {
    context.subscriptions.push(
      vscode.workspace.onDidGrantWorkspaceTrust(() => startClient(context))
    );
  }
  context.subscriptions.push(
    vscode.workspace.onDidChangeConfiguration(async (event) => {
      if (event.affectsConfiguration("brat.lsp.path")) {
        await stopClient();
        await startClient(context, true);
      }
    })
  );

  await startClient(context);
}

async function deactivate() {
  await stopClient();
}

module.exports = { activate, deactivate, resolveServerBinary, isExecutableFile, findInCheckoutAncestors };
