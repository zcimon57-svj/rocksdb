# CompactLevel API 设计文档

## 1. 概述

`CompactLevel` 是一个新增的公共 API，允许用户指定一个或多个输入 level，将其中所有 SST 文件合并输出到指定的目标 level。与 `CompactRange`（按 key 范围）和 `CompactFiles`（按文件名列表）不同，`CompactLevel` 以 **level 粒度**为操作单位，接口更简洁。

用户只需传入 column family、输入 level 列表和输出 level，compaction 所需的 compression、output_file_size_limit 等选项全部从当前 column family 的 `MutableCFOptions` / `ImmutableCFOptions` 中自动获取。

## 2. 接口定义

```cpp
// include/rocksdb/db.h

// 指定 column family
virtual Status CompactLevel(
    ColumnFamilyHandle* column_family,
    const std::vector<int>& input_levels,
    int output_level) = 0;

// 默认 column family
virtual Status CompactLevel(
    const std::vector<int>& input_levels,
    int output_level) {
  return CompactLevel(DefaultColumnFamily(), input_levels, output_level);
}
```

### 参数说明

| 参数 | 说明 |
|------|------|
| `column_family` | 目标 column family，不可为 null |
| `input_levels` | 输入 level 列表，非空，每个值在 `[0, num_levels)` 范围内 |
| `output_level` | 输出 level，在 `[0, num_levels)` 范围内，且 `>= max(input_levels)` |

### 返回值

| Status | 条件 |
|--------|------|
| `OK` | 成功，或输入 level 无文件（no-op） |
| `InvalidArgument` | 参数校验失败 |
| `Aborted` | 文件正在被其他 compaction 占用 |
| `CompactionTooLarge` | 磁盘空间不足 |
| `ShutdownInProgress` | DB 正在关闭 |
| `NotSupported` | ReadOnly 模式或 ROCKSDB_LITE 编译 |

## 3. 设计决策

### 3.1 内部复用 CompactFiles

`CompactLevel` 不新增独立的 compaction 执行路径，而是：

1. 通过 `GetColumnFamilyMetaData` 收集输入 level 的所有 SST 文件名
2. 构造 `CompactionOptions`
3. 委托给 `CompactFiles` 执行

**优点**：
- 改动量小（实现约 70 行），风险低
- overlap 扩展、冲突检测、锁管理全部复用 `CompactFiles` → `CompactFilesImpl` → `SanitizeCompactionInputFiles` 的已验证链路
- 同步执行语义（在调用线程完成），调用者明确知道完成时机

### 3.2 CompactionOptions 的自动构造

| 选项 | 取值来源 | 说明 |
|------|---------|------|
| `compression` | `compression_per_level[output_level]`（优先）或 `MutableCFOptions::compression` | 与自动 compaction 行为一致 |
| `output_file_size_limit` | `MutableCFOptions::target_file_size_base` | **必须设置**，否则退化为 `max_compaction_bytes` 兜底，产生巨大文件 |
| `max_subcompactions` | 0（使用 DB 默认值） | - |

### 3.3 为什么必须设置 output_file_size_limit

CompactFiles 路径中 `grandparents` 为空向量（`compaction_picker.cc` CompactFiles 方法 343 行），导致 `ShouldStopBefore` 中 `overlapped_bytes` 始终为 0。输出文件切分仅依赖两个条件：

```
条件(1): current_output_file_size >= max_output_file_size    // output_file_size_limit
条件(2): curr_file_size > max_compaction_bytes               // grandparents 为空时退化
```

若不设置 `output_file_size_limit`（默认 MAX），条件(1) 永远不触发，仅由条件(2) 的 `max_compaction_bytes` 兜底。以 `max_compaction_bytes = 1.5GB` 为例，每个输出文件将接近 1.5GB，导致：

- 后续自动 compaction 写放大严重恶化（每次涉及的 overlap 数据量增大约 24 倍）
- compaction 执行时间和资源消耗显著增加
- 磁盘空间峰值压力增大

## 4. 关键行为

### 4.1 L0 文件自动扩展

当 `input_levels` 包含 L0 且 `output_level > 0` 时，`SanitizeCompactionInputFilesForAllLevels` 会自动将 L0 的**所有文件**包含进来（因为 L0 文件间可能有 key overlap）。

### 4.2 重复 level 去重

`input_levels` 中的重复值通过 `std::set` 去重，不会导致文件被重复添加。

### 4.3 同级 rewrite

`input_level == output_level` 是合法的，相当于对该 level 做原地重写（merge 聚合、文件合并）。

### 4.4 并发冲突

如果目标文件正在被其他 compaction 使用，返回 `Status::Aborted`。调用者应重试或等待。

## 5. 文件改动清单

| 文件 | 改动 |
|------|------|
| `include/rocksdb/db.h` | 纯虚接口声明 + 默认 CF 重载 |
| `db/db_impl.h` | override 声明 |
| `db/db_impl_compaction_flush.cc` | 核心实现 + `#include "util/string_util.h"` |
| `db/db_impl_readonly.h` | ReadOnly 模式返回 `NotSupported` |
| `include/rocksdb/utilities/stackable_db.h` | StackableDB 转发 |
| `db/compact_files_test.cc` | 18 个测试用例 |

## 6. 测试覆盖

共 18 个测试用例，测试环境统一使用 `level_compaction_dynamic_level_bytes = true`，写入方式为 `Merge`（StringAppendOperator）。

| 类别 | 数量 | 用例 |
|------|------|------|
| 参数校验 | 4 | 空输入、level 超范围、output < input |
| 基础功能 | 4 | 单 level、多 level、同级 rewrite、L0 输入 |
| Bottom Level | 4 | bottom 自身 rewrite、多级到 bottom、merge 累积、输出文件大小切分 |
| 边界场景 | 3 | 空 level、部分空 level、重复 level |
| 并发冲突 | 1 | 与进行中的 CompactFiles 冲突 |
| 数据正确性 | 2 | 多级 merge 累积顺序、Delete tombstone 消除 |

## 7. 使用示例

```cpp
// 将 L1, L2 的所有文件合并到 L3
db->CompactLevel(cf_handle, {1, 2}, 3);

// 将 L0 的所有文件合并到 L1（L0 全部文件会被自动包含）
db->CompactLevel({0}, 1);

// 原地重写 bottom level（合并 merge operands、重新切分文件）
db->CompactLevel({4}, 4);
```
