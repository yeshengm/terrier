#include <memory>
#include <vector>

#include "execution/exec/execution_context.h"
#include "execution/sql/table_vector_iterator.h"
#include "execution/util/timer.h"
#include "tbb/blocked_range.h"
#include "tbb/parallel_for.h"
#include "tbb/task_scheduler_init.h"

namespace terrier::execution::sql {
TableVectorIterator::TableVectorIterator(u32 table_oid, exec::ExecutionContext *exec_ctx)
    : table_oid_(table_oid), exec_ctx_(exec_ctx) {}

TableVectorIterator::~TableVectorIterator() { delete[] buffer_; }

bool TableVectorIterator::Init() {
  // Find the table
  table_ = exec_ctx_->GetAccessor()->GetTable(table_oid_);
  TPL_ASSERT(table_ != nullptr, "Table must exist!!");

  // Initialize the projected column
  TERRIER_ASSERT(!col_oids_.empty(), "There must be at least one col oid!");
  auto pc_map = table_->InitializerForProjectedColumns(col_oids_, kDefaultVectorSize);
  buffer_ = common::AllocationUtil::AllocateAligned(pc_map.first.ProjectedColumnsSize());
  projected_columns_ = pc_map.first.Initialize(buffer_);
  initialized = true;

  // Begin iterating
  iter_ = std::make_unique<storage::DataTable::SlotIterator>(table_->begin());
  return true;
}

bool TableVectorIterator::Advance() {
  if (!initialized) return false;
  // First check if the iterator ended.
  if (*iter_ == table_->end()) {
    return false;
  }
  // Scan the table to set the projected column.
  table_->Scan(exec_ctx_->GetTxn(), iter_.get(), projected_columns_);
  pci_.SetProjectedColumn(projected_columns_);
  return true;
}

bool TableVectorIterator::ParallelScan(u32 db_oid, u32 table_oid, void *const query_state,
                                       ThreadStateContainer *const thread_states, const ScanFn scan_fn,
                                       const u32 min_grain_size) {
  // TODO(Amadou): Implement Me!!
  return false;
}

}  // namespace terrier::execution::sql