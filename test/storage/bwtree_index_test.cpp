#include <algorithm>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <random>
#include <vector>
#include "portable_endian/portable_endian.h"
#include "storage/garbage_collector.h"
#include "storage/index/compact_ints_key.h"
#include "storage/index/index_builder.h"
#include "storage/projected_row.h"
#include "storage/sql_table.h"
#include "transaction/transaction_context.h"
#include "transaction/transaction_manager.h"
#include "type/type_id.h"
#include "type/type_util.h"
#include "util/random_test_util.h"
#include "util/storage_test_util.h"
#include "util/test_harness.h"
#include "util/transaction_test_util.h"

namespace terrier::storage::index {

class BwTreeIndexTests : public TerrierTest {
 private:
  void StartGC(transaction::TransactionManager *const txn_manager) {
    gc_ = new storage::GarbageCollector(txn_manager);
    run_gc_ = true;
    gc_thread_ = std::thread([this] { GCThreadLoop(); });
  }

  void EndGC() {
    run_gc_ = false;
    gc_thread_.join();
    // Make sure all garbage is collected. This take 2 runs for unlink and deallocate
    gc_->PerformGarbageCollection();
    gc_->PerformGarbageCollection();
    delete gc_;
  }

  std::thread gc_thread_;
  storage::GarbageCollector *gc_ = nullptr;
  volatile bool run_gc_ = false;
  const std::chrono::milliseconds gc_period_{10};

  void GCThreadLoop() {
    while (run_gc_) {
      std::this_thread::sleep_for(gc_period_);
      gc_->PerformGarbageCollection();
    }
  }

  storage::BlockStore block_store_{1000, 1000};
  storage::RecordBufferSegmentPool buffer_pool_{100000, 100000};
  const catalog::Schema table_schema_{
      catalog::Schema({{"attribute", type::TypeId::INTEGER, false, catalog::col_oid_t(0)}})};
  const IndexKeySchema key_schema_{{catalog::indexkeycol_oid_t(1), type::TypeId::INTEGER, false}};

 public:
  std::default_random_engine generator_;
  const uint32_t num_threads_ = 4;

  // SqlTable
  storage::SqlTable *const sql_table_{new storage::SqlTable(&block_store_, table_schema_, catalog::table_oid_t(1))};
  const storage::ProjectedRowInitializer tuple_initializer_{
      sql_table_->InitializerForProjectedRow({catalog::col_oid_t(0)}).first};

  // BwTreeIndex
  Index *default_index_, *unique_index_;
  transaction::TransactionManager txn_manager_{&buffer_pool_, true, LOGGING_DISABLED};

  byte *insert_buffer_, *key_buffer_1_, *key_buffer_2_;

  common::WorkerPool thread_pool_{num_threads_, {}};

 protected:
  void SetUp() override {
    TerrierTest::SetUp();

    StartGC(&txn_manager_);

    unique_index_ = (IndexBuilder()
                         .SetConstraintType(ConstraintType::UNIQUE)
                         .SetKeySchema(key_schema_)
                         .SetOid(catalog::index_oid_t(2)))
                        .Build();
    default_index_ = (IndexBuilder()
                          .SetConstraintType(ConstraintType::DEFAULT)
                          .SetKeySchema(key_schema_)
                          .SetOid(catalog::index_oid_t(2)))
                         .Build();
    insert_buffer_ =
        common::AllocationUtil::AllocateAligned(default_index_->GetProjectedRowInitializer().ProjectedRowSize());
    key_buffer_1_ =
        common::AllocationUtil::AllocateAligned(default_index_->GetProjectedRowInitializer().ProjectedRowSize());
    key_buffer_2_ =
        common::AllocationUtil::AllocateAligned(default_index_->GetProjectedRowInitializer().ProjectedRowSize());
  }
  void TearDown() override {
    EndGC();
    delete sql_table_;
    delete default_index_;
    delete unique_index_;
    delete[] insert_buffer_;
    delete[] key_buffer_1_;
    delete[] key_buffer_2_;
    TerrierTest::TearDown();
  }
};

/**
 * This test creates multiple worker threads that all try to insert [0,num_inserts) as tuples in the table and into the
 * primary key index. At completion of the workload, only num_inserts_ txns should have committed with visible versions
 * in the index and table.
 */
// NOLINTNEXTLINE
TEST_F(BwTreeIndexTests, UniqueInsert) {
  const uint32_t num_inserts_ = 100000;  // number of tuples/primary keys for each worker to attempt to insert
  auto workload = [&](uint32_t worker_id) {
    auto *const insert_buffer =
        common::AllocationUtil::AllocateAligned(unique_index_->GetProjectedRowInitializer().ProjectedRowSize());
    auto *const insert_tuple = tuple_initializer_.InitializeRow(insert_buffer);
    auto *const key_buffer =
        common::AllocationUtil::AllocateAligned(unique_index_->GetProjectedRowInitializer().ProjectedRowSize());
    auto *const insert_key = unique_index_->GetProjectedRowInitializer().InitializeRow(key_buffer);

    // some threads count up, others count down. This is to mix whether threads abort for write-write conflict or
    // previously committed versions
    if (worker_id % 2 == 0) {
      for (uint32_t i = 0; i < num_inserts_; i++) {
        auto *const insert_txn = txn_manager_.BeginTransaction();
        *reinterpret_cast<int32_t *>(insert_tuple->AccessForceNotNull(0)) = i;
        const auto tuple_slot = sql_table_->Insert(insert_txn, *insert_tuple);

        *reinterpret_cast<int32_t *>(insert_key->AccessForceNotNull(0)) = i;
        if (unique_index_->InsertUnique(*insert_txn, *insert_key, tuple_slot)) {
          txn_manager_.Commit(insert_txn, TestCallbacks::EmptyCallback, nullptr);
        } else {
          txn_manager_.Abort(insert_txn);
        }
      }

    } else {
      for (uint32_t i = num_inserts_ - 1; i < num_inserts_; i--) {
        auto *const insert_txn = txn_manager_.BeginTransaction();
        *reinterpret_cast<int32_t *>(insert_tuple->AccessForceNotNull(0)) = i;
        const auto tuple_slot = sql_table_->Insert(insert_txn, *insert_tuple);

        *reinterpret_cast<int32_t *>(insert_key->AccessForceNotNull(0)) = i;
        if (unique_index_->InsertUnique(*insert_txn, *insert_key, tuple_slot)) {
          txn_manager_.Commit(insert_txn, TestCallbacks::EmptyCallback, nullptr);
        } else {
          txn_manager_.Abort(insert_txn);
        }
      }
    }

    delete[] insert_buffer;
    delete[] key_buffer;
  };

  // run the workload
  for (uint32_t i = 0; i < num_threads_; i++) {
    thread_pool_.SubmitTask([i, &workload] { workload(i); });
  }
  thread_pool_.WaitUntilAllFinished();

  // scan the results
  auto *const scan_txn = txn_manager_.BeginTransaction();

  std::vector<storage::TupleSlot> results;

  auto *const low_key_pr = default_index_->GetProjectedRowInitializer().InitializeRow(key_buffer_1_);
  auto *const high_key_pr = default_index_->GetProjectedRowInitializer().InitializeRow(key_buffer_2_);

  // scan[0,num_inserts_) should hit num_inserts_ keys (no duplicates)
  *reinterpret_cast<int32_t *>(low_key_pr->AccessForceNotNull(0)) = 0;
  *reinterpret_cast<int32_t *>(high_key_pr->AccessForceNotNull(0)) = num_inserts_ - 1;
  unique_index_->ScanAscending(*scan_txn, *low_key_pr, *high_key_pr, &results);
  EXPECT_EQ(results.size(), num_inserts_);

  txn_manager_.Commit(scan_txn, TestCallbacks::EmptyCallback, nullptr);
}

/**
 * This test creates multiple worker threads that all try to insert [0,num_inserts) as tuples in the table and into the
 * primary key index. At completion of the workload, all num_inserts_ txns * num_threads_ should have committed with
 * visible versions in the index and table.
 */
// NOLINTNEXTLINE
TEST_F(BwTreeIndexTests, DefaultInsert) {
  const uint32_t num_inserts_ = 100000;  // number of tuples/primary keys for each worker to attempt to insert
  auto workload = [&](uint32_t worker_id) {
    auto *const insert_buffer =
        common::AllocationUtil::AllocateAligned(default_index_->GetProjectedRowInitializer().ProjectedRowSize());
    auto *const insert_tuple = tuple_initializer_.InitializeRow(insert_buffer);
    auto *const key_buffer =
        common::AllocationUtil::AllocateAligned(default_index_->GetProjectedRowInitializer().ProjectedRowSize());
    auto *const insert_key = default_index_->GetProjectedRowInitializer().InitializeRow(key_buffer);

    // some threads count up, others count down. Threads shouldn't abort each other
    if (worker_id % 2 == 0) {
      for (uint32_t i = 0; i < num_inserts_; i++) {
        auto *const insert_txn = txn_manager_.BeginTransaction();
        *reinterpret_cast<int32_t *>(insert_tuple->AccessForceNotNull(0)) = i;
        const auto tuple_slot = sql_table_->Insert(insert_txn, *insert_tuple);

        *reinterpret_cast<int32_t *>(insert_key->AccessForceNotNull(0)) = i;
        EXPECT_TRUE(default_index_->Insert(*insert_key, tuple_slot));
        txn_manager_.Commit(insert_txn, TestCallbacks::EmptyCallback, nullptr);
      }
    } else {
      for (uint32_t i = num_inserts_ - 1; i < num_inserts_; i--) {
        auto *const insert_txn = txn_manager_.BeginTransaction();
        *reinterpret_cast<int32_t *>(insert_tuple->AccessForceNotNull(0)) = i;
        const auto tuple_slot = sql_table_->Insert(insert_txn, *insert_tuple);

        *reinterpret_cast<int32_t *>(insert_key->AccessForceNotNull(0)) = i;
        EXPECT_TRUE(default_index_->Insert(*insert_key, tuple_slot));
        txn_manager_.Commit(insert_txn, TestCallbacks::EmptyCallback, nullptr);
      }
    }

    delete[] insert_buffer;
    delete[] key_buffer;
  };

  // run the workload
  for (uint32_t i = 0; i < num_threads_; i++) {
    thread_pool_.SubmitTask([i, &workload] { workload(i); });
  }
  thread_pool_.WaitUntilAllFinished();

  // scan the results
  auto *const scan_txn = txn_manager_.BeginTransaction();

  std::vector<storage::TupleSlot> results;

  auto *const low_key_pr = default_index_->GetProjectedRowInitializer().InitializeRow(key_buffer_1_);
  auto *const high_key_pr = default_index_->GetProjectedRowInitializer().InitializeRow(key_buffer_2_);

  // scan[0,num_inserts_) should hit num_inserts_ * num_threads_ keys
  *reinterpret_cast<int32_t *>(low_key_pr->AccessForceNotNull(0)) = 0;
  *reinterpret_cast<int32_t *>(high_key_pr->AccessForceNotNull(0)) = num_inserts_ - 1;
  default_index_->ScanAscending(*scan_txn, *low_key_pr, *high_key_pr, &results);
  EXPECT_EQ(results.size(), num_inserts_ * num_threads_);

  txn_manager_.Commit(scan_txn, TestCallbacks::EmptyCallback, nullptr);
}

/**
 * Tests basic scan behavior using various windows to scan over (some out of of bounds of keyspace, some matching
 * exactly, etc.)
 */
// NOLINTNEXTLINE
TEST_F(BwTreeIndexTests, ScanAscending) {
  // populate index with [0..20] even keys
  std::map<int32_t, storage::TupleSlot> reference;
  auto *const insert_txn = txn_manager_.BeginTransaction();
  for (int32_t i = 0; i <= 20; i += 2) {
    auto *const insert_tuple = tuple_initializer_.InitializeRow(insert_buffer_);
    *reinterpret_cast<int32_t *>(insert_tuple->AccessForceNotNull(0)) = i;
    const auto tuple_slot = sql_table_->Insert(insert_txn, *insert_tuple);

    auto *const insert_key = default_index_->GetProjectedRowInitializer().InitializeRow(insert_buffer_);
    *reinterpret_cast<int32_t *>(insert_key->AccessForceNotNull(0)) = i;
    EXPECT_TRUE(default_index_->Insert(*insert_key, tuple_slot));
    reference[i] = tuple_slot;
  }
  txn_manager_.Commit(insert_txn, TestCallbacks::EmptyCallback, nullptr);

  auto *const scan_txn = txn_manager_.BeginTransaction();

  std::vector<storage::TupleSlot> results;

  auto *const low_key_pr = default_index_->GetProjectedRowInitializer().InitializeRow(key_buffer_1_);
  auto *const high_key_pr = default_index_->GetProjectedRowInitializer().InitializeRow(key_buffer_2_);

  // scan[8,12] should hit keys 8, 10, 12
  *reinterpret_cast<int32_t *>(low_key_pr->AccessForceNotNull(0)) = 8;
  *reinterpret_cast<int32_t *>(high_key_pr->AccessForceNotNull(0)) = 12;
  default_index_->ScanAscending(*scan_txn, *low_key_pr, *high_key_pr, &results);
  EXPECT_EQ(results.size(), 3);
  EXPECT_EQ(reference.at(8), results[0]);
  EXPECT_EQ(reference.at(10), results[1]);
  EXPECT_EQ(reference.at(12), results[2]);
  results.clear();

  // scan[7,13] should hit keys 8, 10, 12
  *reinterpret_cast<int32_t *>(low_key_pr->AccessForceNotNull(0)) = 7;
  *reinterpret_cast<int32_t *>(high_key_pr->AccessForceNotNull(0)) = 13;
  default_index_->ScanAscending(*scan_txn, *low_key_pr, *high_key_pr, &results);
  EXPECT_EQ(results.size(), 3);
  EXPECT_EQ(reference.at(8), results[0]);
  EXPECT_EQ(reference.at(10), results[1]);
  EXPECT_EQ(reference.at(12), results[2]);
  results.clear();

  // scan[-1,5] should hit keys 0, 2, 4
  *reinterpret_cast<int32_t *>(low_key_pr->AccessForceNotNull(0)) = -1;
  *reinterpret_cast<int32_t *>(high_key_pr->AccessForceNotNull(0)) = 5;
  default_index_->ScanAscending(*scan_txn, *low_key_pr, *high_key_pr, &results);
  EXPECT_EQ(results.size(), 3);
  EXPECT_EQ(reference.at(0), results[0]);
  EXPECT_EQ(reference.at(2), results[1]);
  EXPECT_EQ(reference.at(4), results[2]);
  results.clear();

  // scan[15,21] should hit keys 16, 18, 20
  *reinterpret_cast<int32_t *>(low_key_pr->AccessForceNotNull(0)) = 15;
  *reinterpret_cast<int32_t *>(high_key_pr->AccessForceNotNull(0)) = 21;
  default_index_->ScanAscending(*scan_txn, *low_key_pr, *high_key_pr, &results);
  EXPECT_EQ(results.size(), 3);
  EXPECT_EQ(reference.at(16), results[0]);
  EXPECT_EQ(reference.at(18), results[1]);
  EXPECT_EQ(reference.at(20), results[2]);
  results.clear();

  txn_manager_.Commit(scan_txn, TestCallbacks::EmptyCallback, nullptr);
}

/**
 * Tests basic scan behavior using various windows to scan over (some out of of bounds of keyspace, some matching
 * exactly, etc.)
 */
// NOLINTNEXTLINE
TEST_F(BwTreeIndexTests, ScanDescending) {
  // populate index with [0..20] even keys
  std::map<int32_t, storage::TupleSlot> reference;
  auto *const insert_txn = txn_manager_.BeginTransaction();
  for (int32_t i = 0; i <= 20; i += 2) {
    auto *const insert_tuple = tuple_initializer_.InitializeRow(insert_buffer_);
    *reinterpret_cast<int32_t *>(insert_tuple->AccessForceNotNull(0)) = i;
    const auto tuple_slot = sql_table_->Insert(insert_txn, *insert_tuple);

    auto *const insert_key = default_index_->GetProjectedRowInitializer().InitializeRow(insert_buffer_);
    *reinterpret_cast<int32_t *>(insert_key->AccessForceNotNull(0)) = i;
    EXPECT_TRUE(default_index_->Insert(*insert_key, tuple_slot));
    reference[i] = tuple_slot;
  }
  txn_manager_.Commit(insert_txn, TestCallbacks::EmptyCallback, nullptr);

  auto *const scan_txn = txn_manager_.BeginTransaction();

  std::vector<storage::TupleSlot> results;

  auto *const low_key_pr = default_index_->GetProjectedRowInitializer().InitializeRow(key_buffer_1_);
  auto *const high_key_pr = default_index_->GetProjectedRowInitializer().InitializeRow(key_buffer_2_);

  // scan[8,12] should hit keys 12, 10, 8
  *reinterpret_cast<int32_t *>(low_key_pr->AccessForceNotNull(0)) = 8;
  *reinterpret_cast<int32_t *>(high_key_pr->AccessForceNotNull(0)) = 12;
  default_index_->ScanDescending(*scan_txn, *low_key_pr, *high_key_pr, &results);
  EXPECT_EQ(results.size(), 3);
  EXPECT_EQ(reference.at(12), results[0]);
  EXPECT_EQ(reference.at(10), results[1]);
  EXPECT_EQ(reference.at(8), results[2]);
  results.clear();

  // scan[7,13] should hit keys 12, 10, 8
  *reinterpret_cast<int32_t *>(low_key_pr->AccessForceNotNull(0)) = 7;
  *reinterpret_cast<int32_t *>(high_key_pr->AccessForceNotNull(0)) = 13;
  default_index_->ScanDescending(*scan_txn, *low_key_pr, *high_key_pr, &results);
  EXPECT_EQ(results.size(), 3);
  EXPECT_EQ(reference.at(12), results[0]);
  EXPECT_EQ(reference.at(10), results[1]);
  EXPECT_EQ(reference.at(8), results[2]);
  results.clear();

  // scan[-1,5] should hit keys 4, 2, 0
  *reinterpret_cast<int32_t *>(low_key_pr->AccessForceNotNull(0)) = -1;
  *reinterpret_cast<int32_t *>(high_key_pr->AccessForceNotNull(0)) = 5;
  default_index_->ScanDescending(*scan_txn, *low_key_pr, *high_key_pr, &results);
  EXPECT_EQ(results.size(), 3);
  EXPECT_EQ(reference.at(4), results[0]);
  EXPECT_EQ(reference.at(2), results[1]);
  EXPECT_EQ(reference.at(0), results[2]);
  results.clear();

  // scan[15,21] should hit keys 20, 18, 16
  *reinterpret_cast<int32_t *>(low_key_pr->AccessForceNotNull(0)) = 15;
  *reinterpret_cast<int32_t *>(high_key_pr->AccessForceNotNull(0)) = 21;
  default_index_->ScanDescending(*scan_txn, *low_key_pr, *high_key_pr, &results);
  EXPECT_EQ(results.size(), 3);
  EXPECT_EQ(reference.at(20), results[0]);
  EXPECT_EQ(reference.at(18), results[1]);
  EXPECT_EQ(reference.at(16), results[2]);
  results.clear();

  txn_manager_.Commit(scan_txn, TestCallbacks::EmptyCallback, nullptr);
}

/**
 * Tests basic scan behavior using various windows to scan over (some out of of bounds of keyspace, some matching
 * exactly, etc.)
 */
// NOLINTNEXTLINE
TEST_F(BwTreeIndexTests, ScanLimitAscending) {
  // populate index with [0..20] even keys
  std::map<int32_t, storage::TupleSlot> reference;
  auto *const insert_txn = txn_manager_.BeginTransaction();
  for (int32_t i = 0; i <= 20; i += 2) {
    auto *const insert_tuple = tuple_initializer_.InitializeRow(insert_buffer_);
    *reinterpret_cast<int32_t *>(insert_tuple->AccessForceNotNull(0)) = i;
    const auto tuple_slot = sql_table_->Insert(insert_txn, *insert_tuple);

    auto *const insert_key = default_index_->GetProjectedRowInitializer().InitializeRow(insert_buffer_);
    *reinterpret_cast<int32_t *>(insert_key->AccessForceNotNull(0)) = i;
    EXPECT_TRUE(default_index_->Insert(*insert_key, tuple_slot));
    reference[i] = tuple_slot;
  }
  txn_manager_.Commit(insert_txn, TestCallbacks::EmptyCallback, nullptr);

  auto *const scan_txn = txn_manager_.BeginTransaction();

  std::vector<storage::TupleSlot> results;

  auto *const low_key_pr = default_index_->GetProjectedRowInitializer().InitializeRow(key_buffer_1_);
  auto *const high_key_pr = default_index_->GetProjectedRowInitializer().InitializeRow(key_buffer_2_);

  // scan_limit[8,12] should hit keys 8, 10
  *reinterpret_cast<int32_t *>(low_key_pr->AccessForceNotNull(0)) = 8;
  *reinterpret_cast<int32_t *>(high_key_pr->AccessForceNotNull(0)) = 12;
  default_index_->ScanLimitAscending(*scan_txn, *low_key_pr, *high_key_pr, &results, 2);
  EXPECT_EQ(results.size(), 2);
  EXPECT_EQ(reference.at(8), results[0]);
  EXPECT_EQ(reference.at(10), results[1]);
  results.clear();

  // scan_limit[7,13] should hit keys 8, 10
  *reinterpret_cast<int32_t *>(low_key_pr->AccessForceNotNull(0)) = 7;
  *reinterpret_cast<int32_t *>(high_key_pr->AccessForceNotNull(0)) = 13;
  default_index_->ScanLimitAscending(*scan_txn, *low_key_pr, *high_key_pr, &results, 2);
  EXPECT_EQ(results.size(), 2);
  EXPECT_EQ(reference.at(8), results[0]);
  EXPECT_EQ(reference.at(10), results[1]);
  results.clear();

  // scan_limit[-1,5] should hit keys 0, 2
  *reinterpret_cast<int32_t *>(low_key_pr->AccessForceNotNull(0)) = -1;
  *reinterpret_cast<int32_t *>(high_key_pr->AccessForceNotNull(0)) = 5;
  default_index_->ScanLimitAscending(*scan_txn, *low_key_pr, *high_key_pr, &results, 2);
  EXPECT_EQ(results.size(), 2);
  EXPECT_EQ(reference.at(0), results[0]);
  EXPECT_EQ(reference.at(2), results[1]);
  results.clear();

  // scan_limit[15,21] should hit keys 16, 18
  *reinterpret_cast<int32_t *>(low_key_pr->AccessForceNotNull(0)) = 15;
  *reinterpret_cast<int32_t *>(high_key_pr->AccessForceNotNull(0)) = 21;
  default_index_->ScanLimitAscending(*scan_txn, *low_key_pr, *high_key_pr, &results, 2);
  EXPECT_EQ(results.size(), 2);
  EXPECT_EQ(reference.at(16), results[0]);
  EXPECT_EQ(reference.at(18), results[1]);
  results.clear();

  txn_manager_.Commit(scan_txn, TestCallbacks::EmptyCallback, nullptr);
}

/**
 * Tests basic scan behavior using various windows to scan over (some out of of bounds of keyspace, some matching
 * exactly, etc.)
 */
// NOLINTNEXTLINE
TEST_F(BwTreeIndexTests, ScanLimitDescending) {
  // populate index with [0..20] even keys
  std::map<int32_t, storage::TupleSlot> reference;
  auto *const insert_txn = txn_manager_.BeginTransaction();
  for (int32_t i = 0; i <= 20; i += 2) {
    auto *const insert_tuple = tuple_initializer_.InitializeRow(insert_buffer_);
    *reinterpret_cast<int32_t *>(insert_tuple->AccessForceNotNull(0)) = i;
    const auto tuple_slot = sql_table_->Insert(insert_txn, *insert_tuple);

    auto *const insert_key = default_index_->GetProjectedRowInitializer().InitializeRow(insert_buffer_);
    *reinterpret_cast<int32_t *>(insert_key->AccessForceNotNull(0)) = i;
    EXPECT_TRUE(default_index_->Insert(*insert_key, tuple_slot));
    reference[i] = tuple_slot;
  }
  txn_manager_.Commit(insert_txn, TestCallbacks::EmptyCallback, nullptr);

  auto *const scan_txn = txn_manager_.BeginTransaction();

  std::vector<storage::TupleSlot> results;

  auto *const low_key_pr = default_index_->GetProjectedRowInitializer().InitializeRow(key_buffer_1_);
  auto *const high_key_pr = default_index_->GetProjectedRowInitializer().InitializeRow(key_buffer_2_);

  // scan_limit[8,12] should hit keys 12, 10
  *reinterpret_cast<int32_t *>(low_key_pr->AccessForceNotNull(0)) = 8;
  *reinterpret_cast<int32_t *>(high_key_pr->AccessForceNotNull(0)) = 12;
  default_index_->ScanLimitDescending(*scan_txn, *low_key_pr, *high_key_pr, &results, 2);
  EXPECT_EQ(results.size(), 2);
  EXPECT_EQ(reference.at(12), results[0]);
  EXPECT_EQ(reference.at(10), results[1]);
  results.clear();

  // scan_limit[7,13] should hit keys 12, 10
  *reinterpret_cast<int32_t *>(low_key_pr->AccessForceNotNull(0)) = 7;
  *reinterpret_cast<int32_t *>(high_key_pr->AccessForceNotNull(0)) = 13;
  default_index_->ScanLimitDescending(*scan_txn, *low_key_pr, *high_key_pr, &results, 2);
  EXPECT_EQ(results.size(), 2);
  EXPECT_EQ(reference.at(12), results[0]);
  EXPECT_EQ(reference.at(10), results[1]);
  results.clear();

  // scan_limit[-1,5] should hit keys 4, 2
  *reinterpret_cast<int32_t *>(low_key_pr->AccessForceNotNull(0)) = -1;
  *reinterpret_cast<int32_t *>(high_key_pr->AccessForceNotNull(0)) = 5;
  default_index_->ScanLimitDescending(*scan_txn, *low_key_pr, *high_key_pr, &results, 2);
  EXPECT_EQ(results.size(), 2);
  EXPECT_EQ(reference.at(4), results[0]);
  EXPECT_EQ(reference.at(2), results[1]);
  results.clear();

  // scan_limit[15,21] should hit keys 20, 18
  *reinterpret_cast<int32_t *>(low_key_pr->AccessForceNotNull(0)) = 15;
  *reinterpret_cast<int32_t *>(high_key_pr->AccessForceNotNull(0)) = 21;
  default_index_->ScanLimitDescending(*scan_txn, *low_key_pr, *high_key_pr, &results, 2);
  EXPECT_EQ(results.size(), 2);
  EXPECT_EQ(reference.at(20), results[0]);
  EXPECT_EQ(reference.at(18), results[1]);
  results.clear();

  txn_manager_.Commit(scan_txn, TestCallbacks::EmptyCallback, nullptr);
}

//    Txn #0 | Txn #1 | Txn #2 |
//    --------------------------
//    BEGIN  |        |        |
//    W(X)   |        |        |
//    R(X)   |        |        |
//           | BEGIN  |        |
//           | R(X)   |        |
//    COMMIT |        |        |
//           | R(X)   |        |
//           | COMMIT |        |
//           |        | BEGIN  |
//           |        | R(X)   |
//           |        | COMMIT |
//
// Txn #0 should only read Txn #0's version of X
// Txn #1 should only read the previous version of X because its start time is before #0's commit
// Txn #2 should only read Txn #0's version of X
//
// This test confirms that we are not susceptible to the DIRTY READS and UNREPEATABLE READS anomalies
// NOLINTNEXTLINE
TEST_F(BwTreeIndexTests, CommitInsert1) {
  auto *txn0 = txn_manager_.BeginTransaction();

  auto *const insert_tuple = tuple_initializer_.InitializeRow(insert_buffer_);
  *reinterpret_cast<int32_t *>(insert_tuple->AccessForceNotNull(0)) = 15721;
  const auto tuple_slot = sql_table_->Insert(txn0, *insert_tuple);

  auto *const insert_key = default_index_->GetProjectedRowInitializer().InitializeRow(insert_buffer_);
  *reinterpret_cast<int32_t *>(insert_key->AccessForceNotNull(0)) = 15721;
  EXPECT_TRUE(default_index_->Insert(*insert_key, tuple_slot));

  std::vector<storage::TupleSlot> results;

  auto *const scan_key_pr = default_index_->GetProjectedRowInitializer().InitializeRow(key_buffer_1_);

  *reinterpret_cast<int32_t *>(scan_key_pr->AccessForceNotNull(0)) = 15721;
  default_index_->ScanKey(*txn0, *scan_key_pr, &results);
  EXPECT_EQ(results.size(), 1);
  EXPECT_EQ(tuple_slot, results[0]);
  results.clear();

  auto *txn1 = txn_manager_.BeginTransaction();

  default_index_->ScanKey(*txn1, *scan_key_pr, &results);
  EXPECT_EQ(results.size(), 0);
  results.clear();

  txn_manager_.Commit(txn0, TestCallbacks::EmptyCallback, nullptr);

  default_index_->ScanKey(*txn1, *scan_key_pr, &results);
  EXPECT_EQ(results.size(), 0);
  results.clear();

  txn_manager_.Commit(txn1, TestCallbacks::EmptyCallback, nullptr);

  auto *txn2 = txn_manager_.BeginTransaction();

  default_index_->ScanKey(*txn2, *scan_key_pr, &results);
  EXPECT_EQ(results.size(), 1);
  EXPECT_EQ(tuple_slot, results[0]);
  results.clear();

  txn_manager_.Commit(txn2, TestCallbacks::EmptyCallback, nullptr);
}

}  // namespace terrier::storage::index
