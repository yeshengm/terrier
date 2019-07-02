#include "execution/compiler/compiler.h"
#include "execution/compiler/translator_factory.h"
#include "execution/sema/sema.h"
#include "loggers/execution_logger.h"
#include "execution/ast/ast_dump.h"

namespace tpl::compiler {

Compiler::Compiler(tpl::compiler::Query *query)
: query_(query)
, codegen_(query) {
  // Make the pipelines
  auto * main_pipeline = new Pipeline(&codegen_);
  MakePipelines(query_->GetPlan(), main_pipeline);
  // The query has an ouput
  if (query->GetPlan().GetOutputSchema() != nullptr) {
    auto *output_translator = new OutputTranslator(&codegen_);
    main_pipeline->Add(output_translator);
  }
  // Finally add the main pipeline
  pipelines_.push_back(main_pipeline);
  EXECUTION_LOG_INFO("Made {} pipelines", pipelines_.size());
}

void Compiler::Compile() {
  // Step 1: Generate state, structs, helper functions
  util::RegionVector<ast::Decl*> decls(codegen_.Region());
  util::RegionVector<ast::FieldDecl*> state_fields(codegen_.Region());
  util::RegionVector<ast::Stmt*> setup_stmts(codegen_.Region());
  util::RegionVector<ast::Stmt*> teardow_stmts(codegen_.Region());
  // 1.1 Let every pipeline initialize itself
  for (auto & pipeline: pipelines_) {
    pipeline->Initialize(&decls, &state_fields, &setup_stmts, &teardow_stmts);
  }

  // 1.2 Make the top level declarations
  util::RegionVector<ast::Decl*> top_level(codegen_.Region());
  GenStateStruct(&top_level, std::move(state_fields));
  GenHelperStructsAndFunctions(&top_level, std::move(decls));
  GenFunction(&top_level, codegen_.GetSetupFn(), std::move(setup_stmts));
  GenFunction(&top_level, codegen_.GetTeardownFn(), std::move(teardow_stmts));

  // Step 2: For each pipeline: Generate a pipeline function that performs the produce, consume logic
  // TODO(Amadou): This can actually be combined with the previous step to avoid the additional pass
  // over the list of pipelines. However, I find this easier to debug for now.
  uint32_t pipeline_idx = 0;
  for (auto &pipeline: pipelines_) {
    top_level.emplace_back(pipeline->Produce(pipeline_idx++));
  }

  // Step 3: Make the main function
  top_level.emplace_back(GenMainFunction());

  // Compile
  ast::File* compiled_file = codegen_.Compile(std::move(top_level));
  EXECUTION_LOG_INFO("Generated File");
  sema::Sema type_checker{codegen_.Context()};
  type_checker.Run(compiled_file);
  query_->SetCompiledFile(compiled_file);
}

void Compiler::GenStateStruct(tpl::util::RegionVector<tpl::ast::Decl *> *top_level,
                              tpl::util::RegionVector<tpl::ast::FieldDecl *> &&fields) {
  // Make a dummy fields in case no operator has a state
  ast::Identifier dummy_name = codegen_.Context()->GetIdentifier("DUMMY");
  ast::Expr* dummy_type = codegen_.BuiltinType(ast::BuiltinType::Kind::Int32);
  fields.emplace_back(codegen_.MakeField(dummy_name, dummy_type));
  top_level->emplace_back(codegen_.MakeStruct(codegen_.GetStateType(), std::move(fields)));
}

void Compiler::GenHelperStructsAndFunctions(tpl::util::RegionVector<tpl::ast::Decl *> *top_level,
                                            tpl::util::RegionVector<tpl::ast::Decl *> &&decls) {
  top_level->insert(top_level->end(), decls.begin(), decls.end());
}

void Compiler::GenFunction(tpl::util::RegionVector<tpl::ast::Decl *> *top_level, ast::Identifier fn_name,
                                tpl::util::RegionVector<tpl::ast::Stmt *> &&stmts) {
  // Function parameter
  util::RegionVector<ast::FieldDecl*> params = codegen_.ExecParams();

  // Function return type (nil)
  ast::Expr *ret_type = codegen_.BuiltinType(ast::BuiltinType::Kind::Nil);

  // Make the function
  FunctionBuilder builder{&codegen_, fn_name, std::move(params), ret_type};
  for (const auto & stmt: stmts) {
    builder.Append(stmt);
  }
  top_level->emplace_back(builder.Finish());
}

ast::Decl* Compiler::GenMainFunction() {
  // Function name
  ast::Identifier fn_name = codegen_.GetMainFn();

  // Function parameter
  util::RegionVector<ast::FieldDecl*> params = codegen_.MainParams();

  // Function return type (int32)
  ast::Expr *ret_type = codegen_.BuiltinType(ast::BuiltinType::Kind::Int32);

  // Make the function
  FunctionBuilder builder{&codegen_, fn_name, std::move(params), ret_type};

  // Step 0: Define the state variable.
  ast::Identifier state = codegen_.GetStateVar();
  ast::Expr* state_type = codegen_.MakeExpr(codegen_.GetStateType());
  builder.Append(codegen_.DeclareVariable(state, state_type, nullptr));

  // Step 1: Call setupFn(state, execCtx)
  builder.Append(codegen_.ExecCall(codegen_.GetSetupFn()));
  // Step 2: For each pipeline, call its function
  for (const auto & pipeline: pipelines_) {
    builder.Append(codegen_.ExecCall(pipeline->GetPipelineName()));
  }
  // Step 3: Call the teardown function
  builder.Append(codegen_.ExecCall(codegen_.GetTeardownFn()));
  // Step 4: return a value of 0
  builder.Append(codegen_.ReturnStmt(codegen_.IntLiteral(37)));
  return builder.Finish();
}

void Compiler::MakePipelines(const terrier::planner::AbstractPlanNode & op, Pipeline * curr_pipeline) {
  switch (op.GetPlanNodeType()) {
    case terrier::planner::PlanNodeType::AGGREGATE:
    case terrier::planner::PlanNodeType::ORDERBY: {
      OperatorTranslator *bottom_translator = TranslatorFactory::CreateBottomTranslator(&op, &codegen_);
      OperatorTranslator *top_translator = TranslatorFactory::CreateTopTranslator(&op, bottom_translator, &codegen_);
      curr_pipeline->Add(top_translator);
      // Make the nest pipeline
      auto *next_pipeline = new Pipeline(&codegen_);
      MakePipelines(*op.GetChild(0), next_pipeline);
      next_pipeline->Add(bottom_translator);
      pipelines_.push_back(next_pipeline);
      return;
    }
    case terrier::planner::PlanNodeType::HASHJOIN: {
      // Create left and right translators
      OperatorTranslator * left_translator = TranslatorFactory::CreateLeftTranslator(&op, &codegen_);
      OperatorTranslator * right_translator = TranslatorFactory::CreateRightTranslator(&op, left_translator, &codegen_);
      // Generate left pipeline
      auto *next_pipeline = new Pipeline(&codegen_);
      MakePipelines(*op.GetChild(0), next_pipeline);
      next_pipeline->Add(left_translator);
      pipelines_.push_back(next_pipeline);
      // Continue right pipeline
      MakePipelines(*op.GetChild(1), curr_pipeline);
      curr_pipeline->Add(right_translator);
      return;
    }
    case terrier::planner::PlanNodeType::NESTLOOP : {
      // Create left and right translators
      OperatorTranslator * left_translator = TranslatorFactory::CreateLeftTranslator(&op, &codegen_);
      OperatorTranslator * right_translator = TranslatorFactory::CreateRightTranslator(&op, left_translator, &codegen_);
      // Generate left pipeline
      MakePipelines(*op.GetChild(0), curr_pipeline);
      curr_pipeline->Add(left_translator);
      // Append the pipeline
      MakePipelines(*op.GetChild(1), curr_pipeline);
      curr_pipeline->Add(right_translator);
      return;
    }
    default: {
      OperatorTranslator *translator = TranslatorFactory::CreateRegularTranslator(&op, &codegen_);
      if (op.GetChildrenSize() != 0) MakePipelines(*op.GetChild(0), curr_pipeline);
      curr_pipeline->Add(translator);
      return;
    }
  }
}
}
