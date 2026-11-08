const vscode = require("vscode");
const {
  LanguageClient,
  TransportKind,
} = require("vscode-languageclient/node");

let client;
let restartQueue = Promise.resolve();

function configuredPath(name, fallback) {
  const value = vscode.workspace.getConfiguration("flecha").get(name);
  return typeof value === "string" && value.trim().length > 0
    ? value.trim()
    : fallback;
}

function createClient() {
  const serverPath = configuredPath("server.path", "flecha-lsp");
  const compilerPath = configuredPath("compiler.path", "flecha");
  const serverOptions = {
    command: serverPath,
    args: [],
    transport: TransportKind.stdio,
    options: {
      env: {
        ...process.env,
        FLECHA_COMPILER: compilerPath,
      },
    },
  };
  const clientOptions = {
    documentSelector: [{ scheme: "file", language: "flecha" }],
    diagnosticCollectionName: "flecha",
    outputChannelName: "Flecha Language Server",
  };
  return new LanguageClient(
    "flecha",
    "Flecha Language Server",
    serverOptions,
    clientOptions,
  );
}

async function startClient() {
  client = createClient();
  await client.start();
}

async function stopClient() {
  const running = client;
  client = undefined;
  if (running) {
    await running.stop();
  }
}

function reportStartFailure(error) {
  const detail = error instanceof Error ? error.message : String(error);
  void vscode.window.showErrorMessage(
    `Flecha language server failed to start: ${detail}`,
  );
}

function restartClient() {
  restartQueue = restartQueue
    .catch(() => undefined)
    .then(async () => {
      await stopClient();
      await startClient();
    });
  restartQueue.catch(reportStartFailure);
  return restartQueue;
}

async function activate(context) {
  context.subscriptions.push(
    vscode.commands.registerCommand(
      "flecha.restartLanguageServer",
      restartClient,
    ),
  );
  context.subscriptions.push(
    vscode.workspace.onDidChangeConfiguration((event) => {
      if (
        event.affectsConfiguration("flecha.server.path") ||
        event.affectsConfiguration("flecha.compiler.path")
      ) {
        void restartClient();
      }
    }),
  );

  try {
    await startClient();
  } catch (error) {
    reportStartFailure(error);
  }
}

async function deactivate() {
  await stopClient();
}

module.exports = { activate, deactivate };
