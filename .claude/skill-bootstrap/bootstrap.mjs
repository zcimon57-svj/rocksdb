#!/usr/bin/env node
import crypto from "node:crypto";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const BOOTSTRAP_DIR = path.dirname(fileURLToPath(import.meta.url));
const PLACEHOLDERS = {
  bootstrap_market_command: new Set(["registry"]),
  skill_install_command_template: new Set(["cli_package", "agentcentor_skill", "version", "client", "package_store"])
};

function ensureDir(dir) {
  fs.mkdirSync(dir, { recursive: true });
}

function readText(file) {
  return fs.readFileSync(file, "utf8");
}

function writeJson(file, data) {
  ensureDir(path.dirname(file));
  fs.writeFileSync(file, `${JSON.stringify(data, null, 2)}\n`);
}

function listFiles(dir, base = dir) {
  if (!fs.existsSync(dir)) return [];
  const files = [];
  for (const entry of fs.readdirSync(dir, { withFileTypes: true }).sort((a, b) => a.name.localeCompare(b.name))) {
    const full = path.join(dir, entry.name);
    if (entry.isDirectory()) files.push(...listFiles(full, base));
    if (entry.isFile()) files.push(path.relative(base, full).split(path.sep).join("/"));
  }
  return files;
}

function stableHashDir(dir) {
  const hash = crypto.createHash("sha256");
  for (const file of listFiles(dir)) {
    hash.update(file);
    hash.update("\0");
    hash.update(fs.readFileSync(path.join(dir, file)));
    hash.update("\0");
  }
  return `sha256:${hash.digest("hex")}`;
}

function copyDir(from, to) {
  ensureDir(to);
  for (const entry of fs.readdirSync(from, { withFileTypes: true })) {
    const source = path.join(from, entry.name);
    const target = path.join(to, entry.name);
    if (entry.isDirectory()) copyDir(source, target);
    if (entry.isFile()) {
      ensureDir(path.dirname(target));
      fs.copyFileSync(source, target);
    }
  }
}

function removeManagedPath(target) {
  fs.rmSync(target, { recursive: true, force: true });
}

function scalar(raw) {
  const value = raw.trim();
  if (value === "true") return true;
  if (value === "false") return false;
  if (value === "null") return null;
  if (value === "[]") return [];
  if (/^-?\d+$/.test(value)) return Number(value);
  return value.replace(/^["']|["']$/g, "");
}

function parseYaml(text) {
  const lines = text.split(/\r?\n/);
  const root = {};
  const stack = [{ indent: -1, value: root }];

  function nextIsArray(index, indent) {
    for (let i = index + 1; i < lines.length; i += 1) {
      const line = lines[i];
      if (!line.trim() || line.trimStart().startsWith("#")) continue;
      return line.match(/^\s*/)[0].length > indent && line.trimStart().startsWith("- ");
    }
    return false;
  }

  for (let i = 0; i < lines.length; i += 1) {
    const raw = lines[i];
    if (!raw.trim() || raw.trimStart().startsWith("#")) continue;
    const indent = raw.match(/^\s*/)[0].length;
    const textLine = raw.trim();
    while (indent <= stack[stack.length - 1].indent) stack.pop();
    const parent = stack[stack.length - 1].value;

    if (textLine.startsWith("- ")) {
      if (!Array.isArray(parent)) throw new Error(`YAML array item without array parent: ${textLine}`);
      const item = textLine.slice(2);
      const match = item.match(/^([A-Za-z0-9_.-]+):\s*(.*)$/);
      if (!match) {
        parent.push(scalar(item));
        continue;
      }
      const object = {};
      parent.push(object);
      const [, key, value] = match;
      if (value === "") {
        const container = nextIsArray(i, indent) ? [] : {};
        object[key] = container;
        stack.push({ indent, value: object });
        stack.push({ indent: indent + 2, value: container });
      } else {
        object[key] = scalar(value);
        stack.push({ indent, value: object });
      }
      continue;
    }

    const match = textLine.match(/^([A-Za-z0-9_.-]+):\s*(.*)$/);
    if (!match) continue;
    const [, key, value] = match;
    if (value === "") {
      const container = nextIsArray(i, indent) ? [] : {};
      parent[key] = container;
      stack.push({ indent, value: container });
    } else {
      parent[key] = scalar(value);
    }
  }
  return root;
}

function readData(file) {
  const text = readText(file).trim();
  if (text.startsWith("{") || text.startsWith("[")) return JSON.parse(text);
  if (text.startsWith("---\n")) {
    const end = text.indexOf("\n---", 4);
    if (end < 0) throw new Error(`Invalid frontmatter: ${file}`);
    return parseYaml(text.slice(4, end));
  }
  return parseYaml(text);
}

function now() {
  return new Date().toISOString();
}

function runId(prefix) {
  return `${prefix}-${Date.now()}-${crypto.randomBytes(3).toString("hex")}`;
}

function commonReport(schemaVersion, run, status = "success") {
  return {
    schema_version: schemaVersion,
    run_id: run,
    generated_at: now(),
    status,
    blocking_errors: [],
    warnings: []
  };
}

function issue(code, message, blocksExternal, blocksActive) {
  return {
    code,
    message,
    blocks_external_validation_pass: Boolean(blocksExternal),
    blocks_active_required_activation: Boolean(blocksActive),
    blocks_claude_code_startup: false
  };
}

function repoPath(repoRoot, maybeRelative) {
  if (!maybeRelative) return repoRoot;
  if (path.isAbsolute(maybeRelative)) return maybeRelative;
  return path.resolve(repoRoot, maybeRelative);
}

function loadConfig(repoRoot) {
  return readData(path.join(repoRoot, "repo-skill-config.yml"));
}

function renderTemplate(template, values) {
  return template.replace(/\{([A-Za-z0-9_]+)\}/g, (_, key) => String(values[key] ?? ""));
}

function unknownPlaceholderIssues(config) {
  const issues = [];
  for (const [field, allowed] of Object.entries(PLACEHOLDERS)) {
    const template = field === "bootstrap_market_command"
      ? config.agentcentor?.bootstrap_market_command
      : config.agentcentor?.skill_install_command_template;
    for (const match of String(template || "").matchAll(/\{([A-Za-z0-9_]+)\}/g)) {
      if (!allowed.has(match[1])) {
        issues.push(issue("ADAPTER_UNKNOWN_PLACEHOLDER", `${field} uses unknown placeholder {${match[1]}}.`, true, true));
      }
    }
  }
  return issues;
}

function configIssues(config) {
  const issues = [];
  if (config.schema_version !== "repo-skill-config/v1") {
    issues.push(issue("ADAPTER_SCHEMA_VERSION_UNSUPPORTED", "repo-skill-config.yml schema_version is unsupported.", true, true));
  }
  if (config.agentcentor?.client !== "claudecode") {
    issues.push(issue("ADAPTER_CLIENT_UNSUPPORTED", "agentcentor.client must be claudecode for this validation.", true, true));
  }
  if (config.policy?.fail_open !== true || config.policy?.allow_global_install !== false || config.policy?.force_overwrite !== false) {
    issues.push(issue("ADAPTER_UNSAFE_POLICY", "Bootstrap policy must fail open without global or force overwrite defaults.", true, true));
  }
  return [...issues, ...unknownPlaceholderIssues(config)];
}

function cacheFile(config, repoRoot, key) {
  return path.join(repoPath(repoRoot, config.paths.cache_dir), "required-manifests", `${key}.yml`);
}

function fetchManifests(config, repoRoot) {
  const manifests = [];
  const warnings = [];
  let usedCache = false;
  let anyFailure = false;

  for (const key of ["department", "engine", "repo"]) {
    const source = config.required_manifests[key];
    const target = cacheFile(config, repoRoot, key);
    try {
      const sourcePath = repoPath(repoRoot, source);
      const raw = readText(sourcePath);
      ensureDir(path.dirname(target));
      fs.writeFileSync(target, raw.endsWith("\n") ? raw : `${raw}\n`);
      manifests.push({ key, source, manifest: parseYaml(raw), cache_source: "fresh" });
    } catch {
      anyFailure = true;
      if (fs.existsSync(target)) {
        manifests.push({ key, source, manifest: readData(target), cache_source: "cached" });
        usedCache = true;
        warnings.push(issue("REQ_MANIFEST_FETCH_FAILED_CACHE_USED", `Failed to fetch ${key}; cached manifest used.`, false, false));
      } else {
        warnings.push(issue("REQ_MANIFEST_FETCH_FAILED_NO_CACHE", `Failed to fetch ${key}; no cache available.`, false, false));
      }
    }
  }

  let cacheSource = "fresh";
  if (usedCache) cacheSource = "cached";
  if (anyFailure && !usedCache) cacheSource = "empty_no_cache";
  return { manifests, warnings, cacheSource };
}

function sourceRank(entry) {
  if (entry.required_scope === "department") return 1;
  if (String(entry.required_scope || "").startsWith("engine:")) return 2;
  if (String(entry.required_scope || "").startsWith("repo:")) return 3;
  return 0;
}

function packageRecord(config, plugin) {
  const storeRoot = repoPath(process.cwd(), config.agentcentor.package_store.reference);
  const indexPath = path.join(storeRoot, "index", `${plugin.agentcentor_skill}.json`);
  const index = readData(indexPath);
  return index.versions?.[plugin.version] || null;
}

function artifactPathFromRecord(config, record) {
  if (!record?.artifact_path) return null;
  if (path.isAbsolute(record.artifact_path)) return record.artifact_path;
  const storeRoot = repoPath(process.cwd(), config.agentcentor.package_store.reference);
  const centerRoot = path.resolve(storeRoot, "..", "..");
  return path.resolve(centerRoot, record.artifact_path);
}

function resolveRequired(config, fetched, run) {
  const report = {
    ...commonReport("effective-required/v1", run),
    repo: config.repo,
    engine: config.engine,
    sources: fetched.manifests.map((item) => ({ key: item.key, source: item.source, cache_source: item.cache_source })),
    resolved_plugins: [],
    filtered_items: [],
    conflicts: [],
    cache_source: fetched.cacheSource
  };
  report.warnings.push(...fetched.warnings);

  const groups = new Map();
  for (const source of fetched.manifests) {
    for (const plugin of source.manifest.plugins || []) {
      const entry = { ...plugin, source_manifest: source.source };
      if (!groups.has(entry.agentcentor_skill)) groups.set(entry.agentcentor_skill, []);
      groups.get(entry.agentcentor_skill).push(entry);
    }
  }

  for (const [skill, entries] of groups.entries()) {
    for (const entry of entries) {
      if (entry.status === "active" && entry.main_tag !== config.policy.required_main_tag) {
        report.filtered_items.push({
          agentcentor_skill: entry.agentcentor_skill,
          version: entry.version,
          required_scope: entry.required_scope,
          main_tag: entry.main_tag,
          status: entry.status,
          reason: "REQ_NON_DEV_ACTIVE",
          source_manifest: entry.source_manifest
        });
        report.blocking_errors.push(issue("REQ_NON_DEV_ACTIVE", `${skill} is active but main_tag is not dev.`, true, true));
      } else if (entry.status !== "active") {
        report.filtered_items.push({
          agentcentor_skill: entry.agentcentor_skill,
          version: entry.version,
          required_scope: entry.required_scope,
          main_tag: entry.main_tag,
          status: entry.status,
          reason: `REQ_${String(entry.status).toUpperCase()}_FILTERED`,
          source_manifest: entry.source_manifest
        });
      }
    }

    const activeDev = entries.filter((entry) => entry.status === "active" && entry.main_tag === config.policy.required_main_tag);
    if (!activeDev.length) continue;
    activeDev.sort((a, b) => sourceRank(b) - sourceRank(a));
    const selected = activeDev[0];
    const versions = new Set(activeDev.map((entry) => entry.version));
    if (versions.size > 1) {
      const warning = issue("REQ_CONFLICT_RESOLVED", `${skill} active version conflict resolved by precedence.`, false, false);
      report.conflicts.push({
        agentcentor_skill: skill,
        selected: { version: selected.version, required_scope: selected.required_scope },
        candidates: activeDev.map((entry) => ({ version: entry.version, required_scope: entry.required_scope })),
        ...warning
      });
      report.warnings.push(warning);
    }

    let record = null;
    try {
      record = packageRecord(config, selected);
    } catch {
      report.blocking_errors.push(issue("REQ_UNPUBLISHED_ACTIVE", `${skill}@${selected.version} is not available in package store index.`, true, true));
    }
    report.resolved_plugins.push({
      agentcentor_skill: selected.agentcentor_skill,
      version: selected.version,
      required_scope: selected.required_scope,
      resolved_from: activeDev.map((entry) => entry.required_scope),
      main_tag: selected.main_tag,
      status: selected.status,
      artifact_hash: record?.artifact_hash || selected.artifact_hash || null
    });
  }

  report.status = report.blocking_errors.length ? "failed" : report.warnings.length ? "warning" : "success";
  return report;
}

function emptyValidation() {
  return {
    contains_manifest: false,
    contains_skill: false,
    contains_activation_or_hook: false,
    install_entry_present: false
  };
}

function emptyInstallReport(config, run, status, warnings = [], blockingErrors = []) {
  const report = {
    ...commonReport("install-report/v1", run, status),
    command_template_id: "skill_install_command_template",
    rendered_command: "",
    exit_code: 0,
    agentcentor_skill: null,
    version: null,
    artifact_hash: null,
    installed_files: [],
    validation: emptyValidation(),
    used_cache: false
  };
  report.warnings.push(...warnings);
  report.blocking_errors.push(...blockingErrors);
  return report;
}

function readInstalled(config, repoRoot) {
  const file = repoPath(repoRoot, config.paths.manifest);
  if (!fs.existsSync(file)) return null;
  return readData(file);
}

function writeInstalled(config, repoRoot, run, plugins, status = "success") {
  const report = {
    ...commonReport("installed-plugins/v1", run, status),
    repo: config.repo,
    engine: config.engine,
    plugins
  };
  writeJson(repoPath(repoRoot, config.paths.manifest), report);
  return report;
}

function isManaged(existing, skill) {
  return Boolean((existing?.plugins || []).find((plugin) => (
    plugin.agentcentor_skill === skill &&
    plugin.installed_by === "session_start_bootstrap" &&
    plugin.status !== "conflict_skipped"
  )));
}

function managedEntry(existing, skill) {
  return (existing?.plugins || []).find((plugin) => (
    plugin.agentcentor_skill === skill &&
    plugin.installed_by === "session_start_bootstrap" &&
    plugin.status !== "conflict_skipped"
  )) || null;
}

function relativeRepoPath(repoRoot, target) {
  return path.relative(repoRoot, target).split(path.sep).join("/");
}

function isManagedTarget(repoRoot, target, entry) {
  if (!entry) return false;
  return new Set(entry.managed_files || []).has(relativeRepoPath(repoRoot, target));
}

function packageLanding(config, repoRoot, plugin, source) {
  const target = path.join(repoPath(repoRoot, config.paths.plugin_dir), plugin.agentcentor_skill);
  const targets = listFiles(source).map((file) => path.join(target, file));
  for (const root of config.paths.validation_roots || []) {
    targets.push(repoPath(repoRoot, root.path));
  }
  return {
    target,
    targets: [...new Set(targets.map((item) => path.resolve(item)))]
  };
}

function localUnmanagedConflict(config, repoRoot, plugin, existing, source = null) {
  const entry = managedEntry(existing, plugin.agentcentor_skill);
  const skillPath = path.join(repoPath(repoRoot, config.paths.skills_dir), plugin.agentcentor_skill, "SKILL.md");
  if (fs.existsSync(skillPath) && !isManagedTarget(repoRoot, skillPath, entry)) return skillPath;
  const landing = source
    ? packageLanding(config, repoRoot, plugin, source)
    : null;
  const targets = landing
    ? landing.targets
    : (config.paths.validation_roots || []).map((root) => repoPath(repoRoot, root.path));
  if (landing && fs.existsSync(landing.target)) {
    if (!entry) return landing.target;
    for (const file of listFiles(landing.target)) {
      const targetFile = path.join(landing.target, file);
      if (!isManagedTarget(repoRoot, targetFile, entry)) return targetFile;
    }
  }
  return targets.find((target) => fs.existsSync(target) && !isManagedTarget(repoRoot, target, entry)) || null;
}

function validateLanding(config, repoRoot) {
  const validation = emptyValidation();
  for (const root of config.paths.validation_roots || []) {
    const exists = fs.existsSync(repoPath(repoRoot, root.path));
    if (root.name === "plugin_manifest") validation.contains_manifest = exists;
    if (root.name === "skill_content") validation.contains_skill = exists;
    if (root.name === "activation") validation.contains_activation_or_hook = exists;
    if (root.name === "install_entry") validation.install_entry_present = exists;
  }
  return validation;
}

function renderInstallCommand(config, plugin) {
  return renderTemplate(config.agentcentor.skill_install_command_template, {
    cli_package: config.agentcentor.cli_package,
    agentcentor_skill: plugin.agentcentor_skill,
    version: plugin.version,
    client: config.agentcentor.client,
    package_store: config.agentcentor.package_store.reference
  });
}

function installPlugin(config, repoRoot, run, effective, plugin, fromCache) {
  const existing = readInstalled(config, repoRoot);
  let record = null;
  let source = null;
  try {
    record = packageRecord(config, plugin);
    source = artifactPathFromRecord(config, record);
    if (!source || !fs.existsSync(source)) throw new Error("package artifact path is missing");
  } catch (error) {
    const installReport = emptyInstallReport(config, run, "failed");
    installReport.agentcentor_skill = plugin.agentcentor_skill;
    installReport.version = plugin.version;
    installReport.artifact_hash = plugin.artifact_hash;
    installReport.exit_code = 2;
    installReport.blocking_errors.push(issue("CLI_PACKAGE_NOT_FOUND", `Package store lookup failed: ${error.message}`, true, true));
    const installed = writeInstalled(config, repoRoot, run, [{
      agentcentor_skill: plugin.agentcentor_skill,
      version: plugin.version,
      old_version: null,
      new_version: plugin.version,
      required_scope: plugin.required_scope,
      installed_by: "session_start_bootstrap",
      status: "install_failed",
      stale_reason: null,
      managed_files: [],
      validation: installReport.validation
    }], "failed");
    return { installReport, installed, changed: false };
  }

  const conflictPath = localUnmanagedConflict(config, repoRoot, plugin, existing, source);
  if (conflictPath) {
    const warning = issue("REQ_LOCAL_CONFLICT_SKIPPED", `Local unmanaged landing path exists for ${plugin.agentcentor_skill}; install skipped without overwrite.`, true, true);
    effective.conflicts.push({
      agentcentor_skill: plugin.agentcentor_skill,
      selected: { version: plugin.version, required_scope: plugin.required_scope },
      candidates: [{ version: plugin.version, required_scope: plugin.required_scope }],
      ...warning
    });
    effective.warnings.push(warning);
    effective.status = "warning";
    const installReport = emptyInstallReport(config, run, "skipped", [warning]);
    installReport.agentcentor_skill = plugin.agentcentor_skill;
    installReport.version = plugin.version;
    installReport.artifact_hash = plugin.artifact_hash;
    const installed = writeInstalled(config, repoRoot, run, [{
      agentcentor_skill: plugin.agentcentor_skill,
      version: plugin.version,
      old_version: null,
      new_version: null,
      required_scope: plugin.required_scope,
      installed_by: "session_start_bootstrap",
      status: "conflict_skipped",
      stale_reason: "local_unmanaged_conflict",
      managed_files: [],
      validation: installReport.validation
    }], "warning");
    return { installReport, installed, changed: false };
  }

  const installReport = emptyInstallReport(config, run, "success");
  installReport.agentcentor_skill = plugin.agentcentor_skill;
  installReport.version = plugin.version;
  installReport.artifact_hash = plugin.artifact_hash;
  installReport.rendered_command = renderInstallCommand(config, plugin);
  installReport.used_cache = Boolean(fromCache);

  if (process.env.ROCKSDB_BOOTSTRAP_INJECT_INSTALL_FAILURE === "1") {
    installReport.status = "failed";
    installReport.exit_code = 1;
    installReport.blocking_errors.push(issue("CLI_EXIT_NON_ZERO", "Injected local bootstrap install failure.", true, true));
    const installed = writeInstalled(config, repoRoot, run, [{
      agentcentor_skill: plugin.agentcentor_skill,
      version: plugin.version,
      old_version: null,
      new_version: plugin.version,
      required_scope: plugin.required_scope,
      installed_by: "session_start_bootstrap",
      status: "install_failed",
      stale_reason: null,
      managed_files: [],
      validation: installReport.validation
    }], "failed");
    return { installReport, installed, changed: false };
  }

  const target = path.join(repoPath(repoRoot, config.paths.plugin_dir), plugin.agentcentor_skill);
  const beforeHash = fs.existsSync(target) ? stableHashDir(target) : null;
  removeManagedPath(target);
  copyDir(source, target);
  const afterHash = stableHashDir(target);
  installReport.status = beforeHash === afterHash ? "cached" : "success";
  installReport.used_cache = installReport.used_cache || beforeHash === afterHash;
  installReport.artifact_hash = record.artifact_hash;
  installReport.installed_files = listFiles(target).map((file) => path.join(path.relative(repoRoot, target), file).split(path.sep).join("/"));
  installReport.validation = validateLanding(config, repoRoot);
  if (!Object.values(installReport.validation).every(Boolean)) {
    installReport.status = "failed";
    installReport.exit_code = 4;
    installReport.blocking_errors.push(issue("CLI_INSTALL_VALIDATION_FAILED", "Installed package landing validation failed.", true, true));
  }

  const installedStatus = installReport.status === "failed" ? "install_failed" : "installed";
  const installed = writeInstalled(config, repoRoot, run, [{
    agentcentor_skill: plugin.agentcentor_skill,
    version: plugin.version,
    old_version: null,
    new_version: plugin.version,
    required_scope: plugin.required_scope,
    installed_by: "session_start_bootstrap",
    status: installedStatus,
    stale_reason: null,
    managed_files: installReport.installed_files,
    validation: installReport.validation
  }], installedStatus === "installed" ? "success" : "failed");
  return { installReport, installed, changed: installReport.status === "success" };
}

function writeSession(config, repoRoot, run, effective, installReport, installed, changed) {
  const warnings = [...(effective.warnings || []), ...(installReport.warnings || [])];
  const blocking = [...(effective.blocking_errors || []), ...(installReport.blocking_errors || [])];
  const report = {
    ...commonReport("session-context/v1", run, blocking.length || warnings.length ? "warning" : "success"),
    hookSpecificOutput: {
      hookEventName: "SessionStart",
      reloadSkills: Boolean(changed),
      additionalContext: [
        `Effective required: ${(effective.resolved_plugins || []).length} active dev plugin(s).`,
        `Installed state: ${(installed.plugins || []).map((plugin) => `${plugin.agentcentor_skill}:${plugin.status}`).join(", ") || "none"}.`,
        `Reports: ${config.paths.effective_required}, ${config.paths.install_report}, ${config.paths.manifest}.`
      ].join(" ")
    },
    warnings_summary: warnings.map((item) => `${item.code}: ${item.message}`)
  };
  report.warnings.push(...warnings);
  report.blocking_errors.push(...blocking);
  writeJson(repoPath(repoRoot, config.paths.session_context), report);
  return report;
}

function bootstrap(repoRoot, options = {}) {
  const run = options.runId || runId("rocksdb-bootstrap");
  const previousCwd = process.cwd();
  process.chdir(repoRoot);
  try {
    const config = loadConfig(repoRoot);
    const adapterIssues = configIssues(config);
    const fetched = adapterIssues.length ? { manifests: [], warnings: [], cacheSource: "unavailable" } : fetchManifests(config, repoRoot);
    const effective = adapterIssues.length
      ? {
          ...commonReport("effective-required/v1", run, "failed"),
          repo: config.repo,
          engine: config.engine,
          sources: [],
          resolved_plugins: [],
          filtered_items: [],
          conflicts: [],
          cache_source: "unavailable"
        }
      : resolveRequired(config, fetched, run);
    effective.blocking_errors.push(...adapterIssues);
    if (adapterIssues.length) effective.status = "failed";

    let installReport;
    let installed;
    let changed = false;
    if (!effective.resolved_plugins.length || effective.blocking_errors.length) {
      installReport = emptyInstallReport(config, run, effective.blocking_errors.length ? "failed" : "skipped", effective.warnings, effective.blocking_errors);
      installed = writeInstalled(config, repoRoot, run, [], effective.blocking_errors.length ? "failed" : "skipped");
    } else {
      ({ installReport, installed, changed } = installPlugin(config, repoRoot, run, effective, effective.resolved_plugins[0], effective.cache_source === "cached"));
    }

    writeJson(repoPath(repoRoot, config.paths.effective_required), effective);
    writeJson(repoPath(repoRoot, config.paths.install_report), installReport);
    const session = writeSession(config, repoRoot, run, effective, installReport, installed, changed);
    return { effective, installReport, installed, session };
  } finally {
    process.chdir(previousCwd);
  }
}

function findArg(args, name, fallback = null) {
  const index = args.indexOf(name);
  if (index === -1 || index === args.length - 1) return fallback;
  return args[index + 1];
}

function failOpenMain(repoRoot, error) {
  try {
    const config = loadConfig(repoRoot);
    const run = runId("rocksdb-bootstrap-failopen");
    const warning = issue("BOOTSTRAP_FAIL_OPEN", error.message, false, false);
    const effective = {
      ...commonReport("effective-required/v1", run, "warning"),
      repo: config.repo,
      engine: config.engine,
      sources: [],
      resolved_plugins: [],
      filtered_items: [],
      conflicts: [],
      cache_source: "unavailable"
    };
    effective.warnings.push(warning);
    const installReport = emptyInstallReport(config, run, "warning", [warning]);
    const installed = writeInstalled(config, repoRoot, run, [], "warning");
    writeJson(repoPath(repoRoot, config.paths.effective_required), effective);
    writeJson(repoPath(repoRoot, config.paths.install_report), installReport);
    writeSession(config, repoRoot, run, effective, installReport, installed, false);
  } catch {
    // The SessionStart wrapper still exits 0 if even fail-open reports cannot be written.
  }
}

if (import.meta.url === `file://${process.argv[1]}`) {
  const repoRoot = path.resolve(findArg(process.argv.slice(2), "--repo", path.resolve(BOOTSTRAP_DIR, "..", "..")));
  try {
    bootstrap(repoRoot);
  } catch (error) {
    failOpenMain(repoRoot, error);
  }
  process.exit(0);
}

export { bootstrap, parseYaml, resolveRequired };
