/* Copyright 2024 The OpenXLA Authors.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/
#include "xla/service/gpu/fusions/loop_mlir.h"

#include <iterator>
#include <vector>

#include "absl/algorithm/container.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"  // from @llvm-project
#include "mlir/Dialect/Tensor/IR/Tensor.h"  // from @llvm-project
#include "mlir/IR/AffineExpr.h"  // from @llvm-project
#include "mlir/IR/AffineMap.h"  // from @llvm-project
#include "mlir/IR/BuiltinOps.h"  // from @llvm-project
#include "mlir/IR/ImplicitLocOpBuilder.h"  // from @llvm-project
#include "mlir/IR/MLIRContext.h"  // from @llvm-project
#include "mlir/IR/Value.h"  // from @llvm-project
#include "mlir/IR/ValueRange.h"  // from @llvm-project
#include "xla/hlo/ir/hlo_computation.h"
#include "xla/hlo/ir/hlo_instruction.h"
#include "xla/hlo/ir/hlo_instructions.h"
#include "xla/service/gpu/fusions/mlir/computation_partitioner.h"
#include "xla/service/gpu/fusions/mlir/elemental_hlo_to_mlir.h"
#include "xla/service/gpu/launch_dimensions.h"
#include "xla/shape.h"
#include "xla/status_macros.h"
#include "xla/xla_data.pb.h"
#include "tsl/platform/errors.h"
#include "tsl/platform/statusor.h"

namespace xla {
namespace gpu {
namespace {

const Shape& GetFusionResultShape(const HloFusionAnalysis& analysis) {
  const Shape* shape = &analysis.fusion_roots().front()->shape();
  while (shape->IsTuple()) {
    shape = &shape->tuple_shapes(0);
  }
  return *shape;
}

}  // namespace

std::optional<IndexingMap> MlirLoopFusion::ComputeThreadIdToOutputIndexing(
    int64_t root_index, mlir::MLIRContext* ctx) const {
  auto launch_dims = launch_dimensions();
  return GetDefaultThreadIdToOutputIndexingMap(
      launch_dims, config_.unroll_factor, GetFusionResultShape(analysis_), ctx);
}

std::optional<IndexingMap> MlirLoopFusion::ComputeThreadIdToInputIndexing(
    int64_t root_index, int64_t hero_operand_index,
    mlir::MLIRContext* ctx) const {
  std::optional<IndexingMap> thread_id_to_output_indexing =
      ComputeThreadIdToOutputIndexing(root_index, ctx);
  if (!thread_id_to_output_indexing.has_value()) {
    return std::nullopt;
  }
  const HloInstruction* fusion_root = analysis_.fusion_roots()[root_index];
  auto output_to_input_indexing =
      ComputeOutputToInputIndexing(fusion_root, /*output_id=*/0, ctx);
  IndexingMapSet output_to_input_indexing_set =
      output_to_input_indexing.indexing_maps[hero_operand_index];
  // Since we are computing the indexing for a non-fusion op, there is only one
  // indexing map per operand.
  CHECK_EQ(output_to_input_indexing_set.size(), 1);
  IndexingMap thread_id_to_input_indexing_map = ComposeIndexingMaps(
      *thread_id_to_output_indexing, *output_to_input_indexing_set.begin());
  thread_id_to_input_indexing_map.Simplify();
  return thread_id_to_input_indexing_map;
}

LaunchDimensions MlirLoopFusion::launch_dimensions() const {
  return CalculateLaunchDimensions(GetFusionResultShape(analysis_),
                                   analysis_.device_info(), config_);
}

absl::Status MlirLoopFusion::EmitMlir(
    mlir::ModuleOp module, mlir::func::FuncOp entry_function,
    const HloFusionInstruction& fusion) const {
  mlir_converter::PartitionedComputations computations(
      fusion.fused_instructions_computation());

  const auto& root_computation = computations.FindPartitionedComputation(
      fusion.fused_instructions_computation());
  const auto& root_graph = root_computation.GetRootSubgraph();

  auto subgraph_to_mlir_fn = computations.DeclareFunctions(module);
  subgraph_to_mlir_fn.extract(&root_graph).mapped().erase();

  auto call_target_lookup = [&](const HloInstruction* instr) {
    return subgraph_to_mlir_fn[&computations
                                    .FindPartitionedComputation(instr->parent())
                                    .FindSubgraph(instr)];
  };

  for (const auto& comp : computations.partitioned_computations()) {
    for (const auto& subgraph : comp.subgraphs()) {
      if (&subgraph == &root_graph) {
        // We inline the root subgraph.
        continue;
      }
      TF_RETURN_IF_ERROR(mlir_converter::SubgraphToMlirFunction(
          comp, subgraph, subgraph_to_mlir_fn[&subgraph], call_target_lookup));
    }
  }

  mlir::ImplicitLocOpBuilder builder(entry_function.getLoc(), entry_function);
  builder.setInsertionPointToStart(entry_function.addEntryBlock());

  // We enforce that all the root shapes have identical dimensions in
  // IsHloOpSupported.
  auto indexing = ComputeThreadIdToOutputIndexing(0, module.getContext());
  TF_RET_CHECK(indexing) << "Indexing is never nullopt";

  int num_inputs = fusion.fused_instructions_computation()->num_parameters();
  llvm::SmallVector<mlir::Value> input_tensors(
      entry_function.getArguments().take_front(num_inputs));
  auto output_tensor_args =
      entry_function.getArguments().drop_front(num_inputs);

  TF_ASSIGN_OR_RETURN(
      auto result_tensors,
      EmitLoopNest(
          builder, output_tensor_args, *indexing,
          [&](mlir::ValueRange output_tensors, mlir::ValueRange output_indices)
              -> absl::StatusOr<llvm::SmallVector<mlir::Value>> {
            llvm::SmallVector<mlir::Value> args(input_tensors);
            absl::c_copy(output_indices, std::back_inserter(args));
            TF_ASSIGN_OR_RETURN(
                auto result_scalars,
                mlir_converter::SubgraphToMlir(
                    root_computation, root_graph, call_target_lookup,
                    input_tensors, output_indices, builder));

            llvm::SmallVector<mlir::Value> result_tensors;
            result_tensors.reserve(output_tensor_args.size());
            for (auto [tensor, value] :
                 llvm::zip(output_tensors, result_scalars)) {
              result_tensors.push_back(builder
                                           .create<mlir::tensor::InsertOp>(
                                               value, tensor, output_indices)
                                           .getResult());
            }
            return result_tensors;
          }));

  builder.create<mlir::func::ReturnOp>(result_tensors);

  return absl::OkStatus();
}

}  // namespace gpu
}  // namespace xla
