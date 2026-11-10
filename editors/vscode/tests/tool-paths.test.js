const assert = require("node:assert/strict");
const path = require("node:path");
const test = require("node:test");

const {
  executableName,
  installedToolCandidate,
  isBundledExtensionPath,
  resolveTools,
} = require("../tool-paths");

function configuration(values = {}) {
  return { get: (setting) => values[setting] };
}

test("explicit configuration takes precedence over the release bundle", () => {
  const result = resolveTools({
    configuration: configuration({
      "server.path": "  /custom/flecha-lsp  ",
      "compiler.path": "  /custom/flecha  ",
    }),
    extensionPath: "/release/share/flecha/editors/vscode",
    usable: () => true,
  });

  assert.deepEqual(result, {
    serverPath: "/custom/flecha-lsp",
    compilerPath: "/custom/flecha",
  });
});

test("installed support finds tools relative to a relocated release", () => {
  const extensionPath = path.join(
    path.parse(process.cwd()).root,
    "relocated",
    "flecha",
    "share",
    "flecha",
    "editors",
    "vscode",
  );
  const expectedServer = path.join(
    path.parse(process.cwd()).root,
    "relocated",
    "flecha",
    "bin",
    executableName("flecha-lsp"),
  );
  const expectedCompiler = path.join(
    path.parse(process.cwd()).root,
    "relocated",
    "flecha",
    "bin",
    executableName("flecha"),
  );

  assert.equal(
    installedToolCandidate(extensionPath, "flecha-lsp"),
    expectedServer,
  );
  assert.deepEqual(
    resolveTools({
      configuration: configuration(),
      extensionPath,
      usable: (candidate) =>
        candidate === expectedServer || candidate === expectedCompiler,
    }),
    { serverPath: expectedServer, compilerPath: expectedCompiler },
  );
});

test("a standalone VSIX prefers the server beside the PATH compiler", () => {
  const available = new Set([
    "/old/bin/flecha-lsp",
    "/current/bin/flecha",
    "/current/bin/flecha-lsp",
  ]);
  const result = resolveTools({
    configuration: configuration(),
    extensionPath: "/opt/company/vscode/extensions/flecha.flecha-language",
    environmentPath: "/old/bin:/current/bin",
    usable: (candidate) => available.has(candidate),
  });

  assert.deepEqual(result, {
    serverPath: "/current/bin/flecha-lsp",
    compilerPath: "/current/bin/flecha",
  });
});

test("an incomplete release bundle falls back to a complete PATH pair", () => {
  const extensionPath = "/release/share/flecha/editors/vscode";
  const server = "/release/bin/flecha-lsp";
  const pathServer = "/current/bin/flecha-lsp";
  const pathCompiler = "/current/bin/flecha";
  const result = resolveTools({
    configuration: configuration(),
    extensionPath,
    environmentPath: "/current/bin",
    usable: (candidate) =>
      candidate === server ||
      candidate === pathServer ||
      candidate === pathCompiler,
  });

  assert.deepEqual(result, {
    serverPath: pathServer,
    compilerPath: pathCompiler,
  });
});

test("a configured compiler selects its sibling server", () => {
  const result = resolveTools({
    configuration: configuration({
      "compiler.path": "/selected/bin/flecha",
    }),
    extensionPath: "/extension",
    usable: (candidate) => candidate === "/selected/bin/flecha-lsp",
  });

  assert.deepEqual(result, {
    serverPath: "/selected/bin/flecha-lsp",
    compilerPath: "/selected/bin/flecha",
  });
});

test("discovery rejects unmatched standalone executables", () => {
  assert.throws(
    () =>
      resolveTools({
        configuration: configuration(),
        extensionPath: "/extension",
        environmentPath: "/old/bin:/compiler/bin",
        usable: (candidate) =>
          candidate === "/old/bin/flecha-lsp" ||
          candidate === "/compiler/bin/flecha",
      }),
    /matching flecha\/flecha-lsp installation/,
  );
});

test("Windows candidates use Windows paths and executable suffixes", () => {
  const extensionPath = "C:\\release\\share\\flecha\\editors\\vscode";
  assert.equal(executableName("flecha", "win32"), "flecha.exe");
  assert.equal(
    installedToolCandidate(extensionPath, "flecha-lsp", "win32"),
    "C:\\release\\bin\\flecha-lsp.exe",
  );
  assert.equal(isBundledExtensionPath(extensionPath, "win32"), true);
});
