---
name: rocksdb-review
description: Use when reviewing RocksDB changes that need storage-engine specific correctness, testing, or migration scrutiny.
owner: db-ai-skill-owner
version: 3.0.10
tags:
  - dev
  - rocksdb
  - review
source: mock
---

# RocksDB Review Skill

Use this skill to focus review on RocksDB-specific behavior and validation
evidence.

Check these areas first:

- Correctness around write path, compaction, flush, recovery, and iterator
  behavior.
- Boundary compatibility for configuration, options, and on-disk format
  changes.
- Tests or validation evidence that exercise the changed storage-engine path.
- Clear separation between external validation scaffolding and production
  engine behavior.

Do not treat required skill presence as approval for a code change. The required
manifest only controls default installation for matching validation scopes.
