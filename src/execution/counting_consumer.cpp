//===----------------------------------------------------------------------===//
//
//                         Peloton
//
// counting_consumer.cpp
//
// Identification: src/execution/counting_consumer.cpp
//
// Copyright (c) 2015-2017, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include "execution/counting_consumer.h"
#include "execution/compilation_context.h"

namespace terrier::execution {

void CountingConsumer::Prepare(CompilationContext &ctx) {
  // Be sure to call our parent
  ExecutionConsumer::Prepare(ctx);

  CodeGen &codegen = ctx.GetCodeGen();
  QueryState &query_state = ctx.GetQueryState();
  counter_state_id_ = query_state.RegisterState("consumerState", codegen.Int64Type()->getPointerTo());
}

void CountingConsumer::InitializeQueryState(CompilationContext &context) {
  CodeGen &codegen = context.GetCodeGen();
  auto *state_ptr = GetCounterState(codegen, context.GetQueryState());
  codegen->CreateStore(codegen.Const64(0), state_ptr);
}

// Increment the counter
void CountingConsumer::ConsumeResult(ConsumerContext &context, RowBatch::Row &) const {
  CodeGen &codegen = context.GetCodeGen();

  auto *counter_ptr = GetCounterState(codegen, context.GetQueryState());
  auto *new_count = codegen->CreateAdd(codegen->CreateLoad(counter_ptr), codegen.Const64(1));
  codegen->CreateStore(new_count, counter_ptr);
}

llvm::Value *CountingConsumer::GetCounterState(CodeGen &codegen, QueryState &query_state) const {
  return query_state.LoadStateValue(codegen, counter_state_id_);
}

}  // namespace terrier::execution