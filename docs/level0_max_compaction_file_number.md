# level0_max_compaction_file_number Design Document

## Overview

New CF option `level0_max_compaction_file_number` limits the number of L0 files
included in a single L0 -> Lbase compaction. This prevents OOM and thread
starvation when the L0 overlap closure is very large (e.g., under heavy Merge
workloads with `level_compaction_dynamic_level_bytes = true`).

## Option Definition

```cpp
// include/rocksdb/advanced_options.h
int level0_max_compaction_file_number = 0;  // 0 = no limit (original behavior)
```

- **Mutable**: yes (can be changed via `SetOptions()` at runtime).
- **Sanitization**: negative values are clamped to 0 (`db/column_family.cc`).

## Core Algorithm

### Truncation: `SetupOtherL0FilesIfNeeded` (`compaction_picker.cc`)

After `GetOverlappingL0Files` returns the full overlap closure:

1. **Sort** files by `largest_seqno` descending (newest first).
   - Required because `GetOverlappingInputs` for L0 may return files in
     non-newest-first order due to multi-pass chain expansion.
2. **Erase** the front (newest) files, keeping only the oldest `limit` files.
3. **Reset `parent_index_`** to -1, forcing a fresh binary search in subsequent
   `GetOverlappingInputs` calls. The stale hint from the pre-truncation range
   may point to an output-level file outside the narrower post-truncation range.
4. **Re-check** `IsRangeInCompaction` with the new (smaller) range.

**Truncation condition**: `size > limit` (strict greater-than).

### Anti-re-expansion Guard: `SetupOtherInputs` (`compaction_picker.cc`)

`SetupOtherInputs` normally tries to expand L0 inputs using the merged L0+Ln
range. After truncation, this would pull the dropped files back in. The guard
`l0_truncated` blocks both expansion paths:

```cpp
bool l0_truncated = (input_level == 0 && l0_file_limit > 0 &&
                     static_cast<int>(inputs->size()) >= l0_file_limit);
```

**Guard condition**: `size >= limit` (greater-than-or-equal).

### Why `>` and `>=` Differ

This is **intentional**, not an inconsistency:

| Location | Condition | Question it answers |
|---|---|---|
| `SetupOtherL0FilesIfNeeded` | `size > limit` | "Do we need to truncate?" |
| `SetupOtherInputs` | `size >= limit` | "Was truncation performed?" |

After truncation from N files to `limit`, `size == limit`. The guard must use
`>=` to detect this state. If `>` were used, `limit > limit` would be false and
the guard would fail to block re-expansion.

Side effect: when the overlap closure naturally equals `limit` (no truncation
happened), the guard also fires. This blocks a legitimate expansion opportunity
but does not affect correctness — the un-expanded files remain in L0 and will
be compacted in a subsequent round.

## Safety Guarantees

**Read correctness**: The dropped newer files remain in L0. Since L0 is queried
newest-first (`FilePicker` iterates `level_files_brief_[0]` from index 0),
these files shadow the compacted older data correctly.

**Merge operator correctness**: The Get path collects operands from L0
(newest-first) then Lbase. Newer operands from L0 are applied after older ones
from Lbase, preserving the correct merge order.

**L0 exclusive lock**: Only one L0->Lbase compaction runs at a time
(`level0_compactions_in_progress_`), so there is no race with another L0
compaction that could violate the file selection.

### Intra-L0 Compaction: `FindIntraL0Compaction` (`compaction_picker.cc`)

When L0->Lbase is blocked (e.g., output level in compaction), `PickIntraL0Compaction`
falls back to intra-L0 compaction (L0->L0, merging multiple L0 files into one).
This path bypasses `SetupOtherL0FilesIfNeeded` entirely.

`FindIntraL0Compaction` is extended with two new parameters:

- `max_compaction_bytes`: hard limit on total compensated bytes in the compaction.
- `level0_max_compaction_file_number`: hard limit on file count (0 = no limit).

These checks are added inside the expansion loop, after the original cost-per-
deleted-file heuristic. The original heuristic (starting from `kMaxSizet`,
dividing by `N-1` deleted files) is preserved — it provides a good efficiency-
based stopping criterion for non-uniform file sizes.

## Files Modified

| File | Change |
|---|---|
| `include/rocksdb/advanced_options.h` | Option declaration |
| `options/cf_options.h` | `MutableCFOptions` member |
| `options/cf_options.cc` | `Dump()` logging |
| `options/options.cc` | Constructor propagation |
| `options/options_helper.cc` | `SetOptions()` / `GetOptions()` registration |
| `options/options_settable_test.cc` | Settable options string |
| `db/column_family.cc` | Sanitization (clamp negatives to 0) |
| `db/compaction_picker.h` | `FindIntraL0Compaction` signature (2 new params) |
| `db/compaction_picker.cc` | Truncation + guard + sort + `parent_index_` reset + intra-L0 limits |
| `db/compaction_picker_fifo.cc` | Updated `FindIntraL0Compaction` call site |
| `db/compaction_picker_test.cc` | 14 test cases |

## Test Cases

| # | Test Name | What it verifies |
|---|---|---|
| 1 | `ZeroNoLimit` | limit=0 → all files selected (original behavior) |
| 2 | `LimitOverlapping` | Two disjoint groups, seed determinism via file_size |
| 3 | `OverlapClosure` | Chain-overlapping files, truncation over full closure |
| 4 | `BelowLimit` | closure < limit → no truncation |
| 5 | `NoReExpansion` | `l0_truncated` guard blocks path 1 (multiple L1 files) |
| 6 | `NoReExpansionWithOutputRange` | Guard blocks with output range overlap |
| 7 | `NoReExpansionLnWiderRange` | Truncated range [a,f], only L1[a,f] selected |
| 8 | `ChainExpansionSort` | Sort before truncation prevents stale-read bug |
| 9 | `DynamicLevelMultipleLnFiles` | `dynamic_level_bytes=true`, base_level=L4 |
| 10 | `DynamicLevelPartialLnOverlap` | Dynamic level + partial Ln overlap |
| 11 | `IntraL0MaxCompactionFileNumber` | Intra-L0: file count limit caps selection |
| 12 | `IntraL0MaxCompactionFileNumberZeroNoLimit` | Intra-L0: limit=0 → no cap |
| 13 | `IntraL0MaxCompactionBytes` | Intra-L0: total bytes limit caps selection |
| 14 | `IntraL0MaxFileNumberBelowMinFiles` | Intra-L0: limit < min_files → no compaction |

### Test Conventions

- **L0 seq ranges**: `smallest_seq = largest_seq - 19` (realistic per-flush windows)
- **Ln seq ranges**: `smallest_seq=2, largest_seq=15` (old compacted data)
- **Seed determinism**: files expected as seeds use larger `file_size` to rank
  first in `FilesByCompactionPri[0]` (`kByCompensatedSize`)

### Stress Test: `L0MaxCompactionFileNumberStress` (`db/db_compaction_test.cc`)

Integration test that verifies both L0→Lbase and intra-L0 paths under sustained
concurrent Merge workload with `dynamic_level_bytes=true`.

**What it does:**

- 4 writer threads continuously issue `Merge()` on a 100-key space (maximizes L0 overlap).
- `EventListener::OnCompactionBegin` captures L0 file count before each compaction pick.
- `EventListener::OnCompactionCompleted` verifies L0 picked files <= limit.
- Periodic (every 10s) level stats printed for observability.
- Final summary reports L0→Lbase / intra-L0 counts, max L0 before pick, truncation count.

**Key assertions:**

- `violations == 0`: no compaction ever picked more L0 files than the limit.
- `max_l0_before > limit`: L0 did accumulate beyond the limit (truncation was needed).
- `truncation_count > 0`: truncation actually fired.

**How to run:**

```bash
# Build (use DISABLE_WARNING_AS_ERROR=1 for older compilers)
DISABLE_WARNING_AS_ERROR=1 make -j$(nproc) db_compaction_test

# Quick validation (30 seconds)
TEST_DURATION_SEC=30 ./db_compaction_test --gtest_filter="DBCompactionTest.L0MaxCompactionFileNumberStress"

# Full run (10 minutes, default)
./db_compaction_test --gtest_filter="DBCompactionTest.L0MaxCompactionFileNumberStress"

# Custom duration
TEST_DURATION_SEC=300 ./db_compaction_test --gtest_filter="DBCompactionTest.L0MaxCompactionFileNumberStress"
```

**Sample output:**

```
[COMPACTION] L0->Lbase: L0_before_pick=13, L0_picked=5, Ln_input=52, output_level=5, output=71
[COMPACTION] IntraL0: L0_before_pick=20, L0_picked=4, Ln_input=0, output_level=0, output=1

=== [T+30s] Level Stats ===
Level Files Size(MB)
...
  L0->Lbase: 145 (max_L0_picked=5), IntraL0: 105 (max_L0_picked=5), max_L0_before_pick=20, truncations=107, violations: 0

=== Final Results ===
  L0->Lbase compactions: 149 (max L0 picked: 5)
  IntraL0 compactions:   105 (max L0 picked: 5)
  Max L0 files before pick: 20
  Truncations (L0_before > limit): 110
  Limit: 5
  Violations: 0
```
