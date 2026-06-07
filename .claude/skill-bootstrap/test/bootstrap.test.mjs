import assert from "node:assert/strict";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { spawnSync } from "node:child_process";
import test from "node:test";
import { fileURLToPath } from "node:url";

const testDir = path.dirname(fileURLToPath(import.meta.url));
const repoRoot = path.resolve(testDir, "..", "..", "..");
const bootstrapDir = path.join(repoRoot, ".claude", "skill-bootstrap");

function copyDir(from, to) {
  fs.mkdirSync(to, { recursive: true });
  for (const entry of fs.readdirSync(from, { withFileTypes: true })) {
    if (["cache", "reports", "state", "test"].includes(entry.name)) continue;
    if (entry.name === "session-context.json") continue;
    const source = path.join(from, entry.name);
    const target = path.join(to, entry.name);
    if (entry.isDirectory()) copyDir(source, target);
    if (entry.isFile()) fs.copyFileSync(source, target);
  }
}

function prepareRepo(name) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), `rocksdb-bootstrap-${name}-`));
  fs.copyFileSync(path.join(repoRoot, "repo-skill-config.yml"), path.join(root, "repo-skill-config.yml"));
  fs.mkdirSync(path.join(root, ".claude"), { recursive: true });
  fs.copyFileSync(path.join(repoRoot, ".claude", "settings.json"), path.join(root, ".claude", "settings.json"));
  copyDir(bootstrapDir, path.join(root, ".claude", "skill-bootstrap"));
  return root;
}

function runBootstrap(root, env = {}) {
  const result = spawnSync(
    process.execPath,
    [path.join(root, ".claude", "skill-bootstrap", "bootstrap.mjs"), "--repo", root],
    { cwd: root, env: { ...process.env, ...env }, encoding: "utf8" }
  );
  assert.equal(result.status, 0, result.stderr || result.stdout);
}

function readJson(root, relativePath) {
  return JSON.parse(fs.readFileSync(path.join(root, relativePath), "utf8"));
}

function assertIssue(issue) {
  assert.equal(typeof issue.code, "string");
  assert.equal(typeof issue.message, "string");
  assert.equal(typeof issue.blocks_external_validation_pass, "boolean");
  assert.equal(typeof issue.blocks_active_required_activation, "boolean");
  assert.equal(issue.blocks_claude_code_startup, false);
}

function assertCommon(report, schemaVersion) {
  assert.equal(report.schema_version, schemaVersion);
  assert.equal(typeof report.run_id, "string");
  assert.equal(typeof report.generated_at, "string");
  assert.match(report.status, /^(success|warning|failed|cached|skipped)$/);
  assert.ok(Array.isArray(report.blocking_errors));
  assert.ok(Array.isArray(report.warnings));
  for (const issue of [...report.blocking_errors, ...report.warnings]) assertIssue(issue);
}

test("happy path installs rocksdb-review from center required manifests and mock file-store", () => {
  const root = prepareRepo("happy");
  runBootstrap(root);

  const effective = readJson(root, ".claude/skill-bootstrap/cache/effective_required.json");
  const install = readJson(root, ".claude/skill-bootstrap/reports/install_report.json");
  const installed = readJson(root, ".claude/skill-bootstrap/state/installed-plugins.json");
  const session = readJson(root, ".claude/skill-bootstrap/session-context.json");

  assertCommon(effective, "effective-required/v1");
  assertCommon(install, "install-report/v1");
  assertCommon(installed, "installed-plugins/v1");
  assertCommon(session, "session-context/v1");

  assert.equal(effective.repo, "rocksdb");
  assert.equal(effective.engine, "taurus");
  assert.equal(effective.cache_source, "fresh");
  assert.equal(effective.resolved_plugins[0].agentcentor_skill, "rocksdb-review");
  assert.equal(effective.resolved_plugins[0].version, "3.0.10");
  assert.equal(effective.resolved_plugins[0].artifact_hash, install.artifact_hash);
  assert.equal(install.validation.contains_manifest, true);
  assert.equal(install.validation.contains_skill, true);
  assert.equal(install.validation.contains_activation_or_hook, true);
  assert.equal(install.validation.install_entry_present, true);
  assert.match(install.rendered_command, /rocksdb-review@3\.0\.10/);
  assert.equal(installed.plugins[0].status, "installed");
  assert.equal(session.hookSpecificOutput.hookEventName, "SessionStart");
  assert.equal(session.hookSpecificOutput.reloadSkills, true);
  assert.doesNotMatch(session.hookSpecificOutput.additionalContext, /adopted|采纳|proof/i);
});

test("missing required sources use cached manifests and still fail open", () => {
  const root = prepareRepo("cache");
  runBootstrap(root);

  const configPath = path.join(root, "repo-skill-config.yml");
  const missingRoot = path.join(root, "missing-required");
  const originalConfig = fs.readFileSync(configPath, "utf8");
  fs.writeFileSync(
    configPath,
    originalConfig.replaceAll("/home/cimon/code/skill-management-validation-repos/Mock-DBS-Skill/required", missingRoot)
  );

  runBootstrap(root);
  const effective = readJson(root, ".claude/skill-bootstrap/cache/effective_required.json");
  const install = readJson(root, ".claude/skill-bootstrap/reports/install_report.json");
  const session = readJson(root, ".claude/skill-bootstrap/session-context.json");

  assert.equal(effective.cache_source, "cached");
  assert.equal(effective.resolved_plugins[0].agentcentor_skill, "rocksdb-review");
  assert.equal(install.used_cache, true);
  assert.equal(session.status, "warning");
  for (const issue of [...effective.warnings, ...install.warnings, ...session.warnings]) {
    assert.equal(issue.blocks_claude_code_startup, false);
  }
});

test("local unmanaged rocksdb-review skill is not overwritten", () => {
  const root = prepareRepo("conflict");
  const unmanagedPath = path.join(root, ".claude", "skills", "rocksdb-review", "SKILL.md");
  fs.mkdirSync(path.dirname(unmanagedPath), { recursive: true });
  fs.writeFileSync(unmanagedPath, "UNMANAGED LOCAL SKILL\nDo not overwrite.\n");

  runBootstrap(root);

  const install = readJson(root, ".claude/skill-bootstrap/reports/install_report.json");
  const installed = readJson(root, ".claude/skill-bootstrap/state/installed-plugins.json");
  const effective = readJson(root, ".claude/skill-bootstrap/cache/effective_required.json");

  assert.equal(fs.readFileSync(unmanagedPath, "utf8"), "UNMANAGED LOCAL SKILL\nDo not overwrite.\n");
  assert.equal(install.status, "skipped");
  assert.equal(install.rendered_command, "");
  assert.equal(install.warnings[0].code, "REQ_LOCAL_CONFLICT_SKIPPED");
  assert.equal(install.warnings[0].blocks_claude_code_startup, false);
  assert.equal(installed.plugins[0].status, "conflict_skipped");
  assert.equal(effective.conflicts[0].code, "REQ_LOCAL_CONFLICT_SKIPPED");
});

test("local unmanaged plugin manifest landing path is not overwritten", () => {
  const root = prepareRepo("plugin-path-conflict");
  const unmanagedPath = path.join(root, ".claude", "plugins", "rocksdb-review", ".claude-plugin", "plugin.json");
  const unmanagedContent = "{\"unmanaged\":\"plugin manifest sentinel\"}\n";
  fs.mkdirSync(path.dirname(unmanagedPath), { recursive: true });
  fs.writeFileSync(unmanagedPath, unmanagedContent);

  runBootstrap(root);

  const install = readJson(root, ".claude/skill-bootstrap/reports/install_report.json");
  const installed = readJson(root, ".claude/skill-bootstrap/state/installed-plugins.json");
  const effective = readJson(root, ".claude/skill-bootstrap/cache/effective_required.json");

  assert.equal(fs.readFileSync(unmanagedPath, "utf8"), unmanagedContent);
  assert.match(install.status, /^(skipped|warning)$/);
  assert.equal(install.rendered_command, "");
  assert.equal(install.warnings[0].code, "REQ_LOCAL_CONFLICT_SKIPPED");
  assert.equal(install.warnings[0].blocks_claude_code_startup, false);
  assert.equal(installed.plugins[0].status, "conflict_skipped");
  assert.equal(effective.conflicts[0].code, "REQ_LOCAL_CONFLICT_SKIPPED");
});

test("local unmanaged activation landing path is not overwritten", () => {
  const root = prepareRepo("activation-path-conflict");
  const unmanagedPath = path.join(root, ".claude", "plugins", "rocksdb-review", "activation.yml");
  const unmanagedContent = "unmanaged: activation sentinel\n";
  fs.mkdirSync(path.dirname(unmanagedPath), { recursive: true });
  fs.writeFileSync(unmanagedPath, unmanagedContent);

  runBootstrap(root);

  const install = readJson(root, ".claude/skill-bootstrap/reports/install_report.json");
  const installed = readJson(root, ".claude/skill-bootstrap/state/installed-plugins.json");

  assert.equal(fs.readFileSync(unmanagedPath, "utf8"), unmanagedContent);
  assert.match(install.status, /^(skipped|warning)$/);
  assert.equal(install.rendered_command, "");
  assert.equal(install.warnings[0].code, "REQ_LOCAL_CONFLICT_SKIPPED");
  assert.equal(install.warnings[0].blocks_claude_code_startup, false);
  assert.equal(installed.plugins[0].status, "conflict_skipped");
});
