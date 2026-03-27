//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#include "db/compaction_picker.h"

#include <limits>
#include <string>
#include <utility>
#include "db/compaction.h"
#include "db/compaction_picker_fifo.h"
#include "db/compaction_picker_universal.h"

#include "util/logging.h"
#include "util/string_util.h"
#include "util/testharness.h"
#include "util/testutil.h"

namespace rocksdb {

class CountingLogger : public Logger {
 public:
  using Logger::Logv;
  virtual void Logv(const char* /*format*/, va_list /*ap*/) override {
    log_count++;
  }
  size_t log_count;
};

class CompactionPickerTest : public testing::Test {
 public:
  const Comparator* ucmp_;
  InternalKeyComparator icmp_;
  Options options_;
  ImmutableCFOptions ioptions_;
  MutableCFOptions mutable_cf_options_;
  LevelCompactionPicker level_compaction_picker;
  std::string cf_name_;
  CountingLogger logger_;
  LogBuffer log_buffer_;
  uint32_t file_num_;
  CompactionOptionsFIFO fifo_options_;
  std::unique_ptr<VersionStorageInfo> vstorage_;
  std::vector<std::unique_ptr<FileMetaData>> files_;
  // does not own FileMetaData
  std::unordered_map<uint32_t, std::pair<FileMetaData*, int>> file_map_;
  // input files to compaction process.
  std::vector<CompactionInputFiles> input_files_;
  int compaction_level_start_;

  CompactionPickerTest()
      : ucmp_(BytewiseComparator()),
        icmp_(ucmp_),
        ioptions_(options_),
        mutable_cf_options_(options_),
        level_compaction_picker(ioptions_, &icmp_),
        cf_name_("dummy"),
        log_buffer_(InfoLogLevel::INFO_LEVEL, &logger_),
        file_num_(1),
        vstorage_(nullptr) {
    fifo_options_.max_table_files_size = 1;
    mutable_cf_options_.RefreshDerivedOptions(ioptions_);
    ioptions_.cf_paths.emplace_back("dummy",
                                    std::numeric_limits<uint64_t>::max());
  }

  ~CompactionPickerTest() {
  }

  void NewVersionStorage(int num_levels, CompactionStyle style) {
    DeleteVersionStorage();
    options_.num_levels = num_levels;
    vstorage_.reset(new VersionStorageInfo(&icmp_, ucmp_, options_.num_levels,
                                           style, nullptr, false));
    vstorage_->CalculateBaseBytes(ioptions_, mutable_cf_options_);
  }

  void DeleteVersionStorage() {
    vstorage_.reset();
    files_.clear();
    file_map_.clear();
    input_files_.clear();
  }

  void Add(int level, uint32_t file_number, const char* smallest,
           const char* largest, uint64_t file_size = 1, uint32_t path_id = 0,
           SequenceNumber smallest_seq = 100,
           SequenceNumber largest_seq = 100) {
    assert(level < vstorage_->num_levels());
    FileMetaData* f = new FileMetaData;
    f->fd = FileDescriptor(file_number, path_id, file_size);
    f->smallest = InternalKey(smallest, smallest_seq, kTypeValue);
    f->largest = InternalKey(largest, largest_seq, kTypeValue);
    f->fd.smallest_seqno = smallest_seq;
    f->fd.largest_seqno = largest_seq;
    f->compensated_file_size = file_size;
    f->refs = 0;
    vstorage_->AddFile(level, f);
    files_.emplace_back(f);
    file_map_.insert({file_number, {f, level}});
  }

  void SetCompactionInputFilesLevels(int level_count, int start_level) {
    input_files_.resize(level_count);
    for (int i = 0; i < level_count; ++i) {
      input_files_[i].level = start_level + i;
    }
    compaction_level_start_ = start_level;
  }

  void AddToCompactionFiles(uint32_t file_number) {
    auto iter = file_map_.find(file_number);
    assert(iter != file_map_.end());
    int level = iter->second.second;
    assert(level < vstorage_->num_levels());
    input_files_[level - compaction_level_start_].files.emplace_back(
        iter->second.first);
  }

  void UpdateVersionStorageInfo() {
    vstorage_->CalculateBaseBytes(ioptions_, mutable_cf_options_);
    vstorage_->UpdateFilesByCompactionPri(ioptions_.compaction_pri);
    vstorage_->UpdateNumNonEmptyLevels();
    vstorage_->GenerateFileIndexer();
    vstorage_->GenerateLevelFilesBrief();
    vstorage_->ComputeCompactionScore(ioptions_, mutable_cf_options_);
    vstorage_->GenerateLevel0NonOverlapping();
    vstorage_->ComputeFilesMarkedForCompaction();
    vstorage_->SetFinalized();
  }
};

TEST_F(CompactionPickerTest, Empty) {
  NewVersionStorage(6, kCompactionStyleLevel);
  UpdateVersionStorageInfo();
  std::unique_ptr<Compaction> compaction(level_compaction_picker.PickCompaction(
      cf_name_, mutable_cf_options_, vstorage_.get(), &log_buffer_));
  ASSERT_TRUE(compaction.get() == nullptr);
}

TEST_F(CompactionPickerTest, Single) {
  NewVersionStorage(6, kCompactionStyleLevel);
  mutable_cf_options_.level0_file_num_compaction_trigger = 2;
  Add(0, 1U, "p", "q");
  UpdateVersionStorageInfo();

  std::unique_ptr<Compaction> compaction(level_compaction_picker.PickCompaction(
      cf_name_, mutable_cf_options_, vstorage_.get(), &log_buffer_));
  ASSERT_TRUE(compaction.get() == nullptr);
}

TEST_F(CompactionPickerTest, Level0Trigger) {
  NewVersionStorage(6, kCompactionStyleLevel);
  mutable_cf_options_.level0_file_num_compaction_trigger = 2;
  Add(0, 1U, "150", "200");
  Add(0, 2U, "200", "250");

  UpdateVersionStorageInfo();

  std::unique_ptr<Compaction> compaction(level_compaction_picker.PickCompaction(
      cf_name_, mutable_cf_options_, vstorage_.get(), &log_buffer_));
  ASSERT_TRUE(compaction.get() != nullptr);
  ASSERT_EQ(2U, compaction->num_input_files(0));
  ASSERT_EQ(1U, compaction->input(0, 0)->fd.GetNumber());
  ASSERT_EQ(2U, compaction->input(0, 1)->fd.GetNumber());
}

TEST_F(CompactionPickerTest, Level1Trigger) {
  NewVersionStorage(6, kCompactionStyleLevel);
  Add(1, 66U, "150", "200", 1000000000U);
  UpdateVersionStorageInfo();

  std::unique_ptr<Compaction> compaction(level_compaction_picker.PickCompaction(
      cf_name_, mutable_cf_options_, vstorage_.get(), &log_buffer_));
  ASSERT_TRUE(compaction.get() != nullptr);
  ASSERT_EQ(1U, compaction->num_input_files(0));
  ASSERT_EQ(66U, compaction->input(0, 0)->fd.GetNumber());
}

TEST_F(CompactionPickerTest, Level1Trigger2) {
  mutable_cf_options_.target_file_size_base = 10000000000;
  mutable_cf_options_.RefreshDerivedOptions(ioptions_);
  NewVersionStorage(6, kCompactionStyleLevel);
  Add(1, 66U, "150", "200", 1000000001U);
  Add(1, 88U, "201", "300", 1000000000U);
  Add(2, 6U, "150", "179", 1000000000U);
  Add(2, 7U, "180", "220", 1000000000U);
  Add(2, 8U, "221", "300", 1000000000U);
  UpdateVersionStorageInfo();

  std::unique_ptr<Compaction> compaction(level_compaction_picker.PickCompaction(
      cf_name_, mutable_cf_options_, vstorage_.get(), &log_buffer_));
  ASSERT_TRUE(compaction.get() != nullptr);
  ASSERT_EQ(1U, compaction->num_input_files(0));
  ASSERT_EQ(2U, compaction->num_input_files(1));
  ASSERT_EQ(66U, compaction->input(0, 0)->fd.GetNumber());
  ASSERT_EQ(6U, compaction->input(1, 0)->fd.GetNumber());
  ASSERT_EQ(7U, compaction->input(1, 1)->fd.GetNumber());
  ASSERT_EQ(uint64_t{1073741824}, compaction->OutputFilePreallocationSize());
}

TEST_F(CompactionPickerTest, LevelMaxScore) {
  NewVersionStorage(6, kCompactionStyleLevel);
  mutable_cf_options_.target_file_size_base = 10000000;
  mutable_cf_options_.max_bytes_for_level_base = 10 * 1024 * 1024;
  mutable_cf_options_.RefreshDerivedOptions(ioptions_);
  Add(0, 1U, "150", "200", 1000000U);
  // Level 1 score 1.2
  Add(1, 66U, "150", "200", 6000000U);
  Add(1, 88U, "201", "300", 6000000U);
  // Level 2 score 1.8. File 7 is the largest. Should be picked
  Add(2, 6U, "150", "179", 60000000U);
  Add(2, 7U, "180", "220", 60000001U);
  Add(2, 8U, "221", "300", 60000000U);
  // Level 3 score slightly larger than 1
  Add(3, 26U, "150", "170", 260000000U);
  Add(3, 27U, "171", "179", 260000000U);
  Add(3, 28U, "191", "220", 260000000U);
  Add(3, 29U, "221", "300", 260000000U);
  UpdateVersionStorageInfo();

  std::unique_ptr<Compaction> compaction(level_compaction_picker.PickCompaction(
      cf_name_, mutable_cf_options_, vstorage_.get(), &log_buffer_));
  ASSERT_TRUE(compaction.get() != nullptr);
  ASSERT_EQ(1U, compaction->num_input_files(0));
  ASSERT_EQ(7U, compaction->input(0, 0)->fd.GetNumber());
  ASSERT_EQ(mutable_cf_options_.target_file_size_base +
                mutable_cf_options_.target_file_size_base / 10,
            compaction->OutputFilePreallocationSize());
}

TEST_F(CompactionPickerTest, NeedsCompactionLevel) {
  const int kLevels = 6;
  const int kFileCount = 20;

  for (int level = 0; level < kLevels - 1; ++level) {
    NewVersionStorage(kLevels, kCompactionStyleLevel);
    uint64_t file_size = vstorage_->MaxBytesForLevel(level) * 2 / kFileCount;
    for (int file_count = 1; file_count <= kFileCount; ++file_count) {
      // start a brand new version in each test.
      NewVersionStorage(kLevels, kCompactionStyleLevel);
      for (int i = 0; i < file_count; ++i) {
        Add(level, i, ToString((i + 100) * 1000).c_str(),
            ToString((i + 100) * 1000 + 999).c_str(),
            file_size, 0, i * 100, i * 100 + 99);
      }
      UpdateVersionStorageInfo();
      ASSERT_EQ(vstorage_->CompactionScoreLevel(0), level);
      ASSERT_EQ(level_compaction_picker.NeedsCompaction(vstorage_.get()),
                vstorage_->CompactionScore(0) >= 1);
      // release the version storage
      DeleteVersionStorage();
    }
  }
}

TEST_F(CompactionPickerTest, Level0TriggerDynamic) {
  int num_levels = ioptions_.num_levels;
  ioptions_.level_compaction_dynamic_level_bytes = true;
  mutable_cf_options_.level0_file_num_compaction_trigger = 2;
  mutable_cf_options_.max_bytes_for_level_base = 200;
  mutable_cf_options_.max_bytes_for_level_multiplier = 10;
  NewVersionStorage(num_levels, kCompactionStyleLevel);
  Add(0, 1U, "150", "200");
  Add(0, 2U, "200", "250");

  UpdateVersionStorageInfo();

  std::unique_ptr<Compaction> compaction(level_compaction_picker.PickCompaction(
      cf_name_, mutable_cf_options_, vstorage_.get(), &log_buffer_));
  ASSERT_TRUE(compaction.get() != nullptr);
  ASSERT_EQ(2U, compaction->num_input_files(0));
  ASSERT_EQ(1U, compaction->input(0, 0)->fd.GetNumber());
  ASSERT_EQ(2U, compaction->input(0, 1)->fd.GetNumber());
  ASSERT_EQ(1, static_cast<int>(compaction->num_input_levels()));
  ASSERT_EQ(num_levels - 1, compaction->output_level());
}

TEST_F(CompactionPickerTest, Level0TriggerDynamic2) {
  int num_levels = ioptions_.num_levels;
  ioptions_.level_compaction_dynamic_level_bytes = true;
  mutable_cf_options_.level0_file_num_compaction_trigger = 2;
  mutable_cf_options_.max_bytes_for_level_base = 200;
  mutable_cf_options_.max_bytes_for_level_multiplier = 10;
  NewVersionStorage(num_levels, kCompactionStyleLevel);
  Add(0, 1U, "150", "200");
  Add(0, 2U, "200", "250");
  Add(num_levels - 1, 3U, "200", "250", 300U);

  UpdateVersionStorageInfo();
  ASSERT_EQ(vstorage_->base_level(), num_levels - 2);

  std::unique_ptr<Compaction> compaction(level_compaction_picker.PickCompaction(
      cf_name_, mutable_cf_options_, vstorage_.get(), &log_buffer_));
  ASSERT_TRUE(compaction.get() != nullptr);
  ASSERT_EQ(2U, compaction->num_input_files(0));
  ASSERT_EQ(1U, compaction->input(0, 0)->fd.GetNumber());
  ASSERT_EQ(2U, compaction->input(0, 1)->fd.GetNumber());
  ASSERT_EQ(1, static_cast<int>(compaction->num_input_levels()));
  ASSERT_EQ(num_levels - 2, compaction->output_level());
}

TEST_F(CompactionPickerTest, Level0TriggerDynamic3) {
  int num_levels = ioptions_.num_levels;
  ioptions_.level_compaction_dynamic_level_bytes = true;
  mutable_cf_options_.level0_file_num_compaction_trigger = 2;
  mutable_cf_options_.max_bytes_for_level_base = 200;
  mutable_cf_options_.max_bytes_for_level_multiplier = 10;
  NewVersionStorage(num_levels, kCompactionStyleLevel);
  Add(0, 1U, "150", "200");
  Add(0, 2U, "200", "250");
  Add(num_levels - 1, 3U, "200", "250", 300U);
  Add(num_levels - 1, 4U, "300", "350", 3000U);

  UpdateVersionStorageInfo();
  ASSERT_EQ(vstorage_->base_level(), num_levels - 3);

  std::unique_ptr<Compaction> compaction(level_compaction_picker.PickCompaction(
      cf_name_, mutable_cf_options_, vstorage_.get(), &log_buffer_));
  ASSERT_TRUE(compaction.get() != nullptr);
  ASSERT_EQ(2U, compaction->num_input_files(0));
  ASSERT_EQ(1U, compaction->input(0, 0)->fd.GetNumber());
  ASSERT_EQ(2U, compaction->input(0, 1)->fd.GetNumber());
  ASSERT_EQ(1, static_cast<int>(compaction->num_input_levels()));
  ASSERT_EQ(num_levels - 3, compaction->output_level());
}

TEST_F(CompactionPickerTest, Level0TriggerDynamic4) {
  int num_levels = ioptions_.num_levels;
  ioptions_.level_compaction_dynamic_level_bytes = true;
  mutable_cf_options_.level0_file_num_compaction_trigger = 2;
  mutable_cf_options_.max_bytes_for_level_base = 200;
  mutable_cf_options_.max_bytes_for_level_multiplier = 10;

  NewVersionStorage(num_levels, kCompactionStyleLevel);
  Add(0, 1U, "150", "200");
  Add(0, 2U, "200", "250");
  Add(num_levels - 1, 3U, "200", "250", 300U);
  Add(num_levels - 1, 4U, "300", "350", 3000U);
  Add(num_levels - 3, 5U, "150", "180", 3U);
  Add(num_levels - 3, 6U, "181", "300", 3U);
  Add(num_levels - 3, 7U, "400", "450", 3U);

  UpdateVersionStorageInfo();
  ASSERT_EQ(vstorage_->base_level(), num_levels - 3);

  std::unique_ptr<Compaction> compaction(level_compaction_picker.PickCompaction(
      cf_name_, mutable_cf_options_, vstorage_.get(), &log_buffer_));
  ASSERT_TRUE(compaction.get() != nullptr);
  ASSERT_EQ(2U, compaction->num_input_files(0));
  ASSERT_EQ(1U, compaction->input(0, 0)->fd.GetNumber());
  ASSERT_EQ(2U, compaction->input(0, 1)->fd.GetNumber());
  ASSERT_EQ(2U, compaction->num_input_files(1));
  ASSERT_EQ(num_levels - 3, compaction->level(1));
  ASSERT_EQ(5U, compaction->input(1, 0)->fd.GetNumber());
  ASSERT_EQ(6U, compaction->input(1, 1)->fd.GetNumber());
  ASSERT_EQ(2, static_cast<int>(compaction->num_input_levels()));
  ASSERT_EQ(num_levels - 3, compaction->output_level());
}

TEST_F(CompactionPickerTest, LevelTriggerDynamic4) {
  int num_levels = ioptions_.num_levels;
  ioptions_.level_compaction_dynamic_level_bytes = true;
  ioptions_.compaction_pri = kMinOverlappingRatio;
  mutable_cf_options_.level0_file_num_compaction_trigger = 2;
  mutable_cf_options_.max_bytes_for_level_base = 200;
  mutable_cf_options_.max_bytes_for_level_multiplier = 10;
  NewVersionStorage(num_levels, kCompactionStyleLevel);
  Add(0, 1U, "150", "200");
  Add(num_levels - 1, 3U, "200", "250", 300U);
  Add(num_levels - 1, 4U, "300", "350", 3000U);
  Add(num_levels - 1, 4U, "400", "450", 3U);
  Add(num_levels - 2, 5U, "150", "180", 300U);
  Add(num_levels - 2, 6U, "181", "350", 500U);
  Add(num_levels - 2, 7U, "400", "450", 200U);

  UpdateVersionStorageInfo();

  std::unique_ptr<Compaction> compaction(level_compaction_picker.PickCompaction(
      cf_name_, mutable_cf_options_, vstorage_.get(), &log_buffer_));
  ASSERT_TRUE(compaction.get() != nullptr);
  ASSERT_EQ(1U, compaction->num_input_files(0));
  ASSERT_EQ(5U, compaction->input(0, 0)->fd.GetNumber());
  ASSERT_EQ(0, compaction->num_input_files(1));
  ASSERT_EQ(1U, compaction->num_input_levels());
  ASSERT_EQ(num_levels - 1, compaction->output_level());
}

// Universal and FIFO Compactions are not supported in ROCKSDB_LITE
#ifndef ROCKSDB_LITE
TEST_F(CompactionPickerTest, NeedsCompactionUniversal) {
  NewVersionStorage(1, kCompactionStyleUniversal);
  UniversalCompactionPicker universal_compaction_picker(
      ioptions_, &icmp_);
  UpdateVersionStorageInfo();
  // must return false when there's no files.
  ASSERT_EQ(universal_compaction_picker.NeedsCompaction(vstorage_.get()),
            false);

  // verify the trigger given different number of L0 files.
  for (int i = 1;
       i <= mutable_cf_options_.level0_file_num_compaction_trigger * 2; ++i) {
    NewVersionStorage(1, kCompactionStyleUniversal);
    Add(0, i, ToString((i + 100) * 1000).c_str(),
        ToString((i + 100) * 1000 + 999).c_str(), 1000000, 0, i * 100,
        i * 100 + 99);
    UpdateVersionStorageInfo();
    ASSERT_EQ(level_compaction_picker.NeedsCompaction(vstorage_.get()),
              vstorage_->CompactionScore(0) >= 1);
  }
}

TEST_F(CompactionPickerTest, CompactionUniversalIngestBehindReservedLevel) {
  const uint64_t kFileSize = 100000;
  NewVersionStorage(1, kCompactionStyleUniversal);
  ioptions_.allow_ingest_behind = true;
  ioptions_.num_levels = 3;
  UniversalCompactionPicker universal_compaction_picker(ioptions_, &icmp_);
  UpdateVersionStorageInfo();
  // must return false when there's no files.
  ASSERT_EQ(universal_compaction_picker.NeedsCompaction(vstorage_.get()),
            false);

  NewVersionStorage(3, kCompactionStyleUniversal);

  Add(0, 1U, "150", "200", kFileSize, 0, 500, 550);
  Add(0, 2U, "201", "250", kFileSize, 0, 401, 450);
  Add(0, 4U, "260", "300", kFileSize, 0, 260, 300);
  Add(1, 5U, "100", "151", kFileSize, 0, 200, 251);
  Add(1, 3U, "301", "350", kFileSize, 0, 101, 150);
  Add(2, 6U, "120", "200", kFileSize, 0, 20, 100);

  UpdateVersionStorageInfo();

  std::unique_ptr<Compaction> compaction(
      universal_compaction_picker.PickCompaction(
          cf_name_, mutable_cf_options_, vstorage_.get(), &log_buffer_));

  // output level should be the one above the bottom-most
  ASSERT_EQ(1, compaction->output_level());
}
// Tests if the files can be trivially moved in multi level
// universal compaction when allow_trivial_move option is set
// In this test as the input files overlaps, they cannot
// be trivially moved.

TEST_F(CompactionPickerTest, CannotTrivialMoveUniversal) {
  const uint64_t kFileSize = 100000;

  mutable_cf_options_.compaction_options_universal.allow_trivial_move = true;
  NewVersionStorage(1, kCompactionStyleUniversal);
  UniversalCompactionPicker universal_compaction_picker(ioptions_, &icmp_);
  UpdateVersionStorageInfo();
  // must return false when there's no files.
  ASSERT_EQ(universal_compaction_picker.NeedsCompaction(vstorage_.get()),
            false);

  NewVersionStorage(3, kCompactionStyleUniversal);

  Add(0, 1U, "150", "200", kFileSize, 0, 500, 550);
  Add(0, 2U, "201", "250", kFileSize, 0, 401, 450);
  Add(0, 4U, "260", "300", kFileSize, 0, 260, 300);
  Add(1, 5U, "100", "151", kFileSize, 0, 200, 251);
  Add(1, 3U, "301", "350", kFileSize, 0, 101, 150);
  Add(2, 6U, "120", "200", kFileSize, 0, 20, 100);

  UpdateVersionStorageInfo();

  std::unique_ptr<Compaction> compaction(
      universal_compaction_picker.PickCompaction(
          cf_name_, mutable_cf_options_, vstorage_.get(), &log_buffer_));

  ASSERT_TRUE(!compaction->is_trivial_move());
}
// Tests if the files can be trivially moved in multi level
// universal compaction when allow_trivial_move option is set
// In this test as the input files doesn't overlaps, they should
// be trivially moved.
TEST_F(CompactionPickerTest, AllowsTrivialMoveUniversal) {
  const uint64_t kFileSize = 100000;

  mutable_cf_options_.compaction_options_universal.allow_trivial_move = true;
  UniversalCompactionPicker universal_compaction_picker(ioptions_, &icmp_);

  NewVersionStorage(3, kCompactionStyleUniversal);

  Add(0, 1U, "150", "200", kFileSize, 0, 500, 550);
  Add(0, 2U, "201", "250", kFileSize, 0, 401, 450);
  Add(0, 4U, "260", "300", kFileSize, 0, 260, 300);
  Add(1, 5U, "010", "080", kFileSize, 0, 200, 251);
  Add(2, 3U, "301", "350", kFileSize, 0, 101, 150);

  UpdateVersionStorageInfo();

  std::unique_ptr<Compaction> compaction(
      universal_compaction_picker.PickCompaction(
          cf_name_, mutable_cf_options_, vstorage_.get(), &log_buffer_));

  ASSERT_TRUE(compaction->is_trivial_move());
}

TEST_F(CompactionPickerTest, NeedsCompactionFIFO) {
  NewVersionStorage(1, kCompactionStyleFIFO);
  const int kFileCount =
      mutable_cf_options_.level0_file_num_compaction_trigger * 3;
  const uint64_t kFileSize = 100000;
  const uint64_t kMaxSize = kFileSize * kFileCount / 2;

  fifo_options_.max_table_files_size = kMaxSize;
  mutable_cf_options_.compaction_options_fifo = fifo_options_;
  FIFOCompactionPicker fifo_compaction_picker(ioptions_, &icmp_);
  UpdateVersionStorageInfo();
  // must return false when there's no files.
  ASSERT_EQ(fifo_compaction_picker.NeedsCompaction(vstorage_.get()), false);

  // verify whether compaction is needed based on the current
  // size of L0 files.
  uint64_t current_size = 0;
  for (int i = 1; i <= kFileCount; ++i) {
    NewVersionStorage(1, kCompactionStyleFIFO);
    Add(0, i, ToString((i + 100) * 1000).c_str(),
        ToString((i + 100) * 1000 + 999).c_str(),
        kFileSize, 0, i * 100, i * 100 + 99);
    current_size += kFileSize;
    UpdateVersionStorageInfo();
    ASSERT_EQ(fifo_compaction_picker.NeedsCompaction(vstorage_.get()),
              vstorage_->CompactionScore(0) >= 1);
  }
}
#endif  // ROCKSDB_LITE

TEST_F(CompactionPickerTest, CompactionPriMinOverlapping1) {
  NewVersionStorage(6, kCompactionStyleLevel);
  ioptions_.compaction_pri = kMinOverlappingRatio;
  mutable_cf_options_.target_file_size_base = 100000000000;
  mutable_cf_options_.target_file_size_multiplier = 10;
  mutable_cf_options_.max_bytes_for_level_base = 10 * 1024 * 1024;
  mutable_cf_options_.RefreshDerivedOptions(ioptions_);

  Add(2, 6U, "150", "179", 50000000U);
  Add(2, 7U, "180", "220", 50000000U);
  Add(2, 8U, "321", "400", 50000000U);  // File not overlapping
  Add(2, 9U, "721", "800", 50000000U);

  Add(3, 26U, "150", "170", 260000000U);
  Add(3, 27U, "171", "179", 260000000U);
  Add(3, 28U, "191", "220", 260000000U);
  Add(3, 29U, "221", "300", 260000000U);
  Add(3, 30U, "750", "900", 260000000U);
  UpdateVersionStorageInfo();

  std::unique_ptr<Compaction> compaction(level_compaction_picker.PickCompaction(
      cf_name_, mutable_cf_options_, vstorage_.get(), &log_buffer_));
  ASSERT_TRUE(compaction.get() != nullptr);
  ASSERT_EQ(1U, compaction->num_input_files(0));
  // Pick file 8 because it overlaps with 0 files on level 3.
  ASSERT_EQ(8U, compaction->input(0, 0)->fd.GetNumber());
  // Compaction input size * 1.1
  ASSERT_GE(uint64_t{55000000}, compaction->OutputFilePreallocationSize());
}

TEST_F(CompactionPickerTest, CompactionPriMinOverlapping2) {
  NewVersionStorage(6, kCompactionStyleLevel);
  ioptions_.compaction_pri = kMinOverlappingRatio;
  mutable_cf_options_.target_file_size_base = 10000000;
  mutable_cf_options_.target_file_size_multiplier = 10;
  mutable_cf_options_.max_bytes_for_level_base = 10 * 1024 * 1024;

  Add(2, 6U, "150", "175",
      60000000U);  // Overlaps with file 26, 27, total size 521M
  Add(2, 7U, "176", "200", 60000000U);  // Overlaps with file 27, 28, total size
                                        // 520M, the smalelst overlapping
  Add(2, 8U, "201", "300",
      60000000U);  // Overlaps with file 28, 29, total size 521M

  Add(3, 26U, "100", "110", 261000000U);
  Add(3, 26U, "150", "170", 261000000U);
  Add(3, 27U, "171", "179", 260000000U);
  Add(3, 28U, "191", "220", 260000000U);
  Add(3, 29U, "221", "300", 261000000U);
  Add(3, 30U, "321", "400", 261000000U);
  UpdateVersionStorageInfo();

  std::unique_ptr<Compaction> compaction(level_compaction_picker.PickCompaction(
      cf_name_, mutable_cf_options_, vstorage_.get(), &log_buffer_));
  ASSERT_TRUE(compaction.get() != nullptr);
  ASSERT_EQ(1U, compaction->num_input_files(0));
  // Picking file 7 because overlapping ratio is the biggest.
  ASSERT_EQ(7U, compaction->input(0, 0)->fd.GetNumber());
}

TEST_F(CompactionPickerTest, CompactionPriMinOverlapping3) {
  NewVersionStorage(6, kCompactionStyleLevel);
  ioptions_.compaction_pri = kMinOverlappingRatio;
  mutable_cf_options_.max_bytes_for_level_base = 10000000;
  mutable_cf_options_.max_bytes_for_level_multiplier = 10;

  // file 7 and 8 over lap with the same file, but file 8 is smaller so
  // it will be picked.
  Add(2, 6U, "150", "167", 60000000U);  // Overlaps with file 26, 27
  Add(2, 7U, "168", "169", 60000000U);  // Overlaps with file 27
  Add(2, 8U, "201", "300", 61000000U);  // Overlaps with file 28, but the file
                                        // itself is larger. Should be picked.

  Add(3, 26U, "160", "165", 260000000U);
  Add(3, 27U, "166", "170", 260000000U);
  Add(3, 28U, "180", "400", 260000000U);
  Add(3, 29U, "401", "500", 260000000U);
  UpdateVersionStorageInfo();

  std::unique_ptr<Compaction> compaction(level_compaction_picker.PickCompaction(
      cf_name_, mutable_cf_options_, vstorage_.get(), &log_buffer_));
  ASSERT_TRUE(compaction.get() != nullptr);
  ASSERT_EQ(1U, compaction->num_input_files(0));
  // Picking file 8 because overlapping ratio is the biggest.
  ASSERT_EQ(8U, compaction->input(0, 0)->fd.GetNumber());
}

// This test exhibits the bug where we don't properly reset parent_index in
// PickCompaction()
TEST_F(CompactionPickerTest, ParentIndexResetBug) {
  int num_levels = ioptions_.num_levels;
  mutable_cf_options_.level0_file_num_compaction_trigger = 2;
  mutable_cf_options_.max_bytes_for_level_base = 200;
  NewVersionStorage(num_levels, kCompactionStyleLevel);
  Add(0, 1U, "150", "200");       // <- marked for compaction
  Add(1, 3U, "400", "500", 600);  // <- this one needs compacting
  Add(2, 4U, "150", "200");
  Add(2, 5U, "201", "210");
  Add(2, 6U, "300", "310");
  Add(2, 7U, "400", "500");  // <- being compacted

  vstorage_->LevelFiles(2)[3]->being_compacted = true;
  vstorage_->LevelFiles(0)[0]->marked_for_compaction = true;

  UpdateVersionStorageInfo();

  std::unique_ptr<Compaction> compaction(level_compaction_picker.PickCompaction(
      cf_name_, mutable_cf_options_, vstorage_.get(), &log_buffer_));
}

// This test checks ExpandWhileOverlapping() by having overlapping user keys
// ranges (with different sequence numbers) in the input files.
TEST_F(CompactionPickerTest, OverlappingUserKeys) {
  NewVersionStorage(6, kCompactionStyleLevel);
  ioptions_.compaction_pri = kByCompensatedSize;

  Add(1, 1U, "100", "150", 1U);
  // Overlapping user keys
  Add(1, 2U, "200", "400", 1U);
  Add(1, 3U, "400", "500", 1000000000U, 0, 0);
  Add(2, 4U, "600", "700", 1U);
  UpdateVersionStorageInfo();

  std::unique_ptr<Compaction> compaction(level_compaction_picker.PickCompaction(
              cf_name_, mutable_cf_options_, vstorage_.get(), &log_buffer_));
  ASSERT_TRUE(compaction.get() != nullptr);
  ASSERT_EQ(1U, compaction->num_input_levels());
  ASSERT_EQ(2U, compaction->num_input_files(0));
  ASSERT_EQ(2U, compaction->input(0, 0)->fd.GetNumber());
  ASSERT_EQ(3U, compaction->input(0, 1)->fd.GetNumber());
}

TEST_F(CompactionPickerTest, OverlappingUserKeys2) {
  NewVersionStorage(6, kCompactionStyleLevel);
  // Overlapping user keys on same level and output level
  Add(1, 1U, "200", "400", 1000000000U);
  Add(1, 2U, "400", "500", 1U, 0, 0);
  Add(2, 3U, "000", "100", 1U);
  Add(2, 4U, "100", "600", 1U, 0, 0);
  Add(2, 5U, "600", "700", 1U, 0, 0);
  UpdateVersionStorageInfo();

  std::unique_ptr<Compaction> compaction(level_compaction_picker.PickCompaction(
              cf_name_, mutable_cf_options_, vstorage_.get(), &log_buffer_));
  ASSERT_TRUE(compaction.get() != nullptr);
  ASSERT_EQ(2U, compaction->num_input_levels());
  ASSERT_EQ(2U, compaction->num_input_files(0));
  ASSERT_EQ(3U, compaction->num_input_files(1));
  ASSERT_EQ(1U, compaction->input(0, 0)->fd.GetNumber());
  ASSERT_EQ(2U, compaction->input(0, 1)->fd.GetNumber());
  ASSERT_EQ(3U, compaction->input(1, 0)->fd.GetNumber());
  ASSERT_EQ(4U, compaction->input(1, 1)->fd.GetNumber());
  ASSERT_EQ(5U, compaction->input(1, 2)->fd.GetNumber());
}

TEST_F(CompactionPickerTest, OverlappingUserKeys3) {
  NewVersionStorage(6, kCompactionStyleLevel);
  // Chain of overlapping user key ranges (forces ExpandWhileOverlapping() to
  // expand multiple times)
  Add(1, 1U, "100", "150", 1U);
  Add(1, 2U, "150", "200", 1U, 0, 0);
  Add(1, 3U, "200", "250", 1000000000U, 0, 0);
  Add(1, 4U, "250", "300", 1U, 0, 0);
  Add(1, 5U, "300", "350", 1U, 0, 0);
  // Output level overlaps with the beginning and the end of the chain
  Add(2, 6U, "050", "100", 1U);
  Add(2, 7U, "350", "400", 1U);
  UpdateVersionStorageInfo();

  std::unique_ptr<Compaction> compaction(level_compaction_picker.PickCompaction(
              cf_name_, mutable_cf_options_, vstorage_.get(), &log_buffer_));
  ASSERT_TRUE(compaction.get() != nullptr);
  ASSERT_EQ(2U, compaction->num_input_levels());
  ASSERT_EQ(5U, compaction->num_input_files(0));
  ASSERT_EQ(2U, compaction->num_input_files(1));
  ASSERT_EQ(1U, compaction->input(0, 0)->fd.GetNumber());
  ASSERT_EQ(2U, compaction->input(0, 1)->fd.GetNumber());
  ASSERT_EQ(3U, compaction->input(0, 2)->fd.GetNumber());
  ASSERT_EQ(4U, compaction->input(0, 3)->fd.GetNumber());
  ASSERT_EQ(5U, compaction->input(0, 4)->fd.GetNumber());
  ASSERT_EQ(6U, compaction->input(1, 0)->fd.GetNumber());
  ASSERT_EQ(7U, compaction->input(1, 1)->fd.GetNumber());
}

TEST_F(CompactionPickerTest, OverlappingUserKeys4) {
  NewVersionStorage(6, kCompactionStyleLevel);
  mutable_cf_options_.max_bytes_for_level_base = 1000000;

  Add(1, 1U, "100", "150", 1U);
  Add(1, 2U, "150", "199", 1U, 0, 0);
  Add(1, 3U, "200", "250", 1100000U, 0, 0);
  Add(1, 4U, "251", "300", 1U, 0, 0);
  Add(1, 5U, "300", "350", 1U, 0, 0);

  Add(2, 6U, "100", "115", 1U);
  Add(2, 7U, "125", "325", 1U);
  Add(2, 8U, "350", "400", 1U);
  UpdateVersionStorageInfo();

  std::unique_ptr<Compaction> compaction(level_compaction_picker.PickCompaction(
      cf_name_, mutable_cf_options_, vstorage_.get(), &log_buffer_));
  ASSERT_TRUE(compaction.get() != nullptr);
  ASSERT_EQ(2U, compaction->num_input_levels());
  ASSERT_EQ(1U, compaction->num_input_files(0));
  ASSERT_EQ(1U, compaction->num_input_files(1));
  ASSERT_EQ(3U, compaction->input(0, 0)->fd.GetNumber());
  ASSERT_EQ(7U, compaction->input(1, 0)->fd.GetNumber());
}

TEST_F(CompactionPickerTest, OverlappingUserKeys5) {
  NewVersionStorage(6, kCompactionStyleLevel);
  // Overlapping user keys on same level and output level
  Add(1, 1U, "200", "400", 1000000000U);
  Add(1, 2U, "400", "500", 1U, 0, 0);
  Add(2, 3U, "000", "100", 1U);
  Add(2, 4U, "100", "600", 1U, 0, 0);
  Add(2, 5U, "600", "700", 1U, 0, 0);

  vstorage_->LevelFiles(2)[2]->being_compacted = true;

  UpdateVersionStorageInfo();

  std::unique_ptr<Compaction> compaction(level_compaction_picker.PickCompaction(
      cf_name_, mutable_cf_options_, vstorage_.get(), &log_buffer_));
  ASSERT_TRUE(compaction.get() == nullptr);
}

TEST_F(CompactionPickerTest, OverlappingUserKeys6) {
  NewVersionStorage(6, kCompactionStyleLevel);
  // Overlapping user keys on same level and output level
  Add(1, 1U, "200", "400", 1U, 0, 0);
  Add(1, 2U, "401", "500", 1U, 0, 0);
  Add(2, 3U, "000", "100", 1U);
  Add(2, 4U, "100", "300", 1U, 0, 0);
  Add(2, 5U, "305", "450", 1U, 0, 0);
  Add(2, 6U, "460", "600", 1U, 0, 0);
  Add(2, 7U, "600", "700", 1U, 0, 0);

  vstorage_->LevelFiles(1)[0]->marked_for_compaction = true;
  vstorage_->LevelFiles(1)[1]->marked_for_compaction = true;

  UpdateVersionStorageInfo();

  std::unique_ptr<Compaction> compaction(level_compaction_picker.PickCompaction(
      cf_name_, mutable_cf_options_, vstorage_.get(), &log_buffer_));
  ASSERT_TRUE(compaction.get() != nullptr);
  ASSERT_EQ(2U, compaction->num_input_levels());
  ASSERT_EQ(1U, compaction->num_input_files(0));
  ASSERT_EQ(3U, compaction->num_input_files(1));
}

TEST_F(CompactionPickerTest, OverlappingUserKeys7) {
  NewVersionStorage(6, kCompactionStyleLevel);
  mutable_cf_options_.max_compaction_bytes = 100000000000u;
  // Overlapping user keys on same level and output level
  Add(1, 1U, "200", "400", 1U, 0, 0);
  Add(1, 2U, "401", "500", 1000000000U, 0, 0);
  Add(2, 3U, "100", "250", 1U);
  Add(2, 4U, "300", "600", 1U, 0, 0);
  Add(2, 5U, "600", "800", 1U, 0, 0);

  UpdateVersionStorageInfo();

  std::unique_ptr<Compaction> compaction(level_compaction_picker.PickCompaction(
      cf_name_, mutable_cf_options_, vstorage_.get(), &log_buffer_));
  ASSERT_TRUE(compaction.get() != nullptr);
  ASSERT_EQ(2U, compaction->num_input_levels());
  ASSERT_GE(1U, compaction->num_input_files(0));
  ASSERT_GE(2U, compaction->num_input_files(1));
  // File 5 has to be included in the compaction
  ASSERT_EQ(5U, compaction->inputs(1)->back()->fd.GetNumber());
}

TEST_F(CompactionPickerTest, OverlappingUserKeys8) {
  NewVersionStorage(6, kCompactionStyleLevel);
  mutable_cf_options_.max_compaction_bytes = 100000000000u;
  // grow the number of inputs in "level" without
  // changing the number of "level+1" files we pick up
  // Expand input level as much as possible
  // no overlapping case
  Add(1, 1U, "101", "150", 1U);
  Add(1, 2U, "151", "200", 1U);
  Add(1, 3U, "201", "300", 1000000000U);
  Add(1, 4U, "301", "400", 1U);
  Add(1, 5U, "401", "500", 1U);
  Add(2, 6U, "150", "200", 1U);
  Add(2, 7U, "200", "450", 1U, 0, 0);
  Add(2, 8U, "500", "600", 1U);

  UpdateVersionStorageInfo();

  std::unique_ptr<Compaction> compaction(level_compaction_picker.PickCompaction(
      cf_name_, mutable_cf_options_, vstorage_.get(), &log_buffer_));
  ASSERT_TRUE(compaction.get() != nullptr);
  ASSERT_EQ(2U, compaction->num_input_levels());
  ASSERT_EQ(3U, compaction->num_input_files(0));
  ASSERT_EQ(2U, compaction->num_input_files(1));
  ASSERT_EQ(2U, compaction->input(0, 0)->fd.GetNumber());
  ASSERT_EQ(3U, compaction->input(0, 1)->fd.GetNumber());
  ASSERT_EQ(4U, compaction->input(0, 2)->fd.GetNumber());
  ASSERT_EQ(6U, compaction->input(1, 0)->fd.GetNumber());
  ASSERT_EQ(7U, compaction->input(1, 1)->fd.GetNumber());
}

TEST_F(CompactionPickerTest, OverlappingUserKeys9) {
  NewVersionStorage(6, kCompactionStyleLevel);
  mutable_cf_options_.max_compaction_bytes = 100000000000u;
  // grow the number of inputs in "level" without
  // changing the number of "level+1" files we pick up
  // Expand input level as much as possible
  // overlapping case
  Add(1, 1U, "121", "150", 1U);
  Add(1, 2U, "151", "200", 1U);
  Add(1, 3U, "201", "300", 1000000000U);
  Add(1, 4U, "301", "400", 1U);
  Add(1, 5U, "401", "500", 1U);
  Add(2, 6U, "100", "120", 1U);
  Add(2, 7U, "150", "200", 1U);
  Add(2, 8U, "200", "450", 1U, 0, 0);
  Add(2, 9U, "501", "600", 1U);

  UpdateVersionStorageInfo();

  std::unique_ptr<Compaction> compaction(level_compaction_picker.PickCompaction(
      cf_name_, mutable_cf_options_, vstorage_.get(), &log_buffer_));
  ASSERT_TRUE(compaction.get() != nullptr);
  ASSERT_EQ(2U, compaction->num_input_levels());
  ASSERT_EQ(5U, compaction->num_input_files(0));
  ASSERT_EQ(2U, compaction->num_input_files(1));
  ASSERT_EQ(1U, compaction->input(0, 0)->fd.GetNumber());
  ASSERT_EQ(2U, compaction->input(0, 1)->fd.GetNumber());
  ASSERT_EQ(3U, compaction->input(0, 2)->fd.GetNumber());
  ASSERT_EQ(4U, compaction->input(0, 3)->fd.GetNumber());
  ASSERT_EQ(7U, compaction->input(1, 0)->fd.GetNumber());
  ASSERT_EQ(8U, compaction->input(1, 1)->fd.GetNumber());
}

TEST_F(CompactionPickerTest, OverlappingUserKeys10) {
  // Locked file encountered when pulling in extra input-level files with same
  // user keys. Verify we pick the next-best file from the same input level.
  NewVersionStorage(6, kCompactionStyleLevel);
  mutable_cf_options_.max_compaction_bytes = 100000000000u;

  // file_number 2U is largest and thus first choice. But it overlaps with
  // file_number 1U which is being compacted. So instead we pick the next-
  // biggest file, 3U, which is eligible for compaction.
  Add(1 /* level */, 1U /* file_number */, "100" /* smallest */,
      "150" /* largest */, 1U /* file_size */);
  file_map_[1U].first->being_compacted = true;
  Add(1 /* level */, 2U /* file_number */, "150" /* smallest */,
      "200" /* largest */, 1000000000U /* file_size */, 0 /* smallest_seq */,
      0 /* largest_seq */);
  Add(1 /* level */, 3U /* file_number */, "201" /* smallest */,
      "250" /* largest */, 900000000U /* file_size */);
  Add(2 /* level */, 4U /* file_number */, "100" /* smallest */,
      "150" /* largest */, 1U /* file_size */);
  Add(2 /* level */, 5U /* file_number */, "151" /* smallest */,
      "200" /* largest */, 1U /* file_size */);
  Add(2 /* level */, 6U /* file_number */, "201" /* smallest */,
      "250" /* largest */, 1U /* file_size */);

  UpdateVersionStorageInfo();

  std::unique_ptr<Compaction> compaction(level_compaction_picker.PickCompaction(
      cf_name_, mutable_cf_options_, vstorage_.get(), &log_buffer_));
  ASSERT_TRUE(compaction.get() != nullptr);
  ASSERT_EQ(2U, compaction->num_input_levels());
  ASSERT_EQ(1U, compaction->num_input_files(0));
  ASSERT_EQ(1U, compaction->num_input_files(1));
  ASSERT_EQ(3U, compaction->input(0, 0)->fd.GetNumber());
  ASSERT_EQ(6U, compaction->input(1, 0)->fd.GetNumber());
}

TEST_F(CompactionPickerTest, OverlappingUserKeys11) {
  // Locked file encountered when pulling in extra output-level files with same
  // user keys. Expected to skip that compaction and pick the next-best choice.
  NewVersionStorage(6, kCompactionStyleLevel);
  mutable_cf_options_.max_compaction_bytes = 100000000000u;

  // score(L1) = 3.7
  // score(L2) = 1.85
  // There is no eligible file in L1 to compact since both candidates pull in
  // file_number 5U, which overlaps with a file pending compaction (6U). The
  // first eligible compaction is from L2->L3.
  Add(1 /* level */, 2U /* file_number */, "151" /* smallest */,
      "200" /* largest */, 1000000000U /* file_size */);
  Add(1 /* level */, 3U /* file_number */, "201" /* smallest */,
      "250" /* largest */, 1U /* file_size */);
  Add(2 /* level */, 4U /* file_number */, "100" /* smallest */,
      "149" /* largest */, 5000000000U /* file_size */);
  Add(2 /* level */, 5U /* file_number */, "150" /* smallest */,
      "201" /* largest */, 1U /* file_size */);
  Add(2 /* level */, 6U /* file_number */, "201" /* smallest */,
      "249" /* largest */, 1U /* file_size */, 0 /* smallest_seq */,
      0 /* largest_seq */);
  file_map_[6U].first->being_compacted = true;
  Add(3 /* level */, 7U /* file_number */, "100" /* smallest */,
      "149" /* largest */, 1U /* file_size */);

  UpdateVersionStorageInfo();

  std::unique_ptr<Compaction> compaction(level_compaction_picker.PickCompaction(
      cf_name_, mutable_cf_options_, vstorage_.get(), &log_buffer_));
  ASSERT_TRUE(compaction.get() != nullptr);
  ASSERT_EQ(2U, compaction->num_input_levels());
  ASSERT_EQ(1U, compaction->num_input_files(0));
  ASSERT_EQ(1U, compaction->num_input_files(1));
  ASSERT_EQ(4U, compaction->input(0, 0)->fd.GetNumber());
  ASSERT_EQ(7U, compaction->input(1, 0)->fd.GetNumber());
}

TEST_F(CompactionPickerTest, NotScheduleL1IfL0WithHigherPri1) {
  NewVersionStorage(6, kCompactionStyleLevel);
  mutable_cf_options_.level0_file_num_compaction_trigger = 2;
  mutable_cf_options_.max_bytes_for_level_base = 900000000U;

  // 6 L0 files, score 3.
  Add(0, 1U, "000", "400", 1U);
  Add(0, 2U, "001", "400", 1U, 0, 0);
  Add(0, 3U, "001", "400", 1000000000U, 0, 0);
  Add(0, 31U, "001", "400", 1000000000U, 0, 0);
  Add(0, 32U, "001", "400", 1000000000U, 0, 0);
  Add(0, 33U, "001", "400", 1000000000U, 0, 0);

  // L1 total size 2GB, score 2.2. If one file being comapcted, score 1.1.
  Add(1, 4U, "050", "300", 1000000000U, 0, 0);
  file_map_[4u].first->being_compacted = true;
  Add(1, 5U, "301", "350", 1000000000U, 0, 0);

  // Output level overlaps with the beginning and the end of the chain
  Add(2, 6U, "050", "100", 1U);
  Add(2, 7U, "300", "400", 1U);

  // No compaction should be scheduled, if L0 has higher priority than L1
  // but L0->L1 compaction is blocked by a file in L1 being compacted.
  UpdateVersionStorageInfo();
  ASSERT_EQ(0, vstorage_->CompactionScoreLevel(0));
  ASSERT_EQ(1, vstorage_->CompactionScoreLevel(1));
  std::unique_ptr<Compaction> compaction(level_compaction_picker.PickCompaction(
      cf_name_, mutable_cf_options_, vstorage_.get(), &log_buffer_));
  ASSERT_TRUE(compaction.get() == nullptr);
}

TEST_F(CompactionPickerTest, NotScheduleL1IfL0WithHigherPri2) {
  NewVersionStorage(6, kCompactionStyleLevel);
  mutable_cf_options_.level0_file_num_compaction_trigger = 2;
  mutable_cf_options_.max_bytes_for_level_base = 900000000U;

  // 6 L0 files, score 3.
  Add(0, 1U, "000", "400", 1U);
  Add(0, 2U, "001", "400", 1U, 0, 0);
  Add(0, 3U, "001", "400", 1000000000U, 0, 0);
  Add(0, 31U, "001", "400", 1000000000U, 0, 0);
  Add(0, 32U, "001", "400", 1000000000U, 0, 0);
  Add(0, 33U, "001", "400", 1000000000U, 0, 0);

  // L1 total size 2GB, score 2.2. If one file being comapcted, score 1.1.
  Add(1, 4U, "050", "300", 1000000000U, 0, 0);
  Add(1, 5U, "301", "350", 1000000000U, 0, 0);

  // Output level overlaps with the beginning and the end of the chain
  Add(2, 6U, "050", "100", 1U);
  Add(2, 7U, "300", "400", 1U);

  // If no file in L1 being compacted, L0->L1 compaction will be scheduled.
  UpdateVersionStorageInfo();  // being_compacted flag is cleared here.
  ASSERT_EQ(0, vstorage_->CompactionScoreLevel(0));
  ASSERT_EQ(1, vstorage_->CompactionScoreLevel(1));
  std::unique_ptr<Compaction> compaction(level_compaction_picker.PickCompaction(
      cf_name_, mutable_cf_options_, vstorage_.get(), &log_buffer_));
  ASSERT_TRUE(compaction.get() != nullptr);
}

TEST_F(CompactionPickerTest, NotScheduleL1IfL0WithHigherPri3) {
  NewVersionStorage(6, kCompactionStyleLevel);
  mutable_cf_options_.level0_file_num_compaction_trigger = 2;
  mutable_cf_options_.max_bytes_for_level_base = 900000000U;

  // 6 L0 files, score 3.
  Add(0, 1U, "000", "400", 1U);
  Add(0, 2U, "001", "400", 1U, 0, 0);
  Add(0, 3U, "001", "400", 1000000000U, 0, 0);
  Add(0, 31U, "001", "400", 1000000000U, 0, 0);
  Add(0, 32U, "001", "400", 1000000000U, 0, 0);
  Add(0, 33U, "001", "400", 1000000000U, 0, 0);

  // L1 score more than 6.
  Add(1, 4U, "050", "300", 1000000000U, 0, 0);
  file_map_[4u].first->being_compacted = true;
  Add(1, 5U, "301", "350", 1000000000U, 0, 0);
  Add(1, 51U, "351", "400", 6000000000U, 0, 0);

  // Output level overlaps with the beginning and the end of the chain
  Add(2, 6U, "050", "100", 1U);
  Add(2, 7U, "300", "400", 1U);

  // If score in L1 is larger than L0, L1 compaction goes through despite
  // there is pending L0 compaction.
  UpdateVersionStorageInfo();
  ASSERT_EQ(1, vstorage_->CompactionScoreLevel(0));
  ASSERT_EQ(0, vstorage_->CompactionScoreLevel(1));
  std::unique_ptr<Compaction> compaction(level_compaction_picker.PickCompaction(
      cf_name_, mutable_cf_options_, vstorage_.get(), &log_buffer_));
  ASSERT_TRUE(compaction.get() != nullptr);
}

TEST_F(CompactionPickerTest, EstimateCompactionBytesNeeded1) {
  int num_levels = ioptions_.num_levels;
  ioptions_.level_compaction_dynamic_level_bytes = false;
  mutable_cf_options_.level0_file_num_compaction_trigger = 4;
  mutable_cf_options_.max_bytes_for_level_base = 1000;
  mutable_cf_options_.max_bytes_for_level_multiplier = 10;
  NewVersionStorage(num_levels, kCompactionStyleLevel);
  Add(0, 1U, "150", "200", 200);
  Add(0, 2U, "150", "200", 200);
  Add(0, 3U, "150", "200", 200);
  // Level 1 is over target by 200
  Add(1, 4U, "400", "500", 600);
  Add(1, 5U, "600", "700", 600);
  // Level 2 is less than target 10000 even added size of level 1
  // Size ratio of L2/L1 is 9600 / 1200 = 8
  Add(2, 6U, "150", "200", 2500);
  Add(2, 7U, "201", "210", 2000);
  Add(2, 8U, "300", "310", 2600);
  Add(2, 9U, "400", "500", 2500);
  // Level 3 exceeds target 100,000 of 1000
  Add(3, 10U, "400", "500", 101000);
  // Level 4 exceeds target 1,000,000 by 900 after adding size from level 3
  // Size ratio L4/L3 is 9.9
  // After merge from L3, L4 size is 1000900
  Add(4, 11U, "400", "500", 999900);
  Add(5, 11U, "400", "500", 8007200);

  UpdateVersionStorageInfo();

  ASSERT_EQ(200u * 9u + 10900u + 900u * 9,
            vstorage_->estimated_compaction_needed_bytes());
}

TEST_F(CompactionPickerTest, EstimateCompactionBytesNeeded2) {
  int num_levels = ioptions_.num_levels;
  ioptions_.level_compaction_dynamic_level_bytes = false;
  mutable_cf_options_.level0_file_num_compaction_trigger = 3;
  mutable_cf_options_.max_bytes_for_level_base = 1000;
  mutable_cf_options_.max_bytes_for_level_multiplier = 10;
  NewVersionStorage(num_levels, kCompactionStyleLevel);
  Add(0, 1U, "150", "200", 200);
  Add(0, 2U, "150", "200", 200);
  Add(0, 4U, "150", "200", 200);
  Add(0, 5U, "150", "200", 200);
  Add(0, 6U, "150", "200", 200);
  // Level 1 size will be 1400 after merging with L0
  Add(1, 7U, "400", "500", 200);
  Add(1, 8U, "600", "700", 200);
  // Level 2 is less than target 10000 even added size of level 1
  Add(2, 9U, "150", "200", 9100);
  // Level 3 over the target, but since level 4 is empty, we assume it will be
  // a trivial move.
  Add(3, 10U, "400", "500", 101000);

  UpdateVersionStorageInfo();

  // estimated L1->L2 merge: 400 * (9100.0 / 1400.0 + 1.0)
  ASSERT_EQ(1400u + 3000u, vstorage_->estimated_compaction_needed_bytes());
}

TEST_F(CompactionPickerTest, EstimateCompactionBytesNeeded3) {
  int num_levels = ioptions_.num_levels;
  ioptions_.level_compaction_dynamic_level_bytes = false;
  mutable_cf_options_.level0_file_num_compaction_trigger = 3;
  mutable_cf_options_.max_bytes_for_level_base = 1000;
  mutable_cf_options_.max_bytes_for_level_multiplier = 10;
  NewVersionStorage(num_levels, kCompactionStyleLevel);
  Add(0, 1U, "150", "200", 2000);
  Add(0, 2U, "150", "200", 2000);
  Add(0, 4U, "150", "200", 2000);
  Add(0, 5U, "150", "200", 2000);
  Add(0, 6U, "150", "200", 1000);
  // Level 1 size will be 10000 after merging with L0
  Add(1, 7U, "400", "500", 500);
  Add(1, 8U, "600", "700", 500);

  Add(2, 9U, "150", "200", 10000);

  UpdateVersionStorageInfo();

  ASSERT_EQ(10000u + 18000u, vstorage_->estimated_compaction_needed_bytes());
}

TEST_F(CompactionPickerTest, EstimateCompactionBytesNeededDynamicLevel) {
  int num_levels = ioptions_.num_levels;
  ioptions_.level_compaction_dynamic_level_bytes = true;
  mutable_cf_options_.level0_file_num_compaction_trigger = 3;
  mutable_cf_options_.max_bytes_for_level_base = 1000;
  mutable_cf_options_.max_bytes_for_level_multiplier = 10;
  NewVersionStorage(num_levels, kCompactionStyleLevel);

  // Set Last level size 50000
  // num_levels - 1 target 5000
  // num_levels - 2 is base level with target 1000 (rounded up to
  // max_bytes_for_level_base).
  Add(num_levels - 1, 10U, "400", "500", 50000);

  Add(0, 1U, "150", "200", 200);
  Add(0, 2U, "150", "200", 200);
  Add(0, 4U, "150", "200", 200);
  Add(0, 5U, "150", "200", 200);
  Add(0, 6U, "150", "200", 200);
  // num_levels - 3 is over target by 100 + 1000
  Add(num_levels - 3, 7U, "400", "500", 550);
  Add(num_levels - 3, 8U, "600", "700", 550);
  // num_levels - 2 is over target by 1100 + 200
  Add(num_levels - 2, 9U, "150", "200", 5200);

  UpdateVersionStorageInfo();

  // Merging to the second last level: (5200 / 2100 + 1) * 1100
  // Merging to the last level: (50000 / 6300 + 1) * 1300
  ASSERT_EQ(2100u + 3823u + 11617u,
            vstorage_->estimated_compaction_needed_bytes());
}

TEST_F(CompactionPickerTest, IsBottommostLevelTest) {
  // case 1: Higher levels are empty
  NewVersionStorage(6, kCompactionStyleLevel);
  Add(0, 1U, "a", "m");
  Add(0, 2U, "c", "z");
  Add(1, 3U, "d", "e");
  Add(1, 4U, "l", "p");
  Add(2, 5U, "g", "i");
  Add(2, 6U, "x", "z");
  UpdateVersionStorageInfo();
  SetCompactionInputFilesLevels(2, 1);
  AddToCompactionFiles(3U);
  AddToCompactionFiles(5U);
  bool result =
      Compaction::TEST_IsBottommostLevel(2, vstorage_.get(), input_files_);
  ASSERT_TRUE(result);

  // case 2: Higher levels have no overlap
  NewVersionStorage(6, kCompactionStyleLevel);
  Add(0, 1U, "a", "m");
  Add(0, 2U, "c", "z");
  Add(1, 3U, "d", "e");
  Add(1, 4U, "l", "p");
  Add(2, 5U, "g", "i");
  Add(2, 6U, "x", "z");
  Add(3, 7U, "k", "p");
  Add(3, 8U, "t", "w");
  Add(4, 9U, "a", "b");
  Add(5, 10U, "c", "cc");
  UpdateVersionStorageInfo();
  SetCompactionInputFilesLevels(2, 1);
  AddToCompactionFiles(3U);
  AddToCompactionFiles(5U);
  result = Compaction::TEST_IsBottommostLevel(2, vstorage_.get(), input_files_);
  ASSERT_TRUE(result);

  // case 3.1: Higher levels (level 3) have overlap
  NewVersionStorage(6, kCompactionStyleLevel);
  Add(0, 1U, "a", "m");
  Add(0, 2U, "c", "z");
  Add(1, 3U, "d", "e");
  Add(1, 4U, "l", "p");
  Add(2, 5U, "g", "i");
  Add(2, 6U, "x", "z");
  Add(3, 7U, "e", "g");
  Add(3, 8U, "h", "k");
  Add(4, 9U, "a", "b");
  Add(5, 10U, "c", "cc");
  UpdateVersionStorageInfo();
  SetCompactionInputFilesLevels(2, 1);
  AddToCompactionFiles(3U);
  AddToCompactionFiles(5U);
  result = Compaction::TEST_IsBottommostLevel(2, vstorage_.get(), input_files_);
  ASSERT_FALSE(result);

  // case 3.2: Higher levels (level 5) have overlap
  DeleteVersionStorage();
  NewVersionStorage(6, kCompactionStyleLevel);
  Add(0, 1U, "a", "m");
  Add(0, 2U, "c", "z");
  Add(1, 3U, "d", "e");
  Add(1, 4U, "l", "p");
  Add(2, 5U, "g", "i");
  Add(2, 6U, "x", "z");
  Add(3, 7U, "j", "k");
  Add(3, 8U, "l", "m");
  Add(4, 9U, "a", "b");
  Add(5, 10U, "c", "cc");
  Add(5, 11U, "h", "k");
  Add(5, 12U, "y", "yy");
  Add(5, 13U, "z", "zz");
  UpdateVersionStorageInfo();
  SetCompactionInputFilesLevels(2, 1);
  AddToCompactionFiles(3U);
  AddToCompactionFiles(5U);
  result = Compaction::TEST_IsBottommostLevel(2, vstorage_.get(), input_files_);
  ASSERT_FALSE(result);

  // case 3.3: Higher levels (level 5) have overlap, but it's only overlapping
  // one key ("d")
  NewVersionStorage(6, kCompactionStyleLevel);
  Add(0, 1U, "a", "m");
  Add(0, 2U, "c", "z");
  Add(1, 3U, "d", "e");
  Add(1, 4U, "l", "p");
  Add(2, 5U, "g", "i");
  Add(2, 6U, "x", "z");
  Add(3, 7U, "j", "k");
  Add(3, 8U, "l", "m");
  Add(4, 9U, "a", "b");
  Add(5, 10U, "c", "cc");
  Add(5, 11U, "ccc", "d");
  Add(5, 12U, "y", "yy");
  Add(5, 13U, "z", "zz");
  UpdateVersionStorageInfo();
  SetCompactionInputFilesLevels(2, 1);
  AddToCompactionFiles(3U);
  AddToCompactionFiles(5U);
  result = Compaction::TEST_IsBottommostLevel(2, vstorage_.get(), input_files_);
  ASSERT_FALSE(result);

  // Level 0 files overlap
  NewVersionStorage(6, kCompactionStyleLevel);
  Add(0, 1U, "s", "t");
  Add(0, 2U, "a", "m");
  Add(0, 3U, "b", "z");
  Add(0, 4U, "e", "f");
  Add(5, 10U, "y", "z");
  UpdateVersionStorageInfo();
  SetCompactionInputFilesLevels(1, 0);
  AddToCompactionFiles(1U);
  AddToCompactionFiles(2U);
  AddToCompactionFiles(3U);
  AddToCompactionFiles(4U);
  result = Compaction::TEST_IsBottommostLevel(2, vstorage_.get(), input_files_);
  ASSERT_FALSE(result);

  // Level 0 files don't overlap
  NewVersionStorage(6, kCompactionStyleLevel);
  Add(0, 1U, "s", "t");
  Add(0, 2U, "a", "m");
  Add(0, 3U, "b", "k");
  Add(0, 4U, "e", "f");
  Add(5, 10U, "y", "z");
  UpdateVersionStorageInfo();
  SetCompactionInputFilesLevels(1, 0);
  AddToCompactionFiles(1U);
  AddToCompactionFiles(2U);
  AddToCompactionFiles(3U);
  AddToCompactionFiles(4U);
  result = Compaction::TEST_IsBottommostLevel(2, vstorage_.get(), input_files_);
  ASSERT_TRUE(result);

  // Level 1 files overlap
  NewVersionStorage(6, kCompactionStyleLevel);
  Add(0, 1U, "s", "t");
  Add(0, 2U, "a", "m");
  Add(0, 3U, "b", "k");
  Add(0, 4U, "e", "f");
  Add(1, 5U, "a", "m");
  Add(1, 6U, "n", "o");
  Add(1, 7U, "w", "y");
  Add(5, 10U, "y", "z");
  UpdateVersionStorageInfo();
  SetCompactionInputFilesLevels(2, 0);
  AddToCompactionFiles(1U);
  AddToCompactionFiles(2U);
  AddToCompactionFiles(3U);
  AddToCompactionFiles(4U);
  AddToCompactionFiles(5U);
  AddToCompactionFiles(6U);
  AddToCompactionFiles(7U);
  result = Compaction::TEST_IsBottommostLevel(2, vstorage_.get(), input_files_);
  ASSERT_FALSE(result);

  DeleteVersionStorage();
}

TEST_F(CompactionPickerTest, MaxCompactionBytesHit) {
  mutable_cf_options_.max_bytes_for_level_base = 1000000u;
  mutable_cf_options_.max_compaction_bytes = 800000u;
  ioptions_.level_compaction_dynamic_level_bytes = false;
  NewVersionStorage(6, kCompactionStyleLevel);
  // A compaction should be triggered and pick file 2 and 5.
  // It can expand because adding file 1 and 3, the compaction size will
  // exceed mutable_cf_options_.max_bytes_for_level_base.
  Add(1, 1U, "100", "150", 300000U);
  Add(1, 2U, "151", "200", 300001U, 0, 0);
  Add(1, 3U, "201", "250", 300000U, 0, 0);
  Add(1, 4U, "251", "300", 300000U, 0, 0);
  Add(2, 5U, "100", "256", 1U);
  UpdateVersionStorageInfo();

  std::unique_ptr<Compaction> compaction(level_compaction_picker.PickCompaction(
      cf_name_, mutable_cf_options_, vstorage_.get(), &log_buffer_));
  ASSERT_TRUE(compaction.get() != nullptr);
  ASSERT_EQ(2U, compaction->num_input_levels());
  ASSERT_EQ(1U, compaction->num_input_files(0));
  ASSERT_EQ(1U, compaction->num_input_files(1));
  ASSERT_EQ(2U, compaction->input(0, 0)->fd.GetNumber());
  ASSERT_EQ(5U, compaction->input(1, 0)->fd.GetNumber());
}

TEST_F(CompactionPickerTest, MaxCompactionBytesNotHit) {
  mutable_cf_options_.max_bytes_for_level_base = 800000u;
  mutable_cf_options_.max_compaction_bytes = 1000000u;
  ioptions_.level_compaction_dynamic_level_bytes = false;
  NewVersionStorage(6, kCompactionStyleLevel);
  // A compaction should be triggered and pick file 2 and 5.
  // and it expands to file 1 and 3 too.
  Add(1, 1U, "100", "150", 300000U);
  Add(1, 2U, "151", "200", 300001U, 0, 0);
  Add(1, 3U, "201", "250", 300000U, 0, 0);
  Add(1, 4U, "251", "300", 300000U, 0, 0);
  Add(2, 5U, "000", "251", 1U);
  UpdateVersionStorageInfo();

  std::unique_ptr<Compaction> compaction(level_compaction_picker.PickCompaction(
      cf_name_, mutable_cf_options_, vstorage_.get(), &log_buffer_));
  ASSERT_TRUE(compaction.get() != nullptr);
  ASSERT_EQ(2U, compaction->num_input_levels());
  ASSERT_EQ(3U, compaction->num_input_files(0));
  ASSERT_EQ(1U, compaction->num_input_files(1));
  ASSERT_EQ(1U, compaction->input(0, 0)->fd.GetNumber());
  ASSERT_EQ(2U, compaction->input(0, 1)->fd.GetNumber());
  ASSERT_EQ(3U, compaction->input(0, 2)->fd.GetNumber());
  ASSERT_EQ(5U, compaction->input(1, 0)->fd.GetNumber());
}

TEST_F(CompactionPickerTest, IsTrivialMoveOn) {
  mutable_cf_options_.max_bytes_for_level_base = 10000u;
  mutable_cf_options_.max_compaction_bytes = 10001u;
  ioptions_.level_compaction_dynamic_level_bytes = false;
  NewVersionStorage(6, kCompactionStyleLevel);
  // A compaction should be triggered and pick file 2
  Add(1, 1U, "100", "150", 3000U);
  Add(1, 2U, "151", "200", 3001U);
  Add(1, 3U, "201", "250", 3000U);
  Add(1, 4U, "251", "300", 3000U);

  Add(3, 5U, "120", "130", 7000U);
  Add(3, 6U, "170", "180", 7000U);
  Add(3, 5U, "220", "230", 7000U);
  Add(3, 5U, "270", "280", 7000U);
  UpdateVersionStorageInfo();

  std::unique_ptr<Compaction> compaction(level_compaction_picker.PickCompaction(
    cf_name_, mutable_cf_options_, vstorage_.get(), &log_buffer_));
  ASSERT_TRUE(compaction.get() != nullptr);
  ASSERT_TRUE(compaction->IsTrivialMove());
}

TEST_F(CompactionPickerTest, IsTrivialMoveOff) {
  mutable_cf_options_.max_bytes_for_level_base = 1000000u;
  mutable_cf_options_.max_compaction_bytes = 10000u;
  ioptions_.level_compaction_dynamic_level_bytes = false;
  NewVersionStorage(6, kCompactionStyleLevel);
  // A compaction should be triggered and pick all files from level 1
  Add(1, 1U, "100", "150", 300000U, 0, 0);
  Add(1, 2U, "150", "200", 300000U, 0, 0);
  Add(1, 3U, "200", "250", 300000U, 0, 0);
  Add(1, 4U, "250", "300", 300000U, 0, 0);

  Add(3, 5U, "120", "130", 6000U);
  Add(3, 6U, "140", "150", 6000U);
  UpdateVersionStorageInfo();

  std::unique_ptr<Compaction> compaction(level_compaction_picker.PickCompaction(
    cf_name_, mutable_cf_options_, vstorage_.get(), &log_buffer_));
  ASSERT_TRUE(compaction.get() != nullptr);
  ASSERT_FALSE(compaction->IsTrivialMove());
}

TEST_F(CompactionPickerTest, CacheNextCompactionIndex) {
  NewVersionStorage(6, kCompactionStyleLevel);
  mutable_cf_options_.max_compaction_bytes = 100000000000u;

  Add(1 /* level */, 1U /* file_number */, "100" /* smallest */,
      "149" /* largest */, 1000000000U /* file_size */);
  file_map_[1U].first->being_compacted = true;
  Add(1 /* level */, 2U /* file_number */, "150" /* smallest */,
      "199" /* largest */, 900000000U /* file_size */);
  Add(1 /* level */, 3U /* file_number */, "200" /* smallest */,
      "249" /* largest */, 800000000U /* file_size */);
  Add(1 /* level */, 4U /* file_number */, "250" /* smallest */,
      "299" /* largest */, 700000000U /* file_size */);
  Add(2 /* level */, 5U /* file_number */, "150" /* smallest */,
      "199" /* largest */, 1U /* file_size */);
  file_map_[5U].first->being_compacted = true;

  UpdateVersionStorageInfo();

  std::unique_ptr<Compaction> compaction(level_compaction_picker.PickCompaction(
      cf_name_, mutable_cf_options_, vstorage_.get(), &log_buffer_));
  ASSERT_TRUE(compaction.get() != nullptr);
  ASSERT_EQ(1U, compaction->num_input_levels());
  ASSERT_EQ(1U, compaction->num_input_files(0));
  ASSERT_EQ(0U, compaction->num_input_files(1));
  ASSERT_EQ(3U, compaction->input(0, 0)->fd.GetNumber());
  ASSERT_EQ(2, vstorage_->NextCompactionIndex(1 /* level */));

  compaction.reset(level_compaction_picker.PickCompaction(
      cf_name_, mutable_cf_options_, vstorage_.get(), &log_buffer_));
  ASSERT_TRUE(compaction.get() != nullptr);
  ASSERT_EQ(1U, compaction->num_input_levels());
  ASSERT_EQ(1U, compaction->num_input_files(0));
  ASSERT_EQ(0U, compaction->num_input_files(1));
  ASSERT_EQ(4U, compaction->input(0, 0)->fd.GetNumber());
  ASSERT_EQ(3, vstorage_->NextCompactionIndex(1 /* level */));

  compaction.reset(level_compaction_picker.PickCompaction(
      cf_name_, mutable_cf_options_, vstorage_.get(), &log_buffer_));
  ASSERT_TRUE(compaction.get() == nullptr);
  ASSERT_EQ(4, vstorage_->NextCompactionIndex(1 /* level */));
}

// Tests for level0_max_compaction_file_number
//
// Sequence number conventions used throughout these tests:
//   L0 files: realistic per-flush ranges (smallest_seq = largest_seq - 19),
//             so consecutive flushes have non-overlapping seqno ranges.
//             e.g. file5: seq=[481,500], file4: seq=[381,400], etc.
//   Ln files: small stable seqnos (smallest_seq=2, largest_seq=15) to
//             reflect old compacted data.

TEST_F(CompactionPickerTest, L0MaxCompactionFileNumberZeroNoLimit) {
  // level0_max_compaction_file_number = 0 means no limit (original behavior).
  NewVersionStorage(6, kCompactionStyleLevel);
  mutable_cf_options_.level0_file_num_compaction_trigger = 2;
  mutable_cf_options_.level0_max_compaction_file_number = 0;

  // 5 L0 files with overlapping ranges. Each flush covers a 20-seqno window.
  Add(0, 5U, "a", "z", 1U, 0, 481, 500);  // newest flush
  Add(0, 4U, "a", "z", 1U, 0, 381, 400);
  Add(0, 3U, "a", "z", 1U, 0, 281, 300);
  Add(0, 2U, "a", "z", 1U, 0, 181, 200);
  Add(0, 1U, "a", "z", 1U, 0, 81, 100);   // oldest flush
  UpdateVersionStorageInfo();

  std::unique_ptr<Compaction> compaction(level_compaction_picker.PickCompaction(
      cf_name_, mutable_cf_options_, vstorage_.get(), &log_buffer_));
  ASSERT_TRUE(compaction.get() != nullptr);
  // All 5 files should be picked since there's no limit.
  ASSERT_EQ(5U, compaction->num_input_files(0));
}

TEST_F(CompactionPickerTest, L0MaxCompactionFileNumberLimitOverlapping) {
  // Limit to 3 files, L0 has 5 files in two disjoint groups:
  //   group A = {1,2,3}: ranges [e,j],[c,h],[a,f] (overlap each other)
  //   group B = {4,5}: ranges [v,y],[x,z] (disjoint from group A)
  // A seed from group A gives a closure of exactly 3 files == limit -> no
  // truncation. A seed from group B gives 2 files < limit, also no truncation.
  //
  // Seed selection uses FilesByCompactionPri[0] ordered by compensated_file_size
  // (kByCompensatedSize, largest first). Group A files have file_size=100U,
  // group B file_size=1U, so a group-A file is always chosen as seed.
  //
  // NOTE: L0 has two independent orderings:
  //   1. files_[0] (NewestFirstBySeqNo): for read queries and truncation.
  //   2. FilesByCompactionPri[0] (kByCompensatedSize): for seed selection only.
  //
  // files_[0] newest-first by largest_seqno:
  //   file5: [x,z] seq=[481,500], size=1U,   group B (newest)
  //   file4: [v,y] seq=[381,400], size=1U,   group B
  //   file3: [a,f] seq=[281,300], size=100U, group A
  //   file2: [c,h] seq=[181,200], size=100U, group A
  //   file1: [e,j] seq=[81, 100], size=100U, group A (oldest)
  NewVersionStorage(6, kCompactionStyleLevel);
  mutable_cf_options_.level0_file_num_compaction_trigger = 2;
  mutable_cf_options_.level0_max_compaction_file_number = 3;

  Add(0, 5U, "x", "z", 1U, 0, 481, 500);
  Add(0, 4U, "v", "y", 1U, 0, 381, 400);
  Add(0, 3U, "a", "f", 100U, 0, 281, 300);
  Add(0, 2U, "c", "h", 100U, 0, 181, 200);
  Add(0, 1U, "e", "j", 100U, 0, 81, 100);
  UpdateVersionStorageInfo();

  std::unique_ptr<Compaction> compaction(level_compaction_picker.PickCompaction(
      cf_name_, mutable_cf_options_, vstorage_.get(), &log_buffer_));
  ASSERT_TRUE(compaction.get() != nullptr);
  // Seed from group A -> closure = {1,2,3} (3 files == limit). No truncation.
  ASSERT_EQ(3U, compaction->num_input_files(0));
}

TEST_F(CompactionPickerTest, L0MaxCompactionFileNumberOverlapClosure) {
  // Limit=2, chain-overlapping L0 files: each adjacent pair overlaps but
  // non-adjacent pairs may not. GetOverlappingL0Files returns all 4 via
  // chain expansion. Truncation keeps only the oldest 2.
  //
  // files_[0] newest-first by largest_seqno:
  //   file4: [a,z] seq=[381,400] (newest) - overlaps all
  //   file3: [m,z] seq=[281,300]           - overlaps file4 and file2 via [m,n]
  //   file2: [a,n] seq=[181,200]           - overlaps file3 and file1
  //   file1: [a,f] seq=[81, 100] (oldest)  - overlaps file2
  //
  // After truncation, oldest 2 kept: file1 [a,f] and file2 [a,n].
  // Files 3 and 4 stay in L0 and shadow the compacted data (correct).
  NewVersionStorage(6, kCompactionStyleLevel);
  mutable_cf_options_.level0_file_num_compaction_trigger = 2;
  mutable_cf_options_.level0_max_compaction_file_number = 2;

  Add(0, 4U, "a", "z", 1U, 0, 381, 400);
  Add(0, 3U, "m", "z", 1U, 0, 281, 300);
  Add(0, 2U, "a", "n", 1U, 0, 181, 200);
  Add(0, 1U, "a", "f", 1U, 0, 81, 100);
  UpdateVersionStorageInfo();

  std::unique_ptr<Compaction> compaction(level_compaction_picker.PickCompaction(
      cf_name_, mutable_cf_options_, vstorage_.get(), &log_buffer_));
  ASSERT_TRUE(compaction.get() != nullptr);
  // Oldest 2 files selected; limit enforced over full overlap closure.
  ASSERT_EQ(2U, compaction->num_input_files(0));
}

TEST_F(CompactionPickerTest, L0MaxCompactionFileNumberBelowLimit) {
  // 3 L0 files, limit=10: closure(3) < limit, so no truncation — all 3 kept.
  NewVersionStorage(6, kCompactionStyleLevel);
  mutable_cf_options_.level0_file_num_compaction_trigger = 2;
  mutable_cf_options_.level0_max_compaction_file_number = 10;

  Add(0, 3U, "a", "z", 1U, 0, 281, 300);
  Add(0, 2U, "a", "z", 1U, 0, 181, 200);
  Add(0, 1U, "a", "z", 1U, 0, 81, 100);
  UpdateVersionStorageInfo();

  std::unique_ptr<Compaction> compaction(level_compaction_picker.PickCompaction(
      cf_name_, mutable_cf_options_, vstorage_.get(), &log_buffer_));
  ASSERT_TRUE(compaction.get() != nullptr);
  ASSERT_EQ(3U, compaction->num_input_files(0));
}

TEST_F(CompactionPickerTest, L0MaxCompactionFileNumberNoReExpansion) {
  // Verify SetupOtherInputs does not re-expand L0 after truncation.
  // L0: 5 files all [a,z], limit=3 → truncated to 3, range stays [a,z].
  // L1: 3 disjoint files [a,h],[i,q],[r,z].
  // Merged L0+L1 range = [a,z]; GetOverlappingInputs(L0,[a,z]) returns all 5.
  // l0_truncated guard blocks re-expansion to 5.
  // All 3 L1 files overlap the truncated [a,z] range and are included.
  NewVersionStorage(6, kCompactionStyleLevel);
  mutable_cf_options_.level0_file_num_compaction_trigger = 2;
  mutable_cf_options_.level0_max_compaction_file_number = 3;
  mutable_cf_options_.max_compaction_bytes = 100000;

  // 5 L0 files: each flush covers a 20-seqno window, non-overlapping ranges.
  Add(0, 5U, "a", "z", 1U, 0, 481, 500);  // newest flush, will be truncated
  Add(0, 4U, "a", "z", 1U, 0, 381, 400);  // will be truncated
  Add(0, 3U, "a", "z", 1U, 0, 281, 300);  // kept (oldest 3)
  Add(0, 2U, "a", "z", 1U, 0, 181, 200);  // kept
  Add(0, 1U, "a", "z", 1U, 0, 81, 100);   // kept

  // L1: 3 disjoint files covering the full [a,z] key space.
  Add(1, 6U, "a", "h", 500U, 0, 2, 15);
  Add(1, 7U, "i", "q", 500U, 0, 2, 15);
  Add(1, 8U, "r", "z", 500U, 0, 2, 15);

  UpdateVersionStorageInfo();

  std::unique_ptr<Compaction> compaction(level_compaction_picker.PickCompaction(
      cf_name_, mutable_cf_options_, vstorage_.get(), &log_buffer_));
  ASSERT_TRUE(compaction.get() != nullptr);
  // L0: 3 files (not re-expanded to 5 despite L1 merged range = [a,z])
  ASSERT_EQ(3U, compaction->num_input_files(0));
  // L1: all 3 files overlap with truncated L0 range [a,z]
  ASSERT_EQ(3U, compaction->num_input_files(1));
}

TEST_F(CompactionPickerTest, L0MaxCompactionNoReExpansionWithOutputRange) {
  // L0: 4 files all [a,z], limit=2 → truncated to 2, range stays [a,z].
  // L1: 2 disjoint files [a,m],[n,z].
  // Merged L0+L1 range = [a,z]; GetOverlappingInputs(L0,[a,z]) returns all 4.
  // l0_truncated guard blocks re-expansion to 4.
  // Both L1 files overlap with truncated [a,z] and are included.
  NewVersionStorage(6, kCompactionStyleLevel);
  mutable_cf_options_.level0_file_num_compaction_trigger = 2;
  mutable_cf_options_.level0_max_compaction_file_number = 2;
  mutable_cf_options_.max_compaction_bytes = 100000;

  Add(0, 4U, "a", "z", 1U, 0, 381, 400);  // newest, will be truncated
  Add(0, 3U, "a", "z", 1U, 0, 281, 300);  // will be truncated
  Add(0, 2U, "a", "z", 1U, 0, 181, 200);  // kept (oldest 2)
  Add(0, 1U, "a", "z", 1U, 0, 81, 100);   // kept

  // L1: 2 disjoint files covering [a,z].
  Add(1, 5U, "a", "m", 500U, 0, 2, 15);
  Add(1, 6U, "n", "z", 500U, 0, 2, 15);

  UpdateVersionStorageInfo();

  std::unique_ptr<Compaction> compaction(level_compaction_picker.PickCompaction(
      cf_name_, mutable_cf_options_, vstorage_.get(), &log_buffer_));
  ASSERT_TRUE(compaction.get() != nullptr);
  // L0: 2 files (not re-expanded to 4)
  ASSERT_EQ(2U, compaction->num_input_files(0));
  // L1: both files overlap with truncated [a,z]
  ASSERT_EQ(2U, compaction->num_input_files(1));
}

TEST_F(CompactionPickerTest, L0MaxCompactionNoReExpansionLnWiderRange) {
  // L0: 5 files, newest 2 cover [a,z], oldest 3 cover [a,d/e/f], limit=3.
  // After truncation, L0 keeps oldest 3 (range [a,f]).
  // L1: 3 disjoint files [a,f],[g,m],[n,z].
  // Merged L0+L1 range would be [a,z]; GetOverlappingInputs(L0,[a,z]) returns
  // all 5. l0_truncated guard blocks re-expansion.
  // Only L1[a,f] overlaps with the truncated L0 range [a,f]; the other two
  // L1 files are outside [a,f] and must NOT be included.
  NewVersionStorage(6, kCompactionStyleLevel);
  mutable_cf_options_.level0_file_num_compaction_trigger = 2;
  mutable_cf_options_.level0_max_compaction_file_number = 3;
  mutable_cf_options_.max_compaction_bytes = 100000;

  Add(0, 5U, "a", "z", 1U, 0, 481, 500);  // newest, will be truncated
  Add(0, 4U, "a", "z", 1U, 0, 381, 400);  // will be truncated
  Add(0, 3U, "a", "f", 1U, 0, 281, 300);  // kept (oldest 3)
  Add(0, 2U, "a", "e", 1U, 0, 181, 200);  // kept
  Add(0, 1U, "a", "d", 1U, 0, 81, 100);   // kept

  // L1: 3 disjoint files. Only [a,f] overlaps with truncated L0 range.
  Add(1, 6U, "a", "f", 500U, 0, 2, 15);
  Add(1, 7U, "g", "m", 500U, 0, 2, 15);  // outside truncated [a,f]
  Add(1, 8U, "n", "z", 500U, 0, 2, 15);  // outside truncated [a,f]

  UpdateVersionStorageInfo();

  std::unique_ptr<Compaction> compaction(level_compaction_picker.PickCompaction(
      cf_name_, mutable_cf_options_, vstorage_.get(), &log_buffer_));
  ASSERT_TRUE(compaction.get() != nullptr);
  // L0: 3 files (not re-expanded to 5)
  ASSERT_EQ(3U, compaction->num_input_files(0));
  // L1: only the file overlapping truncated range [a,f]; [g,m] and [n,z] excluded
  ASSERT_EQ(1U, compaction->num_input_files(1));
}

TEST_F(CompactionPickerTest, L0MaxCompactionFileNumberChainExpansionSort) {
  // Verify that SetupOtherL0FilesIfNeeded sorts files by seqno descending
  // before truncation, even when GetOverlappingInputs returns them in
  // non-newest-first order due to chain expansion.
  //
  // File layout (newest-first in files_[0]):
  //   file3: [p, z] seqno=300 (newest) - not directly overlapping seed range
  //   file2: [g, t] seqno=200          - overlaps file1 and file3 (chain link)
  //   file1: [a, m] seqno=100 (oldest) - seed (large file_size -> ranked 1st)
  //
  // file1 [a,m] does NOT directly overlap file3 [p,z] (m < p), so file3 is
  // pulled in via chain expansion through file2 [g,t]:
  //
  //   GetOverlappingInputs with seed=file1[a,m]:
  //     Pass 1 (scan newest-first): file3[p,z] -> p > m -> SKIP.
  //                                 file2[g,t] -> overlaps [a,m] -> add,
  //                                              expand range to [a,t].
  //                                 file1[a,m] -> add.
  //     Pass 2: file3[p,z] -> p < t -> add.
  //     Output: [file2, file1, file3]  <- NOT newest-first!
  //
  // Without sort before truncation (limit=2):
  //   erase front 1 -> keep [file1(100), file3(300)]
  //   BUG: newest file3 goes to Lbase; file2 stays in L0.
  //        Keys in [p,t] exist in both file2 (L0) and file3 (Lbase).
  //        L0 is queried first -> returns stale value from file2(seqno~200).
  //
  // With sort before truncation:
  //   sort -> [file3(300), file2(200), file1(100)]
  //   erase front 1 -> keep [file2(200), file1(100)] (oldest 2). Correct.
  //   file3 remains in L0 and is read first, shadowing the compacted data.
  NewVersionStorage(6, kCompactionStyleLevel);
  mutable_cf_options_.level0_file_num_compaction_trigger = 2;
  mutable_cf_options_.level0_max_compaction_file_number = 2;

  // file1 has large file_size to ensure it is selected as the seed via
  // FilesByCompactionPri (kByCompensatedSize = largest-first). The oldest
  // file being the seed is what triggers the chain-expansion ordering bug.
  Add(0, 3U, "p", "z", 1U, 0, 281, 300);    // newest, small size
  Add(0, 2U, "g", "t", 1U, 0, 181, 200);    // middle, small size
  Add(0, 1U, "a", "m", 100U, 0, 81, 100);   // oldest, large size -> seed

  UpdateVersionStorageInfo();

  std::unique_ptr<Compaction> compaction(level_compaction_picker.PickCompaction(
      cf_name_, mutable_cf_options_, vstorage_.get(), &log_buffer_));
  ASSERT_TRUE(compaction.get() != nullptr);
  ASSERT_EQ(2U, compaction->num_input_files(0));
  // After sorting by seqno descending, oldest 2 files (1 and 2) are kept.
  // File 3 (newest, seqno=300) must NOT be compacted; it stays in L0 to
  // shadow the older data being written to Lbase.
  for (size_t i = 0; i < compaction->num_input_files(0); i++) {
    ASSERT_NE(3U, compaction->input(0, i)->fd.GetNumber())
        << "Newest file 3 should remain in L0, not be compacted to Lbase";
  }
}

TEST_F(CompactionPickerTest, L0MaxCompactionDynamicLevelMultipleLnFiles) {
  // level_compaction_dynamic_level_bytes=true: the large bottom level (Lmax)
  // determines the effective base_level for L0 compaction, which may be
  // well above L1. This test verifies that the limit is enforced correctly
  // when the output level is not L1 and has multiple files.
  //
  // With max_bytes_for_level_base=200, multiplier=10:
  //   L4 target=200, L5 target=2000, L6 target=20000.
  //   L6 actual ~3300U → base_level = num_levels-3 (= L4 in a 7-level DB).
  //
  // L0: 5 files all [a,z], limit=3 → truncated to oldest 3, range [a,z].
  // L4 (base_level): 3 disjoint files [a,h],[i,q],[r,z].
  // Intermediate levels L1,L2,L3 also have files to simulate a realistic tree.
  // All 3 L4 files overlap truncated [a,z]; all must be included.
  int num_levels = ioptions_.num_levels;
  ioptions_.level_compaction_dynamic_level_bytes = true;
  mutable_cf_options_.level0_file_num_compaction_trigger = 2;
  mutable_cf_options_.level0_max_compaction_file_number = 3;
  mutable_cf_options_.max_bytes_for_level_base = 200;
  mutable_cf_options_.max_bytes_for_level_multiplier = 10;
  mutable_cf_options_.max_compaction_bytes = 100000;

  NewVersionStorage(num_levels, kCompactionStyleLevel);

  // Bottom level (Lmax = num_levels-1 = L6): large data to push base to L4.
  // With base=200, multiplier=10 and L6 total ~3300U, the dynamic calculation
  // gives: cur_level_size = 3300/10/10 = 33U at first_non_empty_level=4,
  // which is within [base_bytes_min=20, base_bytes_max=200] → base_level=4.
  Add(num_levels - 1, 20U, "a", "m", 300U, 0, 2, 15);
  Add(num_levels - 1, 21U, "n", "z", 3000U, 0, 2, 15);

  // L0: 5 files, newest-first by largest_seqno.
  Add(0, 5U, "a", "z", 1U, 0, 481, 500);  // newest, truncated
  Add(0, 4U, "a", "z", 1U, 0, 381, 400);  // truncated
  Add(0, 3U, "a", "z", 1U, 0, 281, 300);  // kept (oldest 3)
  Add(0, 2U, "a", "z", 1U, 0, 181, 200);  // kept
  Add(0, 1U, "a", "z", 1U, 0, 81, 100);   // kept

  // Base level (L4 = num_levels-3): 3 disjoint files covering [a,z].
  // Each file is 5U; total=15U < base_level_size≈33U so L4 score < 1
  // and the L0 compaction (score=2.5) is picked first.
  Add(num_levels - 3, 6U, "a", "h", 5U, 0, 2, 15);
  Add(num_levels - 3, 7U, "i", "q", 5U, 0, 2, 15);
  Add(num_levels - 3, 8U, "r", "z", 5U, 0, 2, 15);

  UpdateVersionStorageInfo();
  ASSERT_EQ(vstorage_->base_level(), num_levels - 3);

  std::unique_ptr<Compaction> compaction(level_compaction_picker.PickCompaction(
      cf_name_, mutable_cf_options_, vstorage_.get(), &log_buffer_));
  ASSERT_TRUE(compaction.get() != nullptr);
  // L0: oldest 3 files (limit enforced even with dynamic base level)
  ASSERT_EQ(3U, compaction->num_input_files(0));
  // Output goes to the dynamic base level, not L1
  ASSERT_EQ(num_levels - 3, compaction->output_level());
  // All 3 base-level files overlap with truncated L0 range [a,z]
  ASSERT_EQ(3U, compaction->num_input_files(1));
}

TEST_F(CompactionPickerTest, L0MaxCompactionDynamicLevelPartialLnOverlap) {
  // Combines dynamic base level with partial Ln overlap after truncation.
  // L0: 5 files, newest 2 cover [a,z], oldest 3 cover [a,d/e/f], limit=3.
  // After truncation, L0 keeps oldest 3 (range [a,f]).
  // L4 (base_level): 3 disjoint files [a,f],[g,m],[n,z].
  // Truncated range [a,f] only overlaps L4[a,f]; the other two L4 files
  // ([g,m] and [n,z]) are outside [a,f] and must NOT be included.
  // l0_truncated guard prevents re-expansion back to [a,z].
  int num_levels = ioptions_.num_levels;
  ioptions_.level_compaction_dynamic_level_bytes = true;
  mutable_cf_options_.level0_file_num_compaction_trigger = 2;
  mutable_cf_options_.level0_max_compaction_file_number = 3;
  mutable_cf_options_.max_bytes_for_level_base = 200;
  mutable_cf_options_.max_bytes_for_level_multiplier = 10;
  mutable_cf_options_.max_compaction_bytes = 100000;

  NewVersionStorage(num_levels, kCompactionStyleLevel);

  // Bottom level (L6): large enough to push base_level to L4.
  Add(num_levels - 1, 20U, "a", "m", 300U, 0, 2, 15);
  Add(num_levels - 1, 21U, "n", "z", 3000U, 0, 2, 15);

  // L0: 5 files. Newest 2 cover full [a,z]; oldest 3 have narrowing ranges.
  Add(0, 5U, "a", "z", 1U, 0, 481, 500);  // newest, truncated
  Add(0, 4U, "a", "z", 1U, 0, 381, 400);  // truncated
  Add(0, 3U, "a", "f", 1U, 0, 281, 300);  // kept (oldest 3), range upper = f
  Add(0, 2U, "a", "e", 1U, 0, 181, 200);  // kept
  Add(0, 1U, "a", "d", 1U, 0, 81, 100);   // kept

  // Base level (L4 = num_levels-3): 3 disjoint files.
  // Only [a,f] overlaps with the truncated L0 range [a,f].
  // Each file is 5U; total=15U < base_level_size≈33U so L4 score < 1
  // and the L0 compaction (score=2.5) is picked first.
  Add(num_levels - 3, 6U, "a", "f", 5U, 0, 2, 15);
  Add(num_levels - 3, 7U, "g", "m", 5U, 0, 2, 15);   // outside truncated range
  Add(num_levels - 3, 8U, "n", "z", 5U, 0, 2, 15);   // outside truncated range

  UpdateVersionStorageInfo();
  ASSERT_EQ(vstorage_->base_level(), num_levels - 3);

  std::unique_ptr<Compaction> compaction(level_compaction_picker.PickCompaction(
      cf_name_, mutable_cf_options_, vstorage_.get(), &log_buffer_));
  ASSERT_TRUE(compaction.get() != nullptr);
  // L0: oldest 3 (range [a,f]), not re-expanded to 5 despite L4 having [g,z]
  ASSERT_EQ(3U, compaction->num_input_files(0));
  ASSERT_EQ(num_levels - 3, compaction->output_level());
  // Only the L4 file overlapping truncated [a,f] is included
  ASSERT_EQ(1U, compaction->num_input_files(1));
}

// Tests for FindIntraL0Compaction with level0_max_compaction_file_number
// and max_compaction_bytes limits.
//
// FindIntraL0Compaction picks files from level_files[0..N) (newest-first).
// The original heuristic expands until cost-per-deleted-file increases.
// With uniform file sizes the heuristic never stops, so the new hard limits
// are the only thing bounding the selection.

TEST_F(CompactionPickerTest, IntraL0MaxCompactionFileNumber) {
  // 8 uniform L0 files, limit=5. Heuristic would pick all 8.
  // level0_max_compaction_file_number=5 caps it at 5.
  NewVersionStorage(6, kCompactionStyleLevel);

  // L0 files newest-first (files_[0] order after UpdateVersionStorageInfo).
  // All have uniform size=1000U so the heuristic never triggers a stop.
  Add(0, 8U, "a", "z", 1000U, 0, 781, 800);
  Add(0, 7U, "a", "z", 1000U, 0, 681, 700);
  Add(0, 6U, "a", "z", 1000U, 0, 581, 600);
  Add(0, 5U, "a", "z", 1000U, 0, 481, 500);
  Add(0, 4U, "a", "z", 1000U, 0, 381, 400);
  Add(0, 3U, "a", "z", 1000U, 0, 281, 300);
  Add(0, 2U, "a", "z", 1000U, 0, 181, 200);
  Add(0, 1U, "a", "z", 1000U, 0, 81, 100);
  UpdateVersionStorageInfo();

  const auto& level_files = vstorage_->LevelFiles(0);
  CompactionInputFiles comp_inputs;
  comp_inputs.level = 0;

  ASSERT_TRUE(FindIntraL0Compaction(
      level_files,
      4 /* min_files_to_compact */,
      port::kMaxUint64 /* max_compact_bytes_per_del_file */,
      port::kMaxUint64 /* max_compaction_bytes */,
      5 /* level0_max_compaction_file_number */,
      &comp_inputs));
  ASSERT_EQ(5U, comp_inputs.files.size());
}

TEST_F(CompactionPickerTest, IntraL0MaxCompactionFileNumberZeroNoLimit) {
  // limit=0 means no limit. All 8 files picked (heuristic never stops with
  // uniform sizes).
  NewVersionStorage(6, kCompactionStyleLevel);

  Add(0, 8U, "a", "z", 1000U, 0, 781, 800);
  Add(0, 7U, "a", "z", 1000U, 0, 681, 700);
  Add(0, 6U, "a", "z", 1000U, 0, 581, 600);
  Add(0, 5U, "a", "z", 1000U, 0, 481, 500);
  Add(0, 4U, "a", "z", 1000U, 0, 381, 400);
  Add(0, 3U, "a", "z", 1000U, 0, 281, 300);
  Add(0, 2U, "a", "z", 1000U, 0, 181, 200);
  Add(0, 1U, "a", "z", 1000U, 0, 81, 100);
  UpdateVersionStorageInfo();

  const auto& level_files = vstorage_->LevelFiles(0);
  CompactionInputFiles comp_inputs;
  comp_inputs.level = 0;

  ASSERT_TRUE(FindIntraL0Compaction(
      level_files,
      4 /* min_files_to_compact */,
      port::kMaxUint64,
      port::kMaxUint64,
      0 /* level0_max_compaction_file_number = no limit */,
      &comp_inputs));
  ASSERT_EQ(8U, comp_inputs.files.size());
}

TEST_F(CompactionPickerTest, IntraL0MaxCompactionBytes) {
  // 8 files each 1000U, max_compaction_bytes=4500.
  // After 4 files: total=4000 < 4500 → OK.
  // 5th file: total=5000 > 4500 → stop.
  // Result: 4 files.
  NewVersionStorage(6, kCompactionStyleLevel);

  Add(0, 8U, "a", "z", 1000U, 0, 781, 800);
  Add(0, 7U, "a", "z", 1000U, 0, 681, 700);
  Add(0, 6U, "a", "z", 1000U, 0, 581, 600);
  Add(0, 5U, "a", "z", 1000U, 0, 481, 500);
  Add(0, 4U, "a", "z", 1000U, 0, 381, 400);
  Add(0, 3U, "a", "z", 1000U, 0, 281, 300);
  Add(0, 2U, "a", "z", 1000U, 0, 181, 200);
  Add(0, 1U, "a", "z", 1000U, 0, 81, 100);
  UpdateVersionStorageInfo();

  const auto& level_files = vstorage_->LevelFiles(0);
  CompactionInputFiles comp_inputs;
  comp_inputs.level = 0;

  ASSERT_TRUE(FindIntraL0Compaction(
      level_files,
      4 /* min_files_to_compact */,
      port::kMaxUint64,
      4500 /* max_compaction_bytes */,
      0 /* no file number limit */,
      &comp_inputs));
  ASSERT_EQ(4U, comp_inputs.files.size());
}

TEST_F(CompactionPickerTest, IntraL0MaxFileNumberBelowMinFiles) {
  // 4 files, limit=2, min_files_to_compact=4.
  // Limit stops expansion at 2, but 2 < min_files → returns false.
  NewVersionStorage(6, kCompactionStyleLevel);

  Add(0, 4U, "a", "z", 1000U, 0, 381, 400);
  Add(0, 3U, "a", "z", 1000U, 0, 281, 300);
  Add(0, 2U, "a", "z", 1000U, 0, 181, 200);
  Add(0, 1U, "a", "z", 1000U, 0, 81, 100);
  UpdateVersionStorageInfo();

  const auto& level_files = vstorage_->LevelFiles(0);
  CompactionInputFiles comp_inputs;
  comp_inputs.level = 0;

  ASSERT_FALSE(FindIntraL0Compaction(
      level_files,
      4 /* min_files_to_compact */,
      port::kMaxUint64,
      port::kMaxUint64,
      2 /* level0_max_compaction_file_number */,
      &comp_inputs));
  // No files selected because min_files threshold not met.
  ASSERT_EQ(0U, comp_inputs.files.size());
}

}  // namespace rocksdb

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
