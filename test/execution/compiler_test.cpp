#include "catalog/catalog_defs.h"

#include <memory>
#include <functional>
#include <string>
#include <utility>
#include <vector>
#include <limits>
#include "execution/ast/ast_dump.h"

#include "execution/sql_test.h"  // NOLINT
#include "execution/compiler/compiler.h"
#include "execution/compiler/query.h"
#include "execution/exec/execution_context.h"
#include "execution/exec/output.h"
#include "execution/sema/sema.h"
#include "execution/sql/value.h"
#include "execution/vm/bytecode_generator.h"
#include "execution/vm/bytecode_module.h"
#include "execution/vm/module.h"
#include "execution/vm/llvm_engine.h"

#include "planner/plannodes/nested_loop_join_plan_node.h"
#include "planner/plannodes/order_by_plan_node.h"
#include "planner/plannodes/output_schema.h"
#include "planner/plannodes/seq_scan_plan_node.h"
#include "planner/plannodes/index_join_plan_node.h"
#include "planner/plannodes/aggregate_plan_node.h"
#include "planner/plannodes/hash_join_plan_node.h"
#include "type/transient_value.h"
#include "type/transient_value_factory.h"
#include "type/type_id.h"

#include "execution/compiler/expression_util.h"
#include "execution/compiler/output_schema_util.h"
#include "execution/compiler/output_checker.h"

namespace tpl::compiler::test {
using namespace terrier::planner;
using namespace terrier::parser;

class CompilerTest : public SqlBasedTest {
 public:

  void SetUp() override {
    SqlBasedTest::SetUp();
    // Make the test tables
    auto exec_ctx = MakeExecCtx();
    sql::TableGenerator table_generator{exec_ctx.get()};
    table_generator.GenerateTestTables();
  }

  static void CompileAndRun(terrier::planner::AbstractPlanNode *node, exec::ExecutionContext * exec_ctx) {
    // Create the query object, whose region must outlive all the processing.
    tpl::compiler::Query query(*node, exec_ctx);


    // Compile and check for errors
    Compiler compiler(&query);
    compiler.Compile();
    if (query.GetReporter()->HasErrors()) {
      EXECUTION_LOG_ERROR("Type-checking error!");
      query.GetReporter()->PrintErrors();
    }
    std::cout << "Converted: " << std::endl;
    auto root = query.GetCompiledFile();
    tpl::ast::AstDump::Dump(root);

    // Convert to bytecode
    /*auto bytecode_module = vm::BytecodeGenerator::Compile(root, exec_ctx, "tmp-tpl");
    auto module = std::make_unique<vm::Module>(std::move(bytecode_module));

    // Run the main function
    std::function<u32(exec::ExecutionContext *)> main;
    if (!module->GetFunction("main", vm::ExecutionMode::Interpret, &main)) {
      EXECUTION_LOG_ERROR(
          "Missing 'main' entry function with signature "
          "(*ExecutionContext)->int32");
      return;
    }
    auto memory = std::make_unique<sql::MemoryPool>(nullptr);
    exec_ctx->SetMemoryPool(std::move(memory));
    EXECUTION_LOG_INFO("VM main() returned: {}", main(exec_ctx));*/
  }
};


// NOLINTNEXTLINE
TEST_F(CompilerTest, SimpleSeqScanTest) {
  // SELECT col1, col2, col1 * col2, col1 >= 100*col2 FROM test_1 WHERE col1 < 500 AND col2 >= 3;
  // Get the right oid
  auto accessor = MakeAccessor();
  auto * catalog_table = accessor->GetUserTable("test_1");

  std::shared_ptr<AbstractPlanNode> seq_scan;
  OutputSchemaHelper seq_scan_out{0};
  {
    // Get Table columns
    auto col1 = ExpressionUtil::TVE(0, 0, terrier::type::TypeId::INTEGER);
    auto col2 = ExpressionUtil::TVE(0, 1, terrier::type::TypeId::INTEGER);
    // Make New Column
    auto col3 = ExpressionUtil::OpMul(col1, col2);
    auto col4 = ExpressionUtil::ComparisonLt(col1, ExpressionUtil::OpMul(ExpressionUtil::Constant(100), col2));
    seq_scan_out.AddOutput("col1", col1);
    seq_scan_out.AddOutput("col2", col2);
    seq_scan_out.AddOutput("col3", col3);
    seq_scan_out.AddOutput("col4", col4);
    auto schema = seq_scan_out.MakeSchema();
    // Make predicate
    auto comp1 = ExpressionUtil::ComparisonLt(col1, ExpressionUtil::Constant(500));
    auto comp2 = ExpressionUtil::ComparisonGe(col2, ExpressionUtil::Constant(3));
    auto predicate = ExpressionUtil::ConjunctionAnd(comp1, comp2);
    // Build
    SeqScanPlanNode::Builder builder;
    seq_scan =
        builder.SetOutputSchema(schema)
            .SetScanPredicate(predicate)
            .SetIsParallelFlag(false)
            .SetIsForUpdateFlag(false)
            .SetDatabaseOid(accessor->GetDBOid())
            .SetNamespaceOid(accessor->GetNSOid())
            .SetTableOid(catalog_table->Oid())
            .Build();
  }
  // Make the output checkers
  SingleIntComparisonChecker col1_checker(std::less<i64>(), 0, 500);
  SingleIntComparisonChecker col2_checker(std::greater_equal<i64>(), 1, 3);
  MultiChecker multi_checker{std::vector<OutputChecker*>{&col1_checker}};

  // Create the execution context
  OutputStore store{&multi_checker, seq_scan->GetOutputSchema().get()};
  exec::OutputPrinter printer(seq_scan->GetOutputSchema().get());
  MultiOutputCallback callack{std::vector<exec::OutputCallback>{store, printer}};
  auto exec_ctx = MakeExecCtx(std::move(callack), seq_scan->GetOutputSchema().get());

  // Create the query object, whose region must outlive all the processing.
  CompileAndRun(seq_scan.get(), exec_ctx.get());
  multi_checker.CheckCorrectness();
}

/*
// NOLINTNEXTLINE
TEST_F(CompilerTest, SimpleAggregateTest) {
  // SELECT col2, SUM(col1) FROM test_1 WHERE col1 < 1000 GROUP BY col2;
  // Begin a transaction

  auto exec = sql::ExecutionStructures::Instance();
  auto txn_mgr = exec->GetTxnManager();
  auto txn = txn_mgr->BeginTransaction();
  // Get the right oids
  auto test_db_ns = exec->GetTestDBAndNS();
  auto * catalog_table = exec->GetCatalog()->GetUserTable(txn, test_db_ns.first, test_db_ns.second, "test_1");

  std::shared_ptr<AbstractPlanNode> seq_scan;
  OutputSchemaHelper seq_scan_out{0};
  {
    // Get Table columns
    auto col1 = ExpressionUtil::TVE(0, 0, terrier::type::TypeId::INTEGER);
    auto col2 = ExpressionUtil::TVE(0, 1, terrier::type::TypeId::INTEGER);
    seq_scan_out.AddOutput("col1", col1);
    seq_scan_out.AddOutput("col2", col2);
    auto schema = seq_scan_out.MakeSchema();
    // Make predicate
    auto predicate = ExpressionUtil::ComparisonLt(col1, ExpressionUtil::Constant(1000));
    // Build
    SeqScanPlanNode::Builder builder;
    seq_scan =
        builder.SetOutputSchema(schema)
            .SetScanPredicate(predicate)
            .SetIsParallelFlag(false)
            .SetIsForUpdateFlag(false)
            .SetDatabaseOid(test_db_ns.first)
            .SetNamespaceOid(test_db_ns.second)
            .SetTableOid(catalog_table->Oid())
            .Build();
  }
  // Make the aggregate
  std::shared_ptr<AbstractPlanNode> agg;
  OutputSchemaHelper agg_out{0};
  {
    // Read previous output
    auto col1 = seq_scan_out.GetOutput("col1");
    auto col2 = seq_scan_out.GetOutput("col2");
    // Add group by term
    agg_out.AddGroupByTerm("col2", col2);
    // Add aggregates
    auto sum_col1 = ExpressionUtil::AggSum(col1);
    agg_out.AddAggTerm("sum_col1", ExpressionUtil::AggSum(col1));
    // Make the output expressions
    agg_out.AddOutput("col2", agg_out.GetGroupByTermForOutput("col2"));
    agg_out.AddOutput("sum_col1", agg_out.GetAggTermForOutput("sum_col1"));
    auto schema = agg_out.MakeSchema();
    // Build
    AggregatePlanNode::Builder builder;
    agg =
        builder.SetOutputSchema(schema)
        .AddGroupByTerm(agg_out.GetGroupByTerm("col2"))
        .AddAggregateTerm(agg_out.GetAggTerm("col1"))
        .AddChild(seq_scan)
        .SetAggregateStrategyType(AggregateStrategyType::HASH)
        .SetHavingClausePredicate(nullptr)
        .Build();
  }
  // Make the checkers
  NumChecker num_checker{10};
  SingleIntSumChecker sum_checker{1, (1000*999) / 2};
  MultiChecker multi_checker{std::vector<OutputChecker*>{&num_checker, &sum_checker}};

  // Compile and Run
  CompileAndRun(agg.get(), txn, &multi_checker);
  multi_checker.CheckCorrectness();
  txn_mgr->Commit(txn, nullptr, nullptr);
}



// NOLINTNEXTLINE
TEST_F(CompilerTest, SimpleHashJoinTest) {
  // SELECT t1.col1, t2.col1, t2.col2, t1.col1 + t2.col2 FROM t1 INNER JOIN t2 ON t1.col1=t2.col1
  // WHERE t1.col1 < 500 AND t2.col1 < 80
  // TODO(Amadou): Simple join tests are very similar. Some refactoring is possible.
  // Begin a transaction
  auto exec = sql::ExecutionStructures::Instance();
  auto txn_mgr = exec->GetTxnManager();
  auto txn = txn_mgr->BeginTransaction();
  // Get the right oids
  auto test_db_ns = exec->GetTestDBAndNS();
  auto * catalog_table1 = exec->GetCatalog()->GetUserTable(txn, test_db_ns.first, test_db_ns.second, "test_1");
  auto * catalog_table2 = exec->GetCatalog()->GetUserTable(txn, test_db_ns.first, test_db_ns.second, "test_2");

  std::shared_ptr<AbstractPlanNode> seq_scan1;
  OutputSchemaHelper seq_scan_out1{0};
  {
    // Get Table columns
    auto col1 = ExpressionUtil::TVE(0, catalog_table1->ColNumToOffset(0), terrier::type::TypeId::INTEGER);
    auto col2 = ExpressionUtil::TVE(0, catalog_table1->ColNumToOffset(1), terrier::type::TypeId::INTEGER);
    seq_scan_out1.AddOutput("col1", col1);
    seq_scan_out1.AddOutput("col2", col2);
    auto schema = seq_scan_out1.MakeSchema();
    // Make predicate
    auto predicate = ExpressionUtil::ComparisonLt(col1, ExpressionUtil::Constant(500));
    // Build
    SeqScanPlanNode::Builder builder;
    seq_scan1 =
        builder.SetOutputSchema(schema)
            .SetScanPredicate(predicate)
            .SetIsParallelFlag(false)
            .SetIsForUpdateFlag(false)
            .SetDatabaseOid(test_db_ns.first)
            .SetNamespaceOid(test_db_ns.second)
            .SetTableOid(catalog_table1->Oid())
            .Build();
  }
  // Make the second seq scan
  std::shared_ptr<AbstractPlanNode> seq_scan2;
  OutputSchemaHelper seq_scan_out2{1};
  {
    // Get Table columns
    auto col1 = ExpressionUtil::TVE(0, catalog_table2->ColNumToOffset(0), terrier::type::TypeId::SMALLINT);
    auto col2 = ExpressionUtil::TVE(0, catalog_table2->ColNumToOffset(1), terrier::type::TypeId::INTEGER);
    seq_scan_out2.AddOutput("col1", col1);
    seq_scan_out2.AddOutput("col2", col2);
    auto schema = seq_scan_out2.MakeSchema();
    auto predicate = ExpressionUtil::ComparisonLt(col1, ExpressionUtil::Constant(80));
    // Build
    SeqScanPlanNode::Builder builder;
    seq_scan2 =
        builder.SetOutputSchema(schema)
            .SetScanPredicate(predicate)
            .SetIsParallelFlag(false)
            .SetIsForUpdateFlag(false)
            .SetDatabaseOid(test_db_ns.first)
            .SetNamespaceOid(test_db_ns.second)
            .SetTableOid(catalog_table2->Oid())
            .Build();
  }
  // Make hash join
  std::shared_ptr<AbstractPlanNode> hash_join;
  OutputSchemaHelper hash_join_out{0};
  {
    // t1.col1, and t1.col2
    auto t1_col1 = seq_scan_out1.GetOutput("col1");
    auto t1_col2 = seq_scan_out1.GetOutput("col2");
    // t2.col1 and t2.col2
    auto t2_col1 = seq_scan_out2.GetOutput("col1");
    auto t2_col2 = seq_scan_out2.GetOutput("col2");
    // t1.col2 + t2.col2
    auto sum = ExpressionUtil::OpSum(t1_col1, t2_col2);
    // Output Schema
    hash_join_out.AddOutput("t1.col1", t1_col1);
    hash_join_out.AddOutput("t2.col1", t2_col1);
    hash_join_out.AddOutput("t2.col2", t2_col2);
    hash_join_out.AddOutput("sum", sum);
    auto schema = hash_join_out.MakeSchema();
    // Predicate
    auto predicate = ExpressionUtil::ComparisonEq(t1_col1, t2_col1);
    // Build
    HashJoinPlanNode::Builder builder;
    hash_join =
        builder.AddChild(seq_scan1).AddChild(seq_scan2)
        .SetOutputSchema(schema)
        .AddLeftHashKey(t1_col1).AddRightHashKey(t2_col1)
        .SetJoinType(LogicalJoinType::INNER)
        .SetJoinPredicate(predicate)
        .Build();
  }
  // Compile and Run
  // 80 hundred rows should be outputted because of the WHERE clause
  // The joined cols should be equal
  // The 4th column is the sum of the 1nd and 3rd columns
  uint32_t num_output_rows{0};
  uint32_t num_expected_rows{80};
  RowChecker row_checker = [&num_output_rows, num_expected_rows](const std::vector<sql::Val*> vals) {
    // Read cols
    auto col1 = static_cast<sql::Integer*>(vals[0]);
    auto col2 = static_cast<sql::Integer*>(vals[1]);
    auto col3 = static_cast<sql::Integer*>(vals[2]);
    auto col4 = static_cast<sql::Integer*>(vals[3]);
    ASSERT_FALSE(col1->is_null || col2->is_null || col3->is_null || col4->is_null);
    // Check join cols
    ASSERT_EQ(col1->val, col2->val);
    // Check that col4 = col1 + col3
    ASSERT_EQ(col4->val, col1->val + col3->val);
    // Check the number of output row
    num_output_rows++;
    ASSERT_LE(num_output_rows, num_expected_rows);
  };
  CorrectnessFn correcteness_fn = [&num_output_rows, num_expected_rows](){
    ASSERT_EQ(num_output_rows, num_expected_rows);
  };
  GenericChecker checker(row_checker, correcteness_fn);
  CompileAndRun(hash_join.get(), txn, &checker);
  checker.CheckCorrectness();
  txn_mgr->Commit(txn, nullptr, nullptr);
}



// NOLINTNEXTLINE
TEST_F(CompilerTest, SimpleSortTest) {
  // SELECT col1, col2, col1 + col2 FROM test_1 WHERE col1 < 500 ORDER BY col2 ASC, col1 - col2 DESC
  // Begin a transaction
  auto exec = sql::ExecutionStructures::Instance();
  auto txn_mgr = exec->GetTxnManager();
  auto txn = txn_mgr->BeginTransaction();
  // Get the right oids
  auto test_db_ns = exec->GetTestDBAndNS();
  auto * catalog_table = exec->GetCatalog()->GetUserTable(txn, test_db_ns.first, test_db_ns.second, "test_1");

  // Make the seq scan
  std::shared_ptr<AbstractPlanNode> seq_scan;
  OutputSchemaHelper seq_scan_out{0};
  {
    // Get Table columns
    auto col1 = ExpressionUtil::TVE(0, catalog_table->ColNumToOffset(0), terrier::type::TypeId::INTEGER);
    auto col2 = ExpressionUtil::TVE(0, catalog_table->ColNumToOffset(1), terrier::type::TypeId::INTEGER);
    seq_scan_out.AddOutput("col1", col1);
    seq_scan_out.AddOutput("col2", col2);
    auto schema = seq_scan_out.MakeSchema();
    // Make predicate
    auto predicate = ExpressionUtil::ComparisonLt(col1, ExpressionUtil::Constant(500));
    // Build
    SeqScanPlanNode::Builder builder;
    seq_scan =
        builder.SetOutputSchema(schema)
            .SetScanPredicate(predicate)
            .SetIsParallelFlag(false)
            .SetIsForUpdateFlag(false)
            .SetDatabaseOid(test_db_ns.first)
            .SetNamespaceOid(test_db_ns.second)
            .SetTableOid(catalog_table->Oid())
            .Build();
  }
  // Order By
  std::shared_ptr<AbstractPlanNode> order_by;
  OutputSchemaHelper order_by_out{0};
  {
    // Output Colums col1, col2, col1 + col2
    auto col1 = seq_scan_out.GetOutput("col1");
    auto col2 = seq_scan_out.GetOutput("col2");
    auto sum = ExpressionUtil::OpSum(col1, col2);
    order_by_out.AddOutput("col1", col1);
    order_by_out.AddOutput("col2", col2);
    order_by_out.AddOutput("sum", sum);
    auto schema = order_by_out.MakeSchema();
    // Order By Clause
    SortKey clause1{col2, OrderByOrderingType::ASC};
    auto diff = ExpressionUtil::OpMin(col1, col2);
    SortKey clause2{diff, OrderByOrderingType::DESC};
    // Build
    OrderByPlanNode::Builder builder;
    order_by =
        builder.SetOutputSchema(schema)
        .AddChild(seq_scan)
        .AddSortKey(clause1.first, clause1.second)
        .AddSortKey(clause2.first, clause2.second)
        .Build();
  }
  // Checkers:
  // There should be 500 output rows, where col1 < 500.
  // The output should be sorted by col2 ASC, then col1 DESC.
  uint32_t num_output_rows{0};
  uint32_t num_expected_rows{500};
  i64 curr_col1{std::numeric_limits<i64>::min()};
  i64 curr_col2{std::numeric_limits<i64>::min()};
  RowChecker row_checker = [&num_output_rows, &curr_col1, &curr_col2, num_expected_rows](const std::vector<sql::Val*> vals) {
    // Read cols
    auto col1 = static_cast<sql::Integer*>(vals[0]);
    auto col2 = static_cast<sql::Integer*>(vals[1]);
    ASSERT_FALSE(col1->is_null || col2->is_null);
    // Check col1 and number of outputs
    ASSERT_LT(col1->val, 500);
    num_output_rows++;
    ASSERT_LE(num_output_rows, num_expected_rows);

    // Check that output is sorted by col2 ASC, then col1 DESC
    ASSERT_LE(curr_col2, col2->val);
    if (curr_col2 == col2->val) {
      ASSERT_GE(curr_col1, col1->val);
    }
    curr_col1 = col1->val;
    curr_col2 = col2->val;
  };
  CorrectnessFn correcteness_fn = [&num_output_rows, num_expected_rows](){
    ASSERT_EQ(num_output_rows, num_expected_rows);
  };
  GenericChecker checker(row_checker, correcteness_fn);
  // Compile and Run
  CompileAndRun(order_by.get(), txn, &checker);
  checker.CheckCorrectness();
  txn_mgr->Commit(txn, nullptr, nullptr);
}


// NOLINTNEXTLINE
TEST_F(CompilerTest, SimpleNestedLoopJoinTest) {
  // SELECT t1.col1, t2.col1, t2.col2, t1.col1 + t2.col2 FROM t1 INNER JOIN t2 ON t1.col1=t2.col1
  // WHERE t1.col1 < 500 AND t2.col1 < 80
  // TODO(Amadou): Refactor simple join tests because of their similarity.
  // Begin a transaction
  auto exec = sql::ExecutionStructures::Instance();
  auto txn_mgr = exec->GetTxnManager();
  auto txn = txn_mgr->BeginTransaction();
  // Get the right oids
  auto test_db_ns = exec->GetTestDBAndNS();
  auto * catalog_table1 = exec->GetCatalog()->GetUserTable(txn, test_db_ns.first, test_db_ns.second, "test_1");
  auto * catalog_table2 = exec->GetCatalog()->GetUserTable(txn, test_db_ns.first, test_db_ns.second, "test_2");

  std::shared_ptr<AbstractPlanNode> seq_scan1;
  OutputSchemaHelper seq_scan_out1{0};
  {
    // Get Table columns
    auto col1 = ExpressionUtil::TVE(0, catalog_table1->ColNumToOffset(0), terrier::type::TypeId::INTEGER);
    auto col2 = ExpressionUtil::TVE(0, catalog_table1->ColNumToOffset(1), terrier::type::TypeId::INTEGER);
    seq_scan_out1.AddOutput("col1", col1);
    seq_scan_out1.AddOutput("col2", col2);
    auto schema = seq_scan_out1.MakeSchema();
    // Make predicate
    auto predicate = ExpressionUtil::ComparisonLt(col1, ExpressionUtil::Constant(500));
    // Build
    SeqScanPlanNode::Builder builder;
    seq_scan1 =
        builder.SetOutputSchema(schema)
            .SetScanPredicate(predicate)
            .SetIsParallelFlag(false)
            .SetIsForUpdateFlag(false)
            .SetDatabaseOid(test_db_ns.first)
            .SetNamespaceOid(test_db_ns.second)
            .SetTableOid(catalog_table1->Oid())
            .Build();
  }
  // Make the second seq scan
  std::shared_ptr<AbstractPlanNode> seq_scan2;
  OutputSchemaHelper seq_scan_out2{1};
  {
    // Get Table columns
    auto col1 = ExpressionUtil::TVE(0, catalog_table2->ColNumToOffset(0), terrier::type::TypeId::SMALLINT);
    auto col2 = ExpressionUtil::TVE(0, catalog_table2->ColNumToOffset(1), terrier::type::TypeId::INTEGER);
    seq_scan_out2.AddOutput("col1", col1);
    seq_scan_out2.AddOutput("col2", col2);
    auto schema = seq_scan_out2.MakeSchema();
    auto predicate = ExpressionUtil::ComparisonLt(col1, ExpressionUtil::Constant(80));
    // Build
    SeqScanPlanNode::Builder builder;
    seq_scan2 =
        builder.SetOutputSchema(schema)
            .SetScanPredicate(predicate)
            .SetIsParallelFlag(false)
            .SetIsForUpdateFlag(false)
            .SetDatabaseOid(test_db_ns.first)
            .SetNamespaceOid(test_db_ns.second)
            .SetTableOid(catalog_table2->Oid())
            .Build();
  }
  // Make nested loop join
  std::shared_ptr<AbstractPlanNode> nl_join;
  OutputSchemaHelper nl_join_out{0};
  {
    // t1.col1, and t1.col2
    auto t1_col1 = seq_scan_out1.GetOutput("col1");
    auto t1_col2 = seq_scan_out1.GetOutput("col2");
    // t2.col1 and t2.col2
    auto t2_col1 = seq_scan_out2.GetOutput("col1");
    auto t2_col2 = seq_scan_out2.GetOutput("col2");
    // t1.col2 + t2.col2
    auto sum = ExpressionUtil::OpSum(t1_col1, t2_col2);
    // Output Schema
    nl_join_out.AddOutput("t1.col1", t1_col1);
    nl_join_out.AddOutput("t2.col1", t2_col1);
    nl_join_out.AddOutput("t2.col2", t2_col2);
    nl_join_out.AddOutput("sum", sum);
    auto schema = nl_join_out.MakeSchema();
    // Predicate
    auto predicate = ExpressionUtil::ComparisonEq(t1_col1, t2_col1);
    // Build

    NestedLoopJoinPlanNode::Builder builder;
    nl_join =
        builder.AddChild(seq_scan1).AddChild(seq_scan2)
        .SetOutputSchema(schema)
        .SetJoinType(LogicalJoinType::INNER)
        .SetJoinPredicate(predicate)
        .Build();
  }
  // Compile and Run
  // 80 hundred rows should be outputted because of the WHERE clause
  // The joined cols should be equal
  // The 4th column is the sum of the 1nd and 3rd columns
  uint32_t num_output_rows{0};
  uint32_t num_expected_rows{80};
  RowChecker row_checker = [&num_output_rows, num_expected_rows](const std::vector<sql::Val*> vals) {
    // Read cols
    auto col1 = static_cast<sql::Integer*>(vals[0]);
    auto col2 = static_cast<sql::Integer*>(vals[1]);
    auto col3 = static_cast<sql::Integer*>(vals[2]);
    auto col4 = static_cast<sql::Integer*>(vals[3]);
    ASSERT_FALSE(col1->is_null || col2->is_null || col3->is_null || col4->is_null);
    // Check join cols
    ASSERT_EQ(col1->val, col2->val);
    // Check that col4 = col1 + col3
    ASSERT_EQ(col4->val, col1->val + col3->val);
    // Check the number of output row
    num_output_rows++;
    ASSERT_LE(num_output_rows, num_expected_rows);
  };
  CorrectnessFn correcteness_fn = [&num_output_rows, num_expected_rows](){
    ASSERT_EQ(num_output_rows, num_expected_rows);
  };
  GenericChecker checker(row_checker, correcteness_fn);
  // Compile and Run
  CompileAndRun(nl_join.get(), txn, &checker);
  checker.CheckCorrectness();
  txn_mgr->Commit(txn, nullptr, nullptr);
}



// NOLINTNEXTLINE
TEST_F(CompilerTest, SimpleIndexNestedLoopJoin) {
  // SELECT t1.col1, t2.col1, t2.col2, t1.col2 + t2.col2 FROM test_2 AS t2 INNER JOIN test_1 AS t1 ON t1.col1=t2.col1
  // WHERE t1.col1 < 500
  // TODO(Amadou): Refactor simple join tests because of their similarity.
  // Begin a transaction
  auto exec = sql::ExecutionStructures::Instance();
  auto txn_mgr = exec->GetTxnManager();
  auto txn = txn_mgr->BeginTransaction();
  // Get the db and ns oid
  auto test_db_ns = exec->GetTestDBAndNS();

  // Make the seq scan: Here test_2 is the outer table
  std::shared_ptr<AbstractPlanNode> seq_scan;
  OutputSchemaHelper seq_scan_out{0};
  {
    auto * catalog_table2 = exec->GetCatalog()->GetUserTable(txn, test_db_ns.first, test_db_ns.second, "test_2");
    // Get Table columns
    auto col1 = ExpressionUtil::TVE(0, catalog_table2->ColNumToOffset(0), terrier::type::TypeId::SMALLINT);
    auto col2 = ExpressionUtil::TVE(0, catalog_table2->ColNumToOffset(1), terrier::type::TypeId::INTEGER);
    seq_scan_out.AddOutput("col1", col1);
    seq_scan_out.AddOutput("col2", col2);
    auto schema = seq_scan_out.MakeSchema();
    // Make predicate
    auto predicate = ExpressionUtil::ComparisonLt(col1, ExpressionUtil::Constant(80));
    // Build
    SeqScanPlanNode::Builder builder;
    seq_scan =
        builder.SetOutputSchema(schema)
            .SetScanPredicate(predicate)
            .SetIsParallelFlag(false)
            .SetIsForUpdateFlag(false)
            .SetDatabaseOid(test_db_ns.first)
            .SetNamespaceOid(test_db_ns.second)
            .SetTableOid(catalog_table2->Oid())
            .Build();
  }
  // Make index join
  std::shared_ptr<AbstractPlanNode> index_join;
  OutputSchemaHelper index_join_out{0};
  {
    // Retrieve table and index
    auto * catalog_table1 = exec->GetCatalog()->GetUserTable(txn, test_db_ns.first, test_db_ns.second, "test_1");
    terrier::catalog::index_oid_t index_oid = exec->GetCatalog()->GetCatalogIndexOid("index_1");
    // t1.col1, and t1.col2
    auto t1_col1 = ExpressionUtil::TVE(1, catalog_table1->ColNumToOffset(0), terrier::type::TypeId::INTEGER);
    auto t1_col2 = ExpressionUtil::TVE(1, catalog_table1->ColNumToOffset(1), terrier::type::TypeId::INTEGER);
    // t2.col1, and t2.col2
    auto t2_col1 = seq_scan_out.GetOutput("col1");
    auto t2_col2 = seq_scan_out.GetOutput("col2");
    // t1.col2 + t2.col2
    auto sum = ExpressionUtil::OpSum(t1_col1, t2_col2);
    // Output Schema
    index_join_out.AddOutput("t1.col1", t1_col1);
    index_join_out.AddOutput("t2.col1", t2_col1);
    index_join_out.AddOutput("t2.col2", t2_col2);
    index_join_out.AddOutput("sum", sum);
    auto schema = index_join_out.MakeSchema();
    // Predicate
    auto predicate = ExpressionUtil::ComparisonEq(t1_col1, t2_col1);
    // Build
    IndexJoinPlanNode::Builder builder;
    index_join =
        builder.AddChild(seq_scan)
            .SetIndexOid(index_oid).SetTableOid(catalog_table1->Oid())
            .AddIndexColum(t2_col1)
            .SetOutputSchema(schema)
            .SetJoinType(LogicalJoinType::INNER)
            .SetJoinPredicate(predicate)
            .Build();
  }
  // Compile and Run
  // 80 hundred rows should be outputted because of the WHERE clause
  // The joined cols should be equal
  // The 4th column is the sum of the 1nd and 3rd columns
  uint32_t num_output_rows{0};
  uint32_t num_expected_rows{80};
  RowChecker row_checker = [&num_output_rows, num_expected_rows](const std::vector<sql::Val*> vals) {
    // Read cols
    auto col1 = static_cast<sql::Integer*>(vals[0]);
    auto col2 = static_cast<sql::Integer*>(vals[1]);
    auto col3 = static_cast<sql::Integer*>(vals[2]);
    auto col4 = static_cast<sql::Integer*>(vals[3]);
    ASSERT_FALSE(col1->is_null || col2->is_null || col3->is_null || col4->is_null);
    // Check join cols
    ASSERT_EQ(col1->val, col2->val);
    // Check that col4 = col1 + col3
    ASSERT_EQ(col4->val, col1->val + col3->val);
    // Check the number of output row
    num_output_rows++;
    ASSERT_LE(num_output_rows, num_expected_rows);
  };
  CorrectnessFn correcteness_fn = [&num_output_rows, num_expected_rows](){
    ASSERT_EQ(num_output_rows, num_expected_rows);
  };
  GenericChecker checker(row_checker, correcteness_fn);
  // Compile and Run
  CompileAndRun(index_join.get(), txn, &checker);
  checker.CheckCorrectness();
  txn_mgr->Commit(txn, nullptr, nullptr);
}


// NOLINTNEXTLINE
TEST_F(CompilerTest, TPCHQ1Test) {
  // TODO: This should be in the benchmarks
  auto exec = sql::ExecutionStructures::Instance();
  auto txn_mgr = exec->GetTxnManager();
  auto catalog = exec->GetCatalog();
  auto txn = txn_mgr->BeginTransaction();
  // Create lineitem table
  auto test_db_ns = exec->GetTestDBAndNS();
  reader::TableReader table_reader(catalog, txn, test_db_ns.first, test_db_ns.second);
  uint32_t num_written = table_reader.ReadTable("../sample_tpl/tables/lineitem.schema", "../sample_tpl/tables/lineitem.data");
  std::cout << "Wrote " << num_written << " for lineitem table" << std::endl;

  // Get the table from the catalog
  auto * catalog_table = exec->GetCatalog()->GetUserTable(txn, test_db_ns.first, test_db_ns.second, "lineitem");
  // Scan the table
  std::shared_ptr<AbstractPlanNode> seq_scan;
  OutputSchemaHelper seq_scan_out{0};
  {
    // Read all needed columns
    auto l_returnflag = ExpressionUtil::TVE(0, catalog_table->ColNumToOffset(8), terrier::type::TypeId::VARCHAR);
    auto l_linestatus = ExpressionUtil::TVE(0, catalog_table->ColNumToOffset(9), terrier::type::TypeId::VARCHAR);
    auto l_extendedprice = ExpressionUtil::TVE(0, catalog_table->ColNumToOffset(5), terrier::type::TypeId::DECIMAL);
    auto l_discount = ExpressionUtil::TVE(0, catalog_table->ColNumToOffset(6), terrier::type::TypeId::DECIMAL);
    auto l_tax = ExpressionUtil::TVE(0, catalog_table->ColNumToOffset(7), terrier::type::TypeId::DECIMAL);
    auto l_quantity = ExpressionUtil::TVE(0, catalog_table->ColNumToOffset(4), terrier::type::TypeId::DECIMAL);
    auto l_shipdate = ExpressionUtil::TVE(0, catalog_table->ColNumToOffset(10), terrier::type::TypeId::DATE);
    // Make the output schema
    seq_scan_out.AddOutput("l_returnflag", l_returnflag);
    seq_scan_out.AddOutput("l_linestatus", l_linestatus);
    seq_scan_out.AddOutput("l_extendedprice", l_extendedprice);
    seq_scan_out.AddOutput("l_discount", l_discount);
    seq_scan_out.AddOutput("l_tax", l_tax);
    seq_scan_out.AddOutput("l_quantity", l_quantity);
    auto schema = seq_scan_out.MakeSchema();

    // Make the predicate
    seq_scan_out.AddOutput("l_shipdate", l_shipdate);
    auto date_val = (date::sys_days(date::year(1998)/12/01)) - date::days(90);
    auto date_const = ExpressionUtil::Constant(date::year_month_day(date_val));
    auto predicate = ExpressionUtil::ComparisonLt(l_shipdate, date_const);

    // Build
    SeqScanPlanNode::Builder builder;
    seq_scan =
        builder.SetOutputSchema(schema)
            .SetScanPredicate(nullptr)
            .SetIsParallelFlag(false)
            .SetIsForUpdateFlag(false)
            .SetDatabaseOid(test_db_ns.first)
            .SetNamespaceOid(test_db_ns.second)
            .SetTableOid(catalog_table->Oid())
            .Build();
  }
  // Make the aggregate
  std::shared_ptr<AbstractPlanNode> agg;
  OutputSchemaHelper agg_out{0};
  {
    // Read previous layer's output
    auto l_returnflag = seq_scan_out.GetOutput("l_returnflag");
    auto l_linestatus = seq_scan_out.GetOutput("l_linestatus");
    auto l_quantity = seq_scan_out.GetOutput("l_quantity");
    auto l_extendedprice = seq_scan_out.GetOutput("l_extendedprice");
    auto l_discount = seq_scan_out.GetOutput("l_discount");
    auto l_tax = seq_scan_out.GetOutput("l_tax");
    // Make the aggregate expressions
    auto sum_qty = ExpressionUtil::AggSum(l_quantity);
    auto sum_base_price = ExpressionUtil::AggSum(l_extendedprice);
    auto one_const = ExpressionUtil::Constant(1.0);
    auto disc_price = ExpressionUtil::OpMul(l_extendedprice, ExpressionUtil::OpSum(one_const, l_discount));
    auto sum_disc_price = ExpressionUtil::AggSum(disc_price);
    auto charge = ExpressionUtil::OpMul(disc_price, ExpressionUtil::OpSum(one_const, l_tax));
    auto sum_charge = ExpressionUtil::AggSum(charge);
    auto avg_qty = ExpressionUtil::AggAvg(l_quantity);
    auto avg_price = ExpressionUtil::AggAvg(l_extendedprice);
    auto avg_disc = ExpressionUtil::AggAvg(l_discount);
    auto count_order = ExpressionUtil::AggCountStar();
    // Add them to the helper.
    agg_out.AddGroupByTerm("l_returnflag", l_returnflag);
    agg_out.AddGroupByTerm("l_linestatus", l_linestatus);
    agg_out.AddAggTerm("sum_qty", sum_qty);
    agg_out.AddAggTerm("sum_base_price", sum_base_price);
    agg_out.AddAggTerm("sum_disc_price", sum_disc_price);
    agg_out.AddAggTerm("sum_charge", sum_charge);
    agg_out.AddAggTerm("avg_qty", avg_qty);
    agg_out.AddAggTerm("avg_price", avg_price);
    agg_out.AddAggTerm("avg_disc", avg_disc);
    agg_out.AddAggTerm("count_order", count_order);
    // Make the output schema
    agg_out.AddOutput("l_returnflag", agg_out.GetGroupByTermForOutput("l_returnflag"));
    agg_out.AddOutput("l_linestatus", agg_out.GetGroupByTermForOutput("l_linestatus"));
    agg_out.AddOutput("sum_qty", agg_out.GetAggTermForOutput("sum_qty"));
    agg_out.AddOutput("sum_base_price", agg_out.GetAggTermForOutput("sum_base_price"));
    agg_out.AddOutput("sum_disc_price", agg_out.GetAggTermForOutput("sum_disc_price"));
    agg_out.AddOutput("sum_charge", agg_out.GetAggTermForOutput("sum_charge"));
    agg_out.AddOutput("avg_qty", agg_out.GetAggTermForOutput("avg_qty"));
    agg_out.AddOutput("avg_price", agg_out.GetAggTermForOutput("avg_price"));
    agg_out.AddOutput("avg_disc", agg_out.GetAggTermForOutput("avg_disc"));
    agg_out.AddOutput("count_order", agg_out.GetAggTermForOutput("count_order"));
    auto schema = agg_out.MakeSchema();
    // Build
    AggregatePlanNode::Builder builder;
    agg =
        builder.SetOutputSchema(schema)
            .AddGroupByTerm(l_returnflag)
            .AddGroupByTerm(l_linestatus)
            .AddAggregateTerm(sum_qty)
            .AddAggregateTerm(sum_base_price)
            .AddAggregateTerm(sum_disc_price)
            .AddAggregateTerm(sum_charge)
            .AddAggregateTerm(avg_qty)
            .AddAggregateTerm(avg_price)
            .AddAggregateTerm(avg_disc)
            .AddAggregateTerm(count_order)
            .AddChild(seq_scan)
            .SetAggregateStrategyType(AggregateStrategyType::HASH)
            .SetHavingClausePredicate(nullptr)
            .Build();
  }
  // Compile and Run
  // TODO(How to auto check this test?)
  CorrectnessFn correcteness_fn;
  RowChecker row_checker;
  GenericChecker checker(row_checker, correcteness_fn);
  // Compile and Run
  CompileAndRun(agg.get(), txn, &checker);
  checker.CheckCorrectness();
  txn_mgr->Commit(txn, nullptr, nullptr);
}
 */

}  // namespace terrier::planner