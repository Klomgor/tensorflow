// Copyright 2025 The OpenXLA Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "xla/hlo/tools/hlo_diff/matchers/hlo_computation_graph_matcher.h"

#include <functional>
#include <string>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/functional/function_ref.h"
#include "absl/log/log.h"
#include "xla/hlo/ir/hlo_instruction.h"
#include "xla/hlo/ir/hlo_print_options.h"
#include "xla/hlo/tools/hlo_diff/graph/hlo_gumgraph.h"
#include "xla/hlo/tools/hlo_diff/graph/hlo_gumgraph_node.h"
#include "xla/hlo/tools/hlo_diff/hlo_gumgraph_mappings.h"
#include "xla/hlo/tools/hlo_diff/matchers/similarity.h"
#include "xla/service/call_graph.h"

namespace xla {
namespace hlo_diff {
namespace {

// Function to compute property match score between two instructions.
// Compares various properties of the instructions and returns a double score.
// Higher the score, more similar the instructions are.
using PropertyMatchesFn = absl::FunctionRef<double(const HloInstructionNode*,
                                                   const HloInstructionNode*)>;

// Match instructions with multiple match candidates using similarity measures.
void MatchInstructionsWithMultipleCandidates(
    const absl::flat_hash_set<const HloInstructionNode*>& left_instructions,
    const absl::flat_hash_set<const HloInstructionNode*>& right_instructions,
    HloGumgraphMappings& mappings, PropertyMatchesFn property_matches_fn,
    const MatcherType& matcher_type) {
  for (const HloInstructionNode* left : left_instructions) {
    double max_match_score = 0.0;
    std::vector<const HloInstructionNode*> right_candidates;
    for (const HloInstructionNode* right : right_instructions) {
      double similarity = property_matches_fn(left, right);
      if (similarity > max_match_score) {
        max_match_score = similarity;
        right_candidates.clear();
        right_candidates.push_back(right);
      } else if (similarity == max_match_score) {
        right_candidates.push_back(right);
      }
    }

    // Avoid matching instructions with multiple candidates.
    if (right_candidates.size() == 1) {
      mappings.MapInstructionsIfAbsent(left, right_candidates[0], matcher_type);
    }
  }
}

// Find optimal matches between the left and right leaf instructions - i.e.
// parameter or constant.
// This function is called when attempting to map two computations. The goal is
// to establish a mapping between corresponding leaf instructions from the
// 'left_instructions' and 'right_instructions' sets. These sets are derived
// from the two computations being mapped.
void MatchLeafInstructions(
    const absl::flat_hash_set<const HloInstructionNode*>& left_instructions,
    const absl::flat_hash_set<const HloInstructionNode*>& right_instructions,
    HloGumgraphMappings& mappings, PropertyMatchesFn property_matches_fn,
    const MatcherType& matcher_type) {
  absl::flat_hash_set<const HloInstructionNode*> matched_instructions;

  // Phase 0: Direct mapping if only one instruction in each set.
  if (left_instructions.size() == 1 && right_instructions.size() == 1) {
    mappings.MapInstructionsIfAbsent(*left_instructions.begin(),
                                     *right_instructions.begin(), matcher_type);
    return;  // Early return after direct mapping.
  }

  // Phase 1: Map instructions with the same shape and metadata op name if its
  // specified. This name is often unique within a computation and specified by
  // the frameworks. Note that for XLA generated computations, the metadata is
  // not consistently specified.
  for (const HloInstructionNode* left_instruction : left_instructions) {
    if (left_instruction->instruction->metadata().op_name().empty()) {
      continue;
    }
    int candidates_found = 0;
    const HloInstructionNode* candidate = nullptr;

    for (const HloInstructionNode* right_instruction : right_instructions) {
      bool same_shape = left_instruction->instruction->shape().ToString(
                            /*print_layout=*/false) ==
                        right_instruction->instruction->shape().ToString(
                            /*print_layout=*/false);
      bool same_op_name = left_instruction->instruction->metadata().op_name() ==
                          right_instruction->instruction->metadata().op_name();
      if (same_shape && same_op_name) {
        ++candidates_found;
        candidate = right_instruction;
      }
    }

    // Avoid matching instructions with multiple candidates.
    if (candidates_found == 1) {
      mappings.MapInstructionsIfAbsent(left_instruction, candidate,
                                       matcher_type);
      matched_instructions.insert(left_instruction);
      matched_instructions.insert(candidate);
    }
  }

  // Phase 2: Group instructions by shape.
  // 2.1: Match unique instructions with the same shape
  // 2.2: Match instructions with multiple candidates using similarity measures.
  absl::flat_hash_map<std::string,
                      absl::flat_hash_set<const HloInstructionNode*>>
      left_instructions_by_shape;
  for (const HloInstructionNode* instruction : left_instructions) {
    if (!matched_instructions.contains(instruction)) {
      left_instructions_by_shape[instruction->instruction->shape().ToString(
                                     /*print_layout=*/false)]
          .insert(instruction);
    }
  }

  absl::flat_hash_map<std::string,
                      absl::flat_hash_set<const HloInstructionNode*>>
      right_instructions_by_shape;
  for (const HloInstructionNode* instruction : right_instructions) {
    if (!matched_instructions.contains(instruction)) {
      right_instructions_by_shape[instruction->instruction->shape().ToString(
                                      /*print_layout=*/false)]
          .insert(instruction);
    }
  }

  for (const auto& [shape, shape_left_instructions] :
       left_instructions_by_shape) {
    if (auto it = right_instructions_by_shape.find(shape);
        it != right_instructions_by_shape.end()) {
      absl::flat_hash_set<const HloInstructionNode*> shape_right_instructions =
          it->second;
      // Phase 2.1: Match unique instructions with the same shape.
      if (shape_left_instructions.size() == 1 &&
          shape_right_instructions.size() == 1) {
        mappings.MapInstructionsIfAbsent(*shape_left_instructions.begin(),
                                         *shape_right_instructions.begin(),
                                         matcher_type);
      } else {
        // Phase 2.2: Match instructions with multiple candidates using
        // similarity measures.
        MatchInstructionsWithMultipleCandidates(
            shape_left_instructions, shape_right_instructions, mappings,
            property_matches_fn, matcher_type);
      }
    }
  }
}

// Match parameter instructions between the left and right computations.
void MatchComputationParams(const HloGumgraph& left, const HloGumgraph& right,
                            const CallGraphNode& left_computation,
                            const CallGraphNode& right_computation,
                            HloGumgraphMappings& mappings,
                            const MatcherType& matcher_type) {
  absl::flat_hash_set<const HloInstructionNode*> left_params, right_params;
  for (const HloInstruction* param :
       left_computation.computation()->parameter_instructions()) {
    left_params.insert(left.GetNode(param));
  }
  for (const HloInstruction* param :
       right_computation.computation()->parameter_instructions()) {
    right_params.insert(right.GetNode(param));
  }

  MatchLeafInstructions(left_params, right_params, mappings,
                        std::ref(ParamPropertySimilarity), matcher_type);
}

// Match constant instructions between the left and right computations.
void MatchComputationConstants(const HloGumgraph& left,
                               const HloGumgraph& right,
                               const CallGraphNode& left_computation,
                               const CallGraphNode& right_computation,
                               HloGumgraphMappings& mappings,
                               const MatcherType& matcher_type) {
  absl::flat_hash_set<const HloInstructionNode*> left_constants,
      right_constants;
  for (const HloInstruction* instruction :
       left_computation.computation()->instructions()) {
    if (instruction->IsConstant()) {
      left_constants.insert(left.GetNode(instruction));
    }
  }
  for (const HloInstruction* instruction :
       right_computation.computation()->instructions()) {
    if (instruction->IsConstant()) {
      right_constants.insert(right.GetNode(instruction));
    }
  }

  MatchLeafInstructions(left_constants, right_constants, mappings,
                        std::ref(ConstantPropertySimilarity), matcher_type);
}

// Match the call site instruction and it's operands for a matched left and
// right computation.
void MatchCallSites(const HloGumgraph& left, const HloGumgraph& right,
                    const CallGraphNode& left_computation,
                    const CallGraphNode& right_computation,
                    HloGumgraphMappings& mappings) {
  // Only match call sites if both computations are called from exactly one call
  // site. In case a computation is called from multiple call sites, we cannot
  // disambiguate between the call sites. The subsequent matchers should be able
  // to find the matches between the call sites in such cases.
  if (left_computation.caller_callsites().size() != 1 ||
      right_computation.caller_callsites().size() != 1) {
    return;
  }

  const CallSite& left_call_site = *left_computation.caller_callsites().begin();
  const CallSite& right_call_site =
      *right_computation.caller_callsites().begin();

  // Match the call site instruction.
  mappings.MapInstructionsIfAbsent(
      left.GetNode(left_call_site.instruction()),
      right.GetNode(right_call_site.instruction()),
      MatcherType::kComputationGraphExactSignatureMatcher);
}

}  // namespace

void MatchComputationGraphs(const HloGumgraph& left, const HloGumgraph& right,
                            const CallGraphNode& left_computation,
                            const CallGraphNode& right_computation,
                            HloGumgraphMappings& mappings) {
  auto it = mappings.left_to_right_computation_map.left.find(&left_computation);
  if (it == mappings.left_to_right_computation_map.left.end()) {
    return;
  }

  MatchCallSites(left, right, left_computation, right_computation, mappings);

  // If the two computations are exact matches, we can match all
  // instructions in the two computations.
  if (it->info.computation_match_type == ComputationMatchType::kExact) {
    auto left_instructions =
        left_computation.computation()->MakeInstructionPostOrder();
    auto right_instructions =
        right_computation.computation()->MakeInstructionPostOrder();
    if (left_instructions.size() != right_instructions.size()) {
      LOG(WARNING) << "Computation size mismatch: Left computation: "
                   << left_computation.computation()->name() << " has "
                   << left_instructions.size()
                   << " instructions and right computation: "
                   << right_computation.computation()->name() << " has "
                   << right_instructions.size() << " instructions";
      return;
    }

    for (int i = 0; i < left_instructions.size(); ++i) {
      mappings.MapInstructionsIfAbsent(
          left.GetNode(left_instructions[i]),
          right.GetNode(right_instructions[i]),
          MatcherType::kComputationGraphExactFingerprintMatcher);
    }
  } else {
    // If the two computations are signature matches, we can match the
    // inputs (parameters, constants) and root instruction of the two
    // computation graph.
    MatchComputationParams(left, right, left_computation, right_computation,
                           mappings,
                           MatcherType::kComputationGraphExactSignatureMatcher);
    MatchComputationConstants(
        left, right, left_computation, right_computation, mappings,
        MatcherType::kComputationGraphExactSignatureMatcher);

    if (left_computation.computation()->root_instruction()->opcode() ==
        right_computation.computation()->root_instruction()->opcode()) {
      mappings.MapInstructionsIfAbsent(
          left.GetNode(left_computation.computation()->root_instruction()),
          right.GetNode(right_computation.computation()->root_instruction()),
          MatcherType::kComputationGraphExactSignatureMatcher);
    }
  }
}
}  // namespace hlo_diff
}  // namespace xla
