#pragma once
#include "execution/exec/execution_context.h"
#include "execution/util/execution_common.h"
namespace terrier::execution::sql {

/**
 * Helper class to perform updates in SQL Tables.
 */
class EXPORT Updater {
 public:
  explicit Updater(exec::ExecutionContext *exec_ctx, std::string table_name, uint32_t *col_oids, uint32_t num_oids,
                   bool is_index_key_update)
      : Updater(exec_ctx, exec_ctx->GetAccessor()->GetTableOid(table_name), col_oids, num_oids, is_index_key_update) {}

  explicit Updater(exec::ExecutionContext *exec_ctx, catalog::table_oid_t table_oid, uint32_t *col_oids,
                   uint32_t num_oids, bool is_index_key_update);

  storage::ProjectedRow *GetTablePR();

  storage::ProjectedRow *GetIndexPR(catalog::index_oid_t index_oid);

  bool TableUpdate(storage::TupleSlot table_tuple_slot);

  bool TableDelete(storage::TupleSlot table_tuple_slot);

  storage::TupleSlot TableInsert();

  void IndexDelete(catalog::index_oid_t index_oid, storage::TupleSlot table_tuple_slot);

  bool IndexInsert(catalog::index_oid_t index_oid);

 private:
  common::ManagedPointer<storage::index::Index> GetIndex(catalog::index_oid_t index_oid);

  uint32_t CalculateMaxIndexPRSize();
  void *GetMaxSizedIndexPRBuffer();

  void PopulateAllColOids(std::vector<catalog::col_oid_t> &all_col_oids);

  storage::ProjectedRow *GetTablePRForColumns(const std::vector<catalog::col_oid_t> &col_oids);

  catalog::table_oid_t table_oid_;
  std::vector<catalog::col_oid_t> col_oids_;
  std::vector<catalog::col_oid_t> all_col_oids_;
  exec::ExecutionContext *exec_ctx_;
  common::ManagedPointer<terrier::storage::SqlTable> table_;

  storage::RedoRecord *table_redo_{nullptr};

  void *index_pr_buffer_;
  storage::ProjectedRow *index_pr_{nullptr};

  std::map<terrier::catalog::index_oid_t, common::ManagedPointer<storage::index::Index>> index_cache_;

  bool is_index_key_update_ = false;
};
}  // namespace terrier::execution::sql