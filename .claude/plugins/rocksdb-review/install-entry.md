---
schema_version: install-entry/v1
agentcentor_skill: rocksdb-review
version: 3.0.10
client: claudecode
command_template_id: skill_install_command_template
command_template: "npx @aimarket/agentcenter skill add {agentcentor_skill}@{version} --client {client}"
bootstrap_mode: session_start
post_install_checks:
  - ".claude-plugin/plugin.json"
  - "skills/rocksdb-review/SKILL.md"
  - "activation.yml"
  - "install-entry.md"
---

Install entry for the external validation package source. Later adapter work
supplies registry, package-store, credential, and internal host mapping without
changing this package identity.
