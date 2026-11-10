const vscode = require("vscode");
const {
  LanguageClient,
  TransportKind,
} = require("vscode-languageclient/node");
const { resolveTools } = require("./tool-paths");

let client;
let restartQueue = Promise.resolve();

function createClient(context) {
  const configuration = vscode.workspace.getConfiguration("flecha");
  const { serverPath, compilerPath } = resolveTools({
    configuration,
    extensionPath: context.extensionPath,
  });
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

async function startClient(context) {
  client = createClient(context);
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

function restartClient(context) {
  restartQueue = restartQueue
    .catch(() => undefined)
    .then(async () => {
      await stopClient();
      await startClient(context);
    });
  restartQueue.catch(reportStartFailure);
  return restartQueue;
}

async function activate(context) {
  context.subscriptions.push(
    vscode.commands.registerCommand(
      "flecha.restartLanguageServer",
      () => restartClient(context),
    ),
  );
  context.subscriptions.push(
    vscode.workspace.onDidChangeConfiguration((event) => {
      if (
        event.affectsConfiguration("flecha.server.path") ||
        event.affectsConfiguration("flecha.compiler.path")
      ) {
        void restartClient(context);
      }
    }),
  );

  try {
    await startClient(context);
  } catch (error) {
    reportStartFailure(error);
  }
}

async function deactivate() {
  await stopClient();
}

module.exports = { activate, deactivate };
