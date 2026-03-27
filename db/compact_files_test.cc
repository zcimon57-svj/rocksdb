//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#ifndef ROCKSDB_LITE

#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "db/db_impl.h"
#include "port/port.h"
#include "rocksdb/db.h"
#include "rocksdb/env.h"
#include "utilities/merge_operators/string_append/stringappend.h"
#include "util/string_util.h"
#include "util/sync_point.h"
#include "util/testharness.h"

namespace rocksdb {

class CompactFilesTest : public testing::Test {
 public:
  CompactFilesTest() {
    env_ = Env::Default();
    db_name_ = test::PerThreadDBPath("compact_files_test");
  }

  std::string db_name_;
  Env* env_;
};

// A class which remembers the name of each flushed file.
class FlushedFileCollector : public EventListener {
 public:
  FlushedFileCollector() {}
  ~FlushedFileCollector() {}

  virtual void OnFlushCompleted(DB* /*db*/, const FlushJobInfo& info) override {
    std::lock_guard<std::mutex> lock(mutex_);
    flushed_files_.push_back(info.file_path);
  }

  std::vector<std::string> GetFlushedFiles() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> result;
    for (auto fname : flushed_files_) {
      result.push_back(fname);
    }
    return result;
  }
  void ClearFlushedFiles() {
    std::lock_guard<std::mutex> lock(mutex_);
    flushed_files_.clear();
  }

 private:
  std::vector<std::string> flushed_files_;
  std::mutex mutex_;
};

TEST_F(CompactFilesTest, L0ConflictsFiles) {
  Options options;
  // to trigger compaction more easily
  const int kWriteBufferSize = 10000;
  const int kLevel0Trigger = 2;
  options.create_if_missing = true;
  options.compaction_style = kCompactionStyleLevel;
  // Small slowdown and stop trigger for experimental purpose.
  options.level0_slowdown_writes_trigger = 20;
  options.level0_stop_writes_trigger = 20;
  options.level0_stop_writes_trigger = 20;
  options.write_buffer_size = kWriteBufferSize;
  options.level0_file_num_compaction_trigger = kLevel0Trigger;
  options.compression = kNoCompression;

  DB* db = nullptr;
  DestroyDB(db_name_, options);
  Status s = DB::Open(options, db_name_, &db);
  assert(s.ok());
  assert(db);

  rocksdb::SyncPoint::GetInstance()->LoadDependency({
      {"CompactFilesImpl:0", "BackgroundCallCompaction:0"},
      {"BackgroundCallCompaction:1", "CompactFilesImpl:1"},
  });
  rocksdb::SyncPoint::GetInstance()->EnableProcessing();

  // create couple files
  // Background compaction starts and waits in BackgroundCallCompaction:0
  for (int i = 0; i < kLevel0Trigger * 4; ++i) {
    db->Put(WriteOptions(), ToString(i), "");
    db->Put(WriteOptions(), ToString(100 - i), "");
    db->Flush(FlushOptions());
  }

  rocksdb::ColumnFamilyMetaData meta;
  db->GetColumnFamilyMetaData(&meta);
  std::string file1;
  for (auto& file : meta.levels[0].files) {
    ASSERT_EQ(0, meta.levels[0].level);
    if (file1 == "") {
      file1 = file.db_path + "/" + file.name;
    } else {
      std::string file2 = file.db_path + "/" + file.name;
      // Another thread starts a compact files and creates an L0 compaction
      // The background compaction then notices that there is an L0 compaction
      // already in progress and doesn't do an L0 compaction
      // Once the background compaction finishes, the compact files finishes
      ASSERT_OK(
          db->CompactFiles(rocksdb::CompactionOptions(), {file1, file2}, 0));
      break;
    }
  }
  rocksdb::SyncPoint::GetInstance()->DisableProcessing();
  delete db;
}

TEST_F(CompactFilesTest, ObsoleteFiles) {
  Options options;
  // to trigger compaction more easily
  const int kWriteBufferSize = 65536;
  options.create_if_missing = true;
  // Disable RocksDB background compaction.
  options.compaction_style = kCompactionStyleNone;
  options.level0_slowdown_writes_trigger = (1 << 30);
  options.level0_stop_writes_trigger = (1 << 30);
  options.write_buffer_size = kWriteBufferSize;
  options.max_write_buffer_number = 2;
  options.compression = kNoCompression;

  // Add listener
  FlushedFileCollector* collector = new FlushedFileCollector();
  options.listeners.emplace_back(collector);

  DB* db = nullptr;
  DestroyDB(db_name_, options);
  Status s = DB::Open(options, db_name_, &db);
  assert(s.ok());
  assert(db);

  // create couple files
  for (int i = 1000; i < 2000; ++i) {
    db->Put(WriteOptions(), ToString(i),
            std::string(kWriteBufferSize / 10, 'a' + (i % 26)));
  }

  auto l0_files = collector->GetFlushedFiles();
  ASSERT_OK(db->CompactFiles(CompactionOptions(), l0_files, 1));
  reinterpret_cast<DBImpl*>(db)->TEST_WaitForCompact();

  // verify all compaction input files are deleted
  for (auto fname : l0_files) {
    ASSERT_EQ(Status::NotFound(), env_->FileExists(fname));
  }
  delete db;
}

TEST_F(CompactFilesTest, NotCutOutputOnLevel0) {
  Options options;
  options.create_if_missing = true;
  // Disable RocksDB background compaction.
  options.compaction_style = kCompactionStyleNone;
  options.level0_slowdown_writes_trigger = 1000;
  options.level0_stop_writes_trigger = 1000;
  options.write_buffer_size = 65536;
  options.max_write_buffer_number = 2;
  options.compression = kNoCompression;
  options.max_compaction_bytes = 5000;

  // Add listener
  FlushedFileCollector* collector = new FlushedFileCollector();
  options.listeners.emplace_back(collector);

  DB* db = nullptr;
  DestroyDB(db_name_, options);
  Status s = DB::Open(options, db_name_, &db);
  assert(s.ok());
  assert(db);

  // create couple files
  for (int i = 0; i < 500; ++i) {
    db->Put(WriteOptions(), ToString(i), std::string(1000, 'a' + (i % 26)));
  }
  reinterpret_cast<DBImpl*>(db)->TEST_WaitForFlushMemTable();
  auto l0_files_1 = collector->GetFlushedFiles();
  collector->ClearFlushedFiles();
  for (int i = 0; i < 500; ++i) {
    db->Put(WriteOptions(), ToString(i), std::string(1000, 'a' + (i % 26)));
  }
  reinterpret_cast<DBImpl*>(db)->TEST_WaitForFlushMemTable();
  auto l0_files_2 = collector->GetFlushedFiles();
  ASSERT_OK(db->CompactFiles(CompactionOptions(), l0_files_1, 0));
  ASSERT_OK(db->CompactFiles(CompactionOptions(), l0_files_2, 0));
  // no assertion failure
  delete db;
}

TEST_F(CompactFilesTest, CapturingPendingFiles) {
  Options options;
  options.create_if_missing = true;
  // Disable RocksDB background compaction.
  options.compaction_style = kCompactionStyleNone;
  // Always do full scans for obsolete files (needed to reproduce the issue).
  options.delete_obsolete_files_period_micros = 0;

  // Add listener.
  FlushedFileCollector* collector = new FlushedFileCollector();
  options.listeners.emplace_back(collector);

  DB* db = nullptr;
  DestroyDB(db_name_, options);
  Status s = DB::Open(options, db_name_, &db);
  assert(s.ok());
  assert(db);

  // Create 5 files.
  for (int i = 0; i < 5; ++i) {
    db->Put(WriteOptions(), "key" + ToString(i), "value");
    db->Flush(FlushOptions());
  }

  auto l0_files = collector->GetFlushedFiles();
  EXPECT_EQ(5, l0_files.size());

  rocksdb::SyncPoint::GetInstance()->LoadDependency({
      {"CompactFilesImpl:2", "CompactFilesTest.CapturingPendingFiles:0"},
      {"CompactFilesTest.CapturingPendingFiles:1", "CompactFilesImpl:3"},
  });
  rocksdb::SyncPoint::GetInstance()->EnableProcessing();

  // Start compacting files.
  rocksdb::port::Thread compaction_thread(
      [&] { EXPECT_OK(db->CompactFiles(CompactionOptions(), l0_files, 1)); });

  // In the meantime flush another file.
  TEST_SYNC_POINT("CompactFilesTest.CapturingPendingFiles:0");
  db->Put(WriteOptions(), "key5", "value");
  db->Flush(FlushOptions());
  TEST_SYNC_POINT("CompactFilesTest.CapturingPendingFiles:1");

  compaction_thread.join();

  rocksdb::SyncPoint::GetInstance()->DisableProcessing();

  delete db;

  // Make sure we can reopen the DB.
  s = DB::Open(options, db_name_, &db);
  ASSERT_TRUE(s.ok());
  assert(db);
  delete db;
}

TEST_F(CompactFilesTest, CompactionFilterWithGetSv) {
  class FilterWithGet : public CompactionFilter {
   public:
    virtual bool Filter(int /*level*/, const Slice& /*key*/,
                        const Slice& /*value*/, std::string* /*new_value*/,
                        bool* /*value_changed*/) const override {
      if (db_ == nullptr) {
        return true;
      }
      std::string res;
      db_->Get(ReadOptions(), "", &res);
      return true;
    }

    void SetDB(DB* db) {
      db_ = db;
    }

    virtual const char* Name() const override { return "FilterWithGet"; }

   private:
    DB* db_;
  };


  std::shared_ptr<FilterWithGet> cf(new FilterWithGet());

  Options options;
  options.create_if_missing = true;
  options.compaction_filter = cf.get();

  DB* db = nullptr;
  DestroyDB(db_name_, options);
  Status s = DB::Open(options, db_name_, &db);
  ASSERT_OK(s);

  cf->SetDB(db);

  // Write one L0 file
  db->Put(WriteOptions(), "K1", "V1");
  db->Flush(FlushOptions());

  // Compact all L0 files using CompactFiles
  rocksdb::ColumnFamilyMetaData meta;
  db->GetColumnFamilyMetaData(&meta);
  for (auto& file : meta.levels[0].files) {
    std::string fname = file.db_path + "/" + file.name;
    ASSERT_OK(
        db->CompactFiles(rocksdb::CompactionOptions(), {fname}, 0));
  }


  delete db;
}

TEST_F(CompactFilesTest, SentinelCompressionType) {
  if (!Zlib_Supported()) {
    fprintf(stderr, "zlib compression not supported, skip this test\n");
    return;
  }
  if (!Snappy_Supported()) {
    fprintf(stderr, "snappy compression not supported, skip this test\n");
    return;
  }
  // Check that passing `CompressionType::kDisableCompressionOption` to
  // `CompactFiles` causes it to use the column family compression options.
  for (auto compaction_style :
       {CompactionStyle::kCompactionStyleLevel,
        CompactionStyle::kCompactionStyleUniversal,
        CompactionStyle::kCompactionStyleNone}) {
    DestroyDB(db_name_, Options());
    Options options;
    options.compaction_style = compaction_style;
    // L0: Snappy, L1: ZSTD, L2: Snappy
    options.compression_per_level = {CompressionType::kSnappyCompression,
                                     CompressionType::kZlibCompression,
                                     CompressionType::kSnappyCompression};
    options.create_if_missing = true;
    FlushedFileCollector* collector = new FlushedFileCollector();
    options.listeners.emplace_back(collector);
    DB* db = nullptr;
    ASSERT_OK(DB::Open(options, db_name_, &db));

    db->Put(WriteOptions(), "key", "val");
    db->Flush(FlushOptions());

    auto l0_files = collector->GetFlushedFiles();
    ASSERT_EQ(1, l0_files.size());

    // L0->L1 compaction, so output should be ZSTD-compressed
    CompactionOptions compaction_opts;
    compaction_opts.compression = CompressionType::kDisableCompressionOption;
    ASSERT_OK(db->CompactFiles(compaction_opts, l0_files, 1));

    rocksdb::TablePropertiesCollection all_tables_props;
    ASSERT_OK(db->GetPropertiesOfAllTables(&all_tables_props));
    for (const auto& name_and_table_props : all_tables_props) {
      ASSERT_EQ(CompressionTypeToString(CompressionType::kZlibCompression),
                name_and_table_props.second->compression_name);
    }
    delete db;
  }
}

// ============================================================
// CompactLevel tests
// ============================================================

class CompactLevelTest : public testing::Test {
 public:
  CompactLevelTest() {
    env_ = Env::Default();
    db_name_ = test::PerThreadDBPath("compact_level_test");
  }

  ~CompactLevelTest() {
    if (db_) {
      delete db_;
    }
    DestroyDB(db_name_, last_options_);
  }

  // Common setup: Level compaction, dynamic_level_bytes, merge operator,
  // auto compaction disabled.
  Options GetDefaultOptions() {
    Options options;
    options.create_if_missing = true;
    options.compaction_style = kCompactionStyleLevel;
    options.level_compaction_dynamic_level_bytes = true;
    options.disable_auto_compactions = true;
    options.num_levels = 5;
    options.compression = kNoCompression;
    options.level0_slowdown_writes_trigger = 1000;
    options.level0_stop_writes_trigger = 1000;
    options.write_buffer_size = 64 << 10;  // 64KB
    options.merge_operator.reset(new StringAppendOperator(','));
    return options;
  }

  void OpenDB(const Options& options) {
    last_options_ = options;
    DestroyDB(db_name_, options);
    ASSERT_OK(DB::Open(options, db_name_, &db_));
  }

  // Merge key range [start, end) with given value, then flush.
  void MergeRangeAndFlush(int start, int end, const std::string& value) {
    for (int i = start; i < end; i++) {
      ASSERT_OK(db_->Merge(WriteOptions(), ToString(i), value));
    }
    ASSERT_OK(db_->Flush(FlushOptions()));
    reinterpret_cast<DBImpl*>(db_)->TEST_WaitForFlushMemTable();
  }

  // Collect all SST file names (with path) from a given level.
  std::vector<std::string> GetFileNamesAtLevel(int level) {
    ColumnFamilyMetaData cf_meta;
    db_->GetColumnFamilyMetaData(&cf_meta);
    std::vector<std::string> result;
    for (const auto& lv : cf_meta.levels) {
      if (lv.level == level) {
        for (const auto& f : lv.files) {
          result.push_back(f.db_path + "/" + f.name);
        }
        break;
      }
    }
    return result;
  }

  int GetFileCountAtLevel(int level) {
    ColumnFamilyMetaData cf_meta;
    db_->GetColumnFamilyMetaData(&cf_meta);
    for (const auto& lv : cf_meta.levels) {
      if (lv.level == level) {
        return static_cast<int>(lv.files.size());
      }
    }
    return 0;
  }

  // Move all L0 files to a target level via CompactFiles.
  void MoveL0FilesToLevel(int target_level) {
    auto files = GetFileNamesAtLevel(0);
    if (!files.empty()) {
      CompactionOptions copts;
      copts.compression = kNoCompression;
      ASSERT_OK(db_->CompactFiles(copts, files, target_level));
      reinterpret_cast<DBImpl*>(db_)->TEST_WaitForCompact();
    }
  }

  // Verify all keys in [start, end) have expected value via Get.
  void VerifyKeyRange(int start, int end, const std::string& expected) {
    for (int i = start; i < end; i++) {
      std::string value;
      Status s = db_->Get(ReadOptions(), ToString(i), &value);
      ASSERT_OK(s) << "key=" << i;
      ASSERT_EQ(expected, value) << "key=" << i;
    }
  }

  // Verify keys in [start, end) are not found.
  void VerifyKeyRangeDeleted(int start, int end) {
    for (int i = start; i < end; i++) {
      std::string value;
      Status s = db_->Get(ReadOptions(), ToString(i), &value);
      ASSERT_TRUE(s.IsNotFound()) << "key=" << i << " value=" << value;
    }
  }

  std::string db_name_;
  Env* env_;
  DB* db_ = nullptr;
  Options last_options_;
};

// ---- 一、参数校验类 ----

TEST_F(CompactLevelTest, InvalidInputLevelsEmpty) {
  OpenDB(GetDefaultOptions());
  Status s = db_->CompactLevel({}, 1);
  ASSERT_TRUE(s.IsInvalidArgument()) << s.ToString();
}

TEST_F(CompactLevelTest, InvalidInputLevelOutOfRange) {
  auto options = GetDefaultOptions();
  OpenDB(options);
  // Negative input level
  Status s = db_->CompactLevel({-1}, 1);
  ASSERT_TRUE(s.IsInvalidArgument()) << s.ToString();
  // Input level >= num_levels
  s = db_->CompactLevel({options.num_levels}, options.num_levels);
  ASSERT_TRUE(s.IsInvalidArgument()) << s.ToString();
}

TEST_F(CompactLevelTest, InvalidOutputLevelOutOfRange) {
  auto options = GetDefaultOptions();
  OpenDB(options);
  // Output level >= num_levels
  Status s = db_->CompactLevel({0}, options.num_levels);
  ASSERT_TRUE(s.IsInvalidArgument()) << s.ToString();
  // Negative output level
  s = db_->CompactLevel({0}, -1);
  ASSERT_TRUE(s.IsInvalidArgument()) << s.ToString();
}

TEST_F(CompactLevelTest, InvalidOutputLessThanInput) {
  OpenDB(GetDefaultOptions());
  Status s = db_->CompactLevel({2, 3}, 1);
  ASSERT_TRUE(s.IsInvalidArgument()) << s.ToString();
}

// ---- 二、基础功能类 ----

TEST_F(CompactLevelTest, SingleLevel) {
  OpenDB(GetDefaultOptions());
  // Write data and move to L2.
  MergeRangeAndFlush(0, 200, "a");
  MoveL0FilesToLevel(2);
  ASSERT_GT(GetFileCountAtLevel(2), 0);

  // CompactLevel L2 -> L3.
  ASSERT_OK(db_->CompactLevel({2}, 3));
  ASSERT_EQ(GetFileCountAtLevel(2), 0);
  ASSERT_GT(GetFileCountAtLevel(3), 0);
  VerifyKeyRange(0, 200, "a");
}

TEST_F(CompactLevelTest, MultipleInputLevels) {
  OpenDB(GetDefaultOptions());
  // L2: key 0..199 merge "a"
  MergeRangeAndFlush(0, 200, "a");
  MoveL0FilesToLevel(2);
  // L1: key 100..299 merge "b" (overlapping 100..199)
  MergeRangeAndFlush(100, 300, "b");
  MoveL0FilesToLevel(1);

  ASSERT_OK(db_->CompactLevel({1, 2}, 3));
  ASSERT_EQ(GetFileCountAtLevel(1), 0);
  ASSERT_EQ(GetFileCountAtLevel(2), 0);
  ASSERT_GT(GetFileCountAtLevel(3), 0);

  // key 0..99: only "a"
  VerifyKeyRange(0, 100, "a");
  // key 100..199: merge("a","b") -> "a,b"
  VerifyKeyRange(100, 200, "a,b");
  // key 200..299: only "b"
  VerifyKeyRange(200, 300, "b");
}

TEST_F(CompactLevelTest, SameOutputLevel) {
  OpenDB(GetDefaultOptions());
  // Place data in L2.
  MergeRangeAndFlush(0, 100, "a");
  MoveL0FilesToLevel(2);
  ASSERT_GT(GetFileCountAtLevel(2), 0);

  // CompactLevel L2 -> L2 (rewrite in place).
  ASSERT_OK(db_->CompactLevel({2}, 2));
  ASSERT_GT(GetFileCountAtLevel(2), 0);
  VerifyKeyRange(0, 100, "a");
}

TEST_F(CompactLevelTest, WithL0) {
  OpenDB(GetDefaultOptions());
  // Create multiple overlapping L0 files.
  // Flush 1: key 0..99 merge "a"
  MergeRangeAndFlush(0, 100, "a");
  // Flush 2: key 50..149 merge "b"
  MergeRangeAndFlush(50, 150, "b");
  ASSERT_GE(GetFileCountAtLevel(0), 2);

  ASSERT_OK(db_->CompactLevel({0}, 1));
  ASSERT_EQ(GetFileCountAtLevel(0), 0);
  ASSERT_GT(GetFileCountAtLevel(1), 0);

  // key 0..49: only "a"
  VerifyKeyRange(0, 50, "a");
  // key 50..99: merge("a","b") -> "a,b"
  VerifyKeyRange(50, 100, "a,b");
  // key 100..149: only "b"
  VerifyKeyRange(100, 150, "b");
}

// ---- 三、Bottom Level 场景 ----

TEST_F(CompactLevelTest, BottomLevelAsInputAndOutput) {
  auto options = GetDefaultOptions();
  OpenDB(options);
  const int bottom = options.num_levels - 1;  // L4

  // Place data at bottom level.
  MergeRangeAndFlush(0, 200, "a");
  MoveL0FilesToLevel(bottom);
  ASSERT_GT(GetFileCountAtLevel(bottom), 0);

  // Rewrite bottom level into itself.
  ASSERT_OK(db_->CompactLevel({bottom}, bottom));
  ASSERT_GT(GetFileCountAtLevel(bottom), 0);
  VerifyKeyRange(0, 200, "a");
}

TEST_F(CompactLevelTest, MultiLevelToBottom) {
  auto options = GetDefaultOptions();
  OpenDB(options);
  const int bottom = options.num_levels - 1;  // L4

  // L4(bottom): key 100..399 merge "y"
  MergeRangeAndFlush(100, 400, "y");
  MoveL0FilesToLevel(bottom);
  // L3: key 0..199 merge "x"
  MergeRangeAndFlush(0, 200, "x");
  MoveL0FilesToLevel(3);

  ASSERT_OK(db_->CompactLevel({3, bottom}, bottom));
  ASSERT_EQ(GetFileCountAtLevel(3), 0);
  ASSERT_GT(GetFileCountAtLevel(bottom), 0);

  // key 0..99: only "x"
  VerifyKeyRange(0, 100, "x");
  // key 100..199: merge("y","x") -> "y,x"
  VerifyKeyRange(100, 200, "y,x");
  // key 200..399: only "y"
  VerifyKeyRange(200, 400, "y");
}

TEST_F(CompactLevelTest, BottomRewriteMergeAccumulation) {
  auto options = GetDefaultOptions();
  OpenDB(options);
  const int bottom = options.num_levels - 1;  // L4

  // Multiple rounds of merge on the same keys via different levels,
  // then compact all into bottom. This ensures multiple files land at bottom.
  // Round 1: key 0..99 merge "r1" -> L3
  MergeRangeAndFlush(0, 100, "r1");
  MoveL0FilesToLevel(3);
  // Round 2: key 0..99 merge "r2" -> L2
  MergeRangeAndFlush(0, 100, "r2");
  MoveL0FilesToLevel(2);
  // Round 3: key 0..99 merge "r3" -> L1
  MergeRangeAndFlush(0, 100, "r3");
  MoveL0FilesToLevel(1);

  // Compact all into bottom level.
  ASSERT_OK(db_->CompactLevel({1, 2, 3}, bottom));
  ASSERT_EQ(GetFileCountAtLevel(1), 0);
  ASSERT_EQ(GetFileCountAtLevel(2), 0);
  ASSERT_EQ(GetFileCountAtLevel(3), 0);
  ASSERT_GT(GetFileCountAtLevel(bottom), 0);
  VerifyKeyRange(0, 100, "r1,r2,r3");

  // Now rewrite bottom level into itself — result should stay the same.
  ASSERT_OK(db_->CompactLevel({bottom}, bottom));
  ASSERT_GT(GetFileCountAtLevel(bottom), 0);
  VerifyKeyRange(0, 100, "r1,r2,r3");
}

TEST_F(CompactLevelTest, BottomOutputFileSizeRespected) {
  auto options = GetDefaultOptions();
  options.target_file_size_base = 4 << 10;  // 4KB — small to force splitting
  options.write_buffer_size = 256 << 10;    // 256KB
  OpenDB(options);
  const int bottom = options.num_levels - 1;

  // Write enough data so total >> target_file_size_base.
  // ~500 keys * ~100 bytes value = ~50KB >> 4KB
  for (int i = 0; i < 500; i++) {
    ASSERT_OK(db_->Merge(WriteOptions(), ToString(i),
                          std::string(100, 'v')));
  }
  ASSERT_OK(db_->Flush(FlushOptions()));
  reinterpret_cast<DBImpl*>(db_)->TEST_WaitForFlushMemTable();
  MoveL0FilesToLevel(bottom);

  // Single big file at bottom level.
  ASSERT_GE(GetFileCountAtLevel(bottom), 1);

  ASSERT_OK(db_->CompactLevel({bottom}, bottom));
  // Output should be split into multiple files.
  ASSERT_GT(GetFileCountAtLevel(bottom), 1);

  // Data integrity.
  for (int i = 0; i < 500; i++) {
    std::string value;
    ASSERT_OK(db_->Get(ReadOptions(), ToString(i), &value));
    ASSERT_EQ(std::string(100, 'v'), value);
  }
}

// ---- 四、边界与特殊场景 ----

TEST_F(CompactLevelTest, EmptyInputLevels) {
  OpenDB(GetDefaultOptions());
  // All levels empty — should be a no-op.
  ASSERT_OK(db_->CompactLevel({1, 2}, 3));
}

TEST_F(CompactLevelTest, PartiallyEmptyInputLevels) {
  OpenDB(GetDefaultOptions());
  // Only L1 has files, L2 is empty.
  MergeRangeAndFlush(0, 100, "a");
  MoveL0FilesToLevel(1);
  ASSERT_EQ(GetFileCountAtLevel(2), 0);

  ASSERT_OK(db_->CompactLevel({1, 2}, 3));
  ASSERT_EQ(GetFileCountAtLevel(1), 0);
  ASSERT_GT(GetFileCountAtLevel(3), 0);
  VerifyKeyRange(0, 100, "a");
}

TEST_F(CompactLevelTest, DuplicateInputLevels) {
  OpenDB(GetDefaultOptions());
  MergeRangeAndFlush(0, 100, "a");
  MoveL0FilesToLevel(2);

  // Duplicate level in input — should deduplicate and succeed.
  ASSERT_OK(db_->CompactLevel({2, 2}, 3));
  ASSERT_EQ(GetFileCountAtLevel(2), 0);
  ASSERT_GT(GetFileCountAtLevel(3), 0);
  VerifyKeyRange(0, 100, "a");
}

// ---- 五、冲突与并发类 ----

TEST_F(CompactLevelTest, ConflictWithOngoingCompaction) {
  OpenDB(GetDefaultOptions());
  MergeRangeAndFlush(0, 100, "a");
  MoveL0FilesToLevel(1);

  // Pause first CompactFiles at SyncPoint so files remain "being_compacted".
  rocksdb::SyncPoint::GetInstance()->LoadDependency({
      {"CompactFilesImpl:0", "CompactLevelTest::WaitForPause"},
      {"CompactLevelTest::Resume", "CompactFilesImpl:1"},
  });
  rocksdb::SyncPoint::GetInstance()->EnableProcessing();

  // Launch first compaction in background thread.
  auto files = GetFileNamesAtLevel(1);
  std::thread bg_thread([&]() {
    CompactionOptions copts;
    copts.compression = kNoCompression;
    db_->CompactFiles(copts, files, 2);
  });

  // Wait until first compaction holds the files.
  TEST_SYNC_POINT("CompactLevelTest::WaitForPause");

  // Now CompactLevel should fail because files are being compacted.
  Status s = db_->CompactLevel({1}, 2);
  ASSERT_TRUE(s.IsAborted()) << s.ToString();

  // Let first compaction finish.
  TEST_SYNC_POINT("CompactLevelTest::Resume");
  bg_thread.join();
  rocksdb::SyncPoint::GetInstance()->DisableProcessing();
}

// ---- 六、数据正确性类 ----

TEST_F(CompactLevelTest, MergeValuesCorrectness) {
  OpenDB(GetDefaultOptions());
  // Round 1: key 0..99 merge "a" -> L3
  MergeRangeAndFlush(0, 100, "a");
  MoveL0FilesToLevel(3);
  // Round 2: key 0..99 merge "b" -> L2
  MergeRangeAndFlush(0, 100, "b");
  MoveL0FilesToLevel(2);
  // Round 3: key 0..99 merge "c" -> L1
  MergeRangeAndFlush(0, 100, "c");
  MoveL0FilesToLevel(1);

  ASSERT_OK(db_->CompactLevel({1, 2, 3}, 4));
  ASSERT_EQ(GetFileCountAtLevel(1), 0);
  ASSERT_EQ(GetFileCountAtLevel(2), 0);
  ASSERT_EQ(GetFileCountAtLevel(3), 0);
  ASSERT_GT(GetFileCountAtLevel(4), 0);
  // Merge order: L3("a") oldest -> L2("b") -> L1("c") newest = "a,b,c"
  VerifyKeyRange(0, 100, "a,b,c");
}

TEST_F(CompactLevelTest, WithDeletions) {
  auto options = GetDefaultOptions();
  OpenDB(options);
  const int bottom = options.num_levels - 1;  // L4

  // L3: key 0..99 merge "a"
  MergeRangeAndFlush(0, 100, "a");
  MoveL0FilesToLevel(3);
  // L2: delete key 0..29
  for (int i = 0; i < 30; i++) {
    ASSERT_OK(db_->Delete(WriteOptions(), ToString(i)));
  }
  ASSERT_OK(db_->Flush(FlushOptions()));
  reinterpret_cast<DBImpl*>(db_)->TEST_WaitForFlushMemTable();
  MoveL0FilesToLevel(2);

  // Compact to bottom level — tombstones can be dropped.
  ASSERT_OK(db_->CompactLevel({2, 3}, bottom));
  ASSERT_EQ(GetFileCountAtLevel(2), 0);
  ASSERT_EQ(GetFileCountAtLevel(3), 0);
  ASSERT_GT(GetFileCountAtLevel(bottom), 0);

  VerifyKeyRangeDeleted(0, 30);
  VerifyKeyRange(30, 100, "a");
}

}  // namespace rocksdb

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

#else
#include <stdio.h>

int main(int /*argc*/, char** /*argv*/) {
  fprintf(stderr,
          "SKIPPED as DBImpl::CompactFiles is not supported in ROCKSDB_LITE\n");
  return 0;
}

#endif  // !ROCKSDB_LITE
