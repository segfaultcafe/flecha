const fs = require("fs");
const path = require("path");

// CMake installs the extension in share/flecha/editors/vscode and the tools in
// bin. Keep this relative so moving an extracted release does not break it.
const FLECHA_BUNDLED_SUPPORT_LAYOUT = ["share", "flecha", "editors", "vscode"];
const FLECHA_BUNDLED_TOOL_LAYOUT = ["..", "..", "..", "..", "bin"];

function pathImplementation(platform = process.platform) {
  return platform === "win32" ? path.win32 : path.posix;
}

function executableName(name, platform = process.platform) {
  return platform === "win32" ? `${name}.exe` : name;
}

function installedToolCandidate(extensionPath, name, platform = process.platform) {
  const platformPath = pathImplementation(platform);
  return platformPath.resolve(
    extensionPath,
    ...FLECHA_BUNDLED_TOOL_LAYOUT,
    executableName(name, platform),
  );
}

function isBundledExtensionPath(extensionPath, platform = process.platform) {
  const platformPath = pathImplementation(platform);
  if (!platformPath.isAbsolute(extensionPath)) {
    return false;
  }

  const normalized = platformPath.normalize(extensionPath);
  const suffix = platformPath.join(...FLECHA_BUNDLED_SUPPORT_LAYOUT);
  const comparable = platform === "win32" ? normalized.toLowerCase() : normalized;
  const comparableSuffix = platform === "win32" ? suffix.toLowerCase() : suffix;
  return comparable.endsWith(`${platformPath.sep}${comparableSuffix}`);
}

function isExecutable(candidate, platform = process.platform) {
  try {
    if (!fs.statSync(candidate).isFile()) {
      return false;
    }
    fs.accessSync(
      candidate,
      platform === "win32" ? fs.constants.F_OK : fs.constants.X_OK,
    );
    return true;
  } catch {
    return false;
  }
}

function configuredPath(configuration, setting) {
  const configured = configuration.get(setting);
  return typeof configured === "string" && configured.trim().length > 0
    ? configured.trim()
    : undefined;
}

function siblingTool(candidate, name, platform, usable) {
  const platformPath = pathImplementation(platform);
  if (!candidate || platformPath.dirname(candidate) === ".") {
    return undefined;
  }
  const sibling = platformPath.join(
    platformPath.dirname(candidate),
    executableName(name, platform),
  );
  return usable(sibling) ? sibling : undefined;
}

function toolOnPath(name, platform, environmentPath, usable) {
  const platformPath = pathImplementation(platform);
  for (const entry of environmentPath.split(platformPath.delimiter)) {
    const directory = entry.replace(/^"|"$/g, "").trim();
    if (!directory) {
      continue;
    }
    const candidate = platformPath.join(
      directory,
      executableName(name, platform),
    );
    if (usable(candidate)) {
      return candidate;
    }
  }
  return undefined;
}

function resolveTools({
  configuration,
  extensionPath,
  platform = process.platform,
  environmentPath = process.env.PATH ?? "",
  usable = (candidate) => isExecutable(candidate, platform),
}) {
  const configuredServer = configuredPath(configuration, "server.path");
  const configuredCompiler = configuredPath(configuration, "compiler.path");
  if (configuredServer && configuredCompiler) {
    return { serverPath: configuredServer, compilerPath: configuredCompiler };
  }

  if (configuredCompiler) {
    const siblingServer = siblingTool(
      configuredCompiler,
      "flecha-lsp",
      platform,
      usable,
    );
    if (siblingServer) {
      return { serverPath: siblingServer, compilerPath: configuredCompiler };
    }
  }
  if (configuredServer) {
    const siblingCompiler = siblingTool(
      configuredServer,
      "flecha",
      platform,
      usable,
    );
    if (siblingCompiler) {
      return { serverPath: configuredServer, compilerPath: siblingCompiler };
    }
  }

  const installedServer = installedToolCandidate(
    extensionPath,
    "flecha-lsp",
    platform,
  );
  const installedCompiler = installedToolCandidate(
    extensionPath,
    "flecha",
    platform,
  );
  const useBundle =
    isBundledExtensionPath(extensionPath, platform) &&
    usable(installedServer) &&
    usable(installedCompiler);

  if (useBundle) {
    return { serverPath: installedServer, compilerPath: installedCompiler };
  }

  // Resolve the compiler first, then select its sibling server. This keeps a
  // stale standalone flecha-lsp earlier on PATH from shadowing the matching
  // server distributed with the selected compiler.
  const pathCompiler = toolOnPath(
    "flecha",
    platform,
    environmentPath,
    usable,
  );
  const compilerServer = siblingTool(
    pathCompiler,
    "flecha-lsp",
    platform,
    usable,
  );
  if (compilerServer) {
    return { serverPath: compilerServer, compilerPath: pathCompiler };
  }

  const pathServer = toolOnPath(
    "flecha-lsp",
    platform,
    environmentPath,
    usable,
  );
  const serverCompiler = siblingTool(pathServer, "flecha", platform, usable);
  if (serverCompiler) {
    return { serverPath: pathServer, compilerPath: serverCompiler };
  }

  throw new Error(
    "Flecha could not find a matching flecha/flecha-lsp installation; " +
      "install a release bundle or configure both executable paths",
  );
}

module.exports = {
  executableName,
  installedToolCandidate,
  isBundledExtensionPath,
  isExecutable,
  resolveTools,
  toolOnPath,
};
