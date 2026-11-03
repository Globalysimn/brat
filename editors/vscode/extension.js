"use strict";

const fs = require("fs");
const path = require("path");
const vscode = require("vscode");
const { LanguageClient, TransportKind } = require("vscode-languageclient/node");

let client = null;

function resolveServerBinary() {
  const cfg = vscode.workspace.getConfiguration("brat");
  // A workspace can carry both a server binary and settings naming one.
  // In Restricted Mode neither is user consent to execute code, so only a
  // path from the user's own settings is honored there.
  const inspected = cfg.inspect("lsp.path") || {};
  const globalExplicit = [inspected.globalValue, inspected.globalLanguageValue]
    .find((value) => typeof value === "string" && value.trim());
  if (globalExplicit) {
    return globalExplicit.trim();
  }
  if (!vscode.workspace.isTrusted) {
    return null;
  }
  const explicit = (cfg.get("lsp.path") || "").trim();
  if (explicit) {
    return explicit;
  }
  const folders = vscode.workspace.workspaceFolders || [];
  const isWin = process.platform === "win32";
  const exeName = isWin ? "brat-cxx.exe" : "brat-cxx";
  const cliNames = isWin ? ["brat.exe", "brat.cmd", exeName] : ["brat", exeName];
  for (const folder of folders) {
    const candidate = path.join(folder.uri.fsPath, "lang", "cxx", "bin", exeName);
    if (fs.existsSync(candidate)) {
      return candidate;
    }
    const binCand = path.join(folder.uri.fsPath, "bin", isWin ? "brat.cmd" : "brat");
    if (fs.existsSync(binCand)) {
      return binCand;
    }
  }
  const pathDirs = (process.env.PATH || "").split(path.delimiter).filter(Boolean);
  for (const name of cliNames) {
    for (const dir of pathDirs) {
      const full = path.join(dir, name);
      if (fs.existsSync(full)) {
        return full;
      }
    }
  }
  return isWin ? "brat.cmd" : "brat";
}

async function startClient(context, warnIfUnavailable = false) {
  if (client) {
    return;
  }
  const command = resolveServerBinary();
  if (!command) {
    const message =
      "BRAT: language server not started in Restricted Mode. Trust this workspace or configure brat.lsp.path in user settings.";
    if (warnIfUnavailable) {
      await vscode.window.showWarningMessage(message);
    } else {
      console.warn(message);
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
    synchronize: {
      fileEvents: vscode.workspace.createFileSystemWatcher("**/*.{brat,bri}"),
    },
  };
  client = new LanguageClient("brat-lsp", "BRAT Language Server", serverOptions, clientOptions);
  context.subscriptions.push(client);
  await client.start();
}

async function activate(context) {
  context.subscriptions.push(
    vscode.commands.registerCommand("brat.restartServer", async () => {
      if (client) {
        await client.stop();
        client = null;
      }
      await startClient(context, true);
    })
  );
  if (vscode.workspace.onDidGrantWorkspaceTrust) {
    context.subscriptions.push(
      vscode.workspace.onDidGrantWorkspaceTrust(() => startClient(context))
    );
  }
  await startClient(context);
}

async function deactivate() {
  if (!client) {
    return undefined;
  }
  const running = client;
  client = null;
  return running.stop();
}

module.exports = { activate, deactivate, resolveServerBinary };
