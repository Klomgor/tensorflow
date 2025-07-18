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

#include "xla/hlo/transforms/host_offloader.h"

#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <memory>
#include <queue>
#include <vector>

#include "absl/algorithm/container.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"
#include "xla/hlo/analysis/hlo_alias_analysis.h"
#include "xla/hlo/ir/hlo_casting_utils.h"
#include "xla/hlo/ir/hlo_computation.h"
#include "xla/hlo/ir/hlo_instruction.h"
#include "xla/hlo/ir/hlo_instructions.h"
#include "xla/hlo/ir/hlo_opcode.h"
#include "xla/literal_util.h"
#include "xla/service/call_graph.h"
#include "xla/service/hlo_buffer.h"
#include "xla/service/hlo_cse.h"
#include "xla/service/hlo_value.h"
#include "xla/service/host_offload_utils.h"
#include "xla/service/memory_annotations.h"
#include "xla/service/pattern_matcher.h"
#include "xla/shape.h"
#include "xla/shape_tree.h"
#include "xla/shape_util.h"
#include "xla/side_effect_util.h"
#include "xla/status_macros.h"
#include "xla/util.h"
#include "xla/xla_data.pb.h"
#include "tsl/platform/errors.h"
#include "tsl/platform/status.h"
#include "tsl/platform/statusor.h"

namespace xla {

namespace {

using ::xla::host_offload_utils::InstructionAndShapeIndex;

void SetMemorySpace(Shape* shape, int64_t memory_space_color) {
  CHECK(shape->has_layout());
  shape->mutable_layout()->set_memory_space(memory_space_color);
}

bool SetBuffersToMemorySpaceColor(
    const std::vector<InstructionAndShapeIndex>& buffers_to_set_to_host_memory,
    int64_t memory_space_color) {
  bool changed = false;
  for (const auto& instr_and_shape : buffers_to_set_to_host_memory) {
    VLOG(2) << absl::StreamFormat("Setting %s to memory space %d",
                                  instr_and_shape.ToString(),
                                  memory_space_color);
    Shape* shape = ShapeUtil::GetMutableSubshape(
        instr_and_shape.instruction->mutable_shape(),
        instr_and_shape.shape_index);
    CHECK(shape->has_layout()) << "Instruction's shape has no layout: "
                               << instr_and_shape.instruction->ToString();
    SetMemorySpace(ShapeUtil::GetMutableSubshape(
                       instr_and_shape.instruction->mutable_shape(),
                       instr_and_shape.shape_index),
                   memory_space_color);
    changed = true;
  }
  return changed;
}

void PrintTrace(InstructionAndShapeIndex instruction_and_shape_index,
                const absl::flat_hash_map<InstructionAndShapeIndex,
                                          InstructionAndShapeIndex>& previous) {
  std::vector<InstructionAndShapeIndex> trace;
  trace.push_back(instruction_and_shape_index);
  auto it = previous.find(instruction_and_shape_index);
  while (it != previous.end()) {
    trace.push_back(it->second);
    instruction_and_shape_index = it->second;
    it = previous.find(instruction_and_shape_index);
  }
  std::reverse(trace.begin(), trace.end());
  for (const auto& instruction_and_shape_index : trace) {
    VLOG(1) << "  " << instruction_and_shape_index.ToString();
  }
}

}  // namespace

bool HostOffloader::InstructionIsAllowedBetweenMoveToHostAndDus(
    const HloInstruction* instruction) const {
  if (instruction->opcode() == HloOpcode::kReshape) {
    return ShapeUtil::ReshapeIsBitcast(instruction->operand(0)->shape(),
                                       instruction->shape());
  }
  return (instruction->opcode() == HloOpcode::kBitcast ||
          instruction->opcode() == HloOpcode::kCopy);
}

bool HostOffloader::InstructionIsAllowedBetweenDsAndMoveToDevice(
    const HloInstruction* instruction) const {
  if (instruction->opcode() == HloOpcode::kReduce) {
    // TODO(b/333902007): Remove this once trivial reduces no longer appear.
    return ShapeUtil::TrueNumDimensions(instruction->operand(0)->shape()) ==
           ShapeUtil::TrueNumDimensions(instruction->shape());
  }
  if (instruction->opcode() == HloOpcode::kReshape) {
    return ShapeUtil::ReshapeIsBitcast(instruction->operand(0)->shape(),
                                       instruction->shape());
  }
  return instruction->opcode() == HloOpcode::kBitcast ||
         instruction->opcode() == HloOpcode::kCopy;
}

absl::StatusOr<bool> HostOffloader::WalkDownHostMemoryOffloadPaths(
    const InstructionAndShapeIndex& starting_instruction_and_index,
    bool insert_copy_before) {
  VLOG(3) << absl::StreamFormat(
      "Walking down host memory offload paths starting from (%s, %s). Insert "
      "copy before: %v",
      starting_instruction_and_index.instruction->name(),
      starting_instruction_and_index.shape_index.ToString(),
      insert_copy_before);
  bool changed = false;
  absl::flat_hash_set<HloInstruction*> mth_custom_calls_to_remove;
  absl::flat_hash_set<HloInstruction*> slices_to_dynamify;
  absl::flat_hash_set<HloInstruction*> custom_calls_to_insert_copies_before;
  absl::flat_hash_set<HloInstruction*> x64_split_instructions;
  std::vector<InstructionAndShapeIndex> buffers_to_set_to_host_memory;
  // std::vector<HloInstruction*> move_to_host_dynamic_update_slices;
  HloInstruction* starting_instruction =
      starting_instruction_and_index.instruction;
  std::queue<InstructionAndShapeIndex> queue;
  absl::flat_hash_map<InstructionAndShapeIndex, InstructionAndShapeIndex>
      previous;
  queue.push(starting_instruction_and_index);
  while (!queue.empty()) {
    InstructionAndShapeIndex instruction_and_shape_index = queue.front();
    queue.pop();
    HloInstruction* instruction = instruction_and_shape_index.instruction;
    VLOG(4) << absl::StreamFormat("Visiting instruction: %s",
                                  instruction_and_shape_index.ToString());
    bool already_saved_buffer = false;
    bool need_to_wrap_instruction_as_host_compute = false;
    if (instruction->opcode() == HloOpcode::kCustomCall &&
        instruction->custom_call_target() ==
            memory_annotations::kMoveToHostCustomCallTarget) {
      // This MoveToHost custom call is a no-op; save it to remove later.
      already_visited_move_to_host_custom_calls_.insert(instruction);
      mth_custom_calls_to_remove.insert(instruction);
    } else if (instruction->opcode() == HloOpcode::kCustomCall &&
               instruction->custom_call_target() ==
                   memory_annotations::kMoveToDeviceCustomCallTarget) {
      // This MoveToDevice marks the end of this path.
      custom_calls_to_insert_copies_before.insert(instruction);
      continue;
    } else if (instruction->opcode() == HloOpcode::kDynamicUpdateSlice) {
      // Save every DynamicUpdateSlice we see to process after all host memory
      // space propagation is done.
      if (!absl::c_linear_search(dynamic_update_slices_seen_, instruction)) {
        dynamic_update_slices_seen_.push_back(instruction);
      }
      if (instruction == starting_instruction) {
        // This DynamicUpdateSlice's update operand had a MoveToHost annotation.
        if (!absl::c_linear_search(dynamic_update_slices_seen_with_annotation_,
                                   instruction)) {
          dynamic_update_slices_seen_with_annotation_.push_back(instruction);
        }
      }
    } else if (host_offload_utils::IsValidDuringPureMemoryOffload(
                   instruction)) {
      if (instruction->opcode() == HloOpcode::kAsyncStart) {
        // When visiting the parameter, we already set the memory space of the
        // input of the async-start; do not set it now.
        already_saved_buffer = true;
      } else if (instruction->opcode() == HloOpcode::kAsyncDone) {
        // Also set host memory space for the output in the async-start's shape.
        HloInstruction* async_start = instruction->mutable_operand(0);
        buffers_to_set_to_host_memory.emplace_back(async_start, ShapeIndex{1});
      } else if (instruction->opcode() == HloOpcode::kParameter) {
        // When setting the memory space of a parameter, also set the memory
        // space of the call site of the computation with this parameter if that
        // caller is an async-start.
        std::unique_ptr<CallGraph> call_graph =
            CallGraph::Build(instruction->GetModule());
        std::vector<HloInstruction*> callers =
            call_graph->GetComputationCallers(instruction->parent());
        for (HloInstruction* caller : callers) {
          if (caller->opcode() == HloOpcode::kAsyncStart) {
            ShapeIndex tmp_index = instruction_and_shape_index.shape_index;
            tmp_index.push_front(instruction->parameter_number());
            tmp_index.push_front(
                0);  // Index 0 for the inputs of the async-start. The shape of
                     // async-start is ((inputs, ...), output, context).
            buffers_to_set_to_host_memory.emplace_back(caller, tmp_index);
          }
        }
      }
    } else if (instruction->opcode() == HloOpcode::kDynamicSlice) {
      TF_ASSIGN_OR_RETURN(bool is_end_of_offload,
                          SliceLeadsToMoveToDeviceCustomCall(instruction));
      if (is_end_of_offload) {
        // This DynamicSlice is the end of this path of host memory offload.
        continue;
      } else {
        // This is not the end of host memory offload. This is treated as device
        // compute happening on host memory, convert it to host compute.
        need_to_wrap_instruction_as_host_compute = true;
      }
    } else if (instruction->opcode() == HloOpcode::kSlice) {
      TF_ASSIGN_OR_RETURN(bool is_end_of_offload,
                          SliceLeadsToMoveToDeviceCustomCall(instruction));
      if (is_end_of_offload) {
        // This Slice is the end of this path of host memory offload.
        // This Slice should be a DynamicSlice to be able to work with host
        // memory.
        slices_to_dynamify.insert(instruction);
        continue;
      } else {
        // This is not the end of host memory offload. This is treated as device
        // compute happening on host memory, convert it to host compute.
        need_to_wrap_instruction_as_host_compute = true;
      }
    } else {
      // This is some unaccounted for instruction. Since it is unaccounted for,
      // it must be something which is not legal to do with device compute.
      need_to_wrap_instruction_as_host_compute = true;
    }

    // We need to insert copies before X64SplitLow and X64SplitHigh custom
    // calls.
    for (const auto user : instruction->users()) {
      if (user->opcode() == HloOpcode::kCustomCall &&
          (user->custom_call_target() == "X64SplitLow" ||
           user->custom_call_target() == "X64SplitHigh")) {
        x64_split_instructions.insert(user);
      }
    }

    if (need_to_wrap_instruction_as_host_compute) {
      LOG(WARNING) << absl::StreamFormat(
          "Found an instruction (\"%s\") which does device compute in host "
          "memory space. Converting into host compute. This is likely to have "
          "a very slow execution time. If you're using JAX, use device_put() "
          "to move the inputs to the device so that computation happens on the "
          "device.",
          instruction->name());
      host_offload_utils::SetHostComputeFrontendAttribute(*instruction);
    }
    if (!already_saved_buffer) {
      const HloInstruction* instruction =
          instruction_and_shape_index.instruction;
      bool set_as_host_memory = true;
      if (instruction->opcode() == HloOpcode::kDynamicUpdateSlice) {
        // We'll do DUSes later.
        set_as_host_memory = false;
      }

      if (set_as_host_memory) {
        // Save buffer to be set to host memory.
        VLOG(5) << "Saving " << instruction_and_shape_index.ToString()
                << " to be set to host memory.";
        buffers_to_set_to_host_memory.push_back(instruction_and_shape_index);
      }
    }

    // Check if this path ends at the output of the entry computation.
    if (instruction->IsRoot() && instruction->parent()->IsEntryComputation()) {
      const Shape& output_shape = ShapeUtil::GetSubshape(
          instruction->GetModule()->entry_computation_layout().result_shape(),
          instruction_and_shape_index.shape_index);
      CHECK(output_shape.has_layout())
          << "Expecting output shape of entry computation to have a layout.";
      if (output_shape.layout().memory_space() == Layout::kHostMemorySpace) {
        VLOG(2) << absl::StreamFormat(
            "Memory offloaded starting from %s is output streamed",
            starting_instruction_and_index.ToString());
        continue;
      } else {
        if (VLOG_IS_ON(1)) {
          LOG(INFO) << "Instruction trace leading to error:";
          PrintTrace(instruction_and_shape_index, previous);
        }
        return absl::InvalidArgumentError(
            absl::StrFormat("Tensor which is moved to host (starting from %s) "
                            "is returned from the entry computation but the "
                            "layout for this output is not set to host memory.",
                            starting_instruction->name()));
      }
    }
    // Push successors onto the queue to be visited.
    TF_ASSIGN_OR_RETURN(
        const std::vector<InstructionAndShapeIndex> successors,
        host_offload_utils::GetSuccessors(instruction_and_shape_index));
    for (const InstructionAndShapeIndex& successor : successors) {
      if (VLOG_IS_ON(1)) {
        previous.emplace(successor, instruction_and_shape_index);
      }
      const Shape& successor_shape = ShapeUtil::GetSubshape(
          successor.instruction->shape(), successor.shape_index);
      if (successor_shape.has_layout() &&
          successor_shape.layout().memory_space() == Layout::kHostMemorySpace) {
        // When a successor shape already has host memory space, we know that
        // we've visited it, so we can skip adding it to the queue here.
        continue;
      }
      queue.push(successor);
    }
  }

  // Finished walking all host memory paths. Now we'll make all the necessary
  // changes.
  const bool set_buffers_changed = SetBuffersToMemorySpaceColor(
      buffers_to_set_to_host_memory, Layout::kHostMemorySpace);
  changed = changed || set_buffers_changed;

  if (insert_copy_before) {
    const auto predecessors =
        host_offload_utils::GetPredecessors(starting_instruction_and_index);
    CHECK_EQ(predecessors.size(), 1);
    TF_ASSIGN_OR_RETURN(bool inserted_copy,
                        InsertCopyBetween(predecessors.front(),
                                          starting_instruction_and_index));
    changed = changed || inserted_copy;
  }

  // Insert copies to move to device.
  for (HloInstruction* custom_call : custom_calls_to_insert_copies_before) {
    HloInstruction* data_to_copy = custom_call->mutable_operand(0);
    HloInstruction* copy_to_device =
        data_to_copy->parent()->AddInstruction(HloInstruction::CreateUnary(
            data_to_copy->shape(), HloOpcode::kCopy, data_to_copy));
    SetMemorySpace(copy_to_device->mutable_shape(),
                   Layout::kDefaultMemorySpace);
    VLOG(1) << absl::StreamFormat(
        "Inserted copy \"%s\" before custom call \"%s\"",
        copy_to_device->name(), custom_call->name());
    TF_RETURN_IF_ERROR(custom_call->ReplaceAllUsesWith(copy_to_device));
    changed = true;
  }

  if (!x64_split_instructions.empty()) {
    LOG(WARNING) << "64-bit type on device is decomposed into 32-bit types "
                    "very early on so host offloader only sees 32-bit "
                    "types. Thus the current handling of 64-bit type host "
                    "offloading might be sub-optimal";
  }
  for (HloInstruction* x64_split_instruction : x64_split_instructions) {
    HloInstruction* data_to_copy = x64_split_instruction->mutable_operand(0);
    HloInstruction* copy_to_device =
        data_to_copy->parent()->AddInstruction(HloInstruction::CreateUnary(
            data_to_copy->shape(), HloOpcode::kCopy, data_to_copy));
    SetMemorySpace(copy_to_device->mutable_shape(),
                   Layout::kDefaultMemorySpace);
    TF_RETURN_IF_ERROR(
        x64_split_instruction->ReplaceOperandWith(0, copy_to_device));
  }

  // All host memory offloading has been completed. Remove MoveToHost custom
  // calls.
  for (HloInstruction* custom_call : mth_custom_calls_to_remove) {
    VLOG(1) << absl::StreamFormat("Removing MoveToHost custom call \"%s\"",
                                  custom_call->name());
    TF_RETURN_IF_ERROR(
        custom_call->ReplaceAllUsesWith(custom_call->mutable_operand(0)));
    TF_RETURN_IF_ERROR(custom_call->parent()->RemoveInstruction(custom_call));
    changed = true;
  }

  for (HloInstruction* slice : slices_to_dynamify) {
    TF_RETURN_IF_ERROR(DynamifySlice(slice));
    changed = true;
  }

  return changed;
}

absl::StatusOr<bool> HostOffloader::HandleInputStreaming(
    HloComputation* entry_computation) {
  bool changed = false;
  const ComputationLayout& entry_computation_layout =
      entry_computation->parent()->entry_computation_layout();

  for (int i = 0; i < entry_computation_layout.parameter_count(); ++i) {
    TF_RETURN_IF_ERROR(ShapeUtil::ForEachSubshapeWithStatus(
        entry_computation_layout.parameter_shape(i),
        [&](const Shape& subshape, const ShapeIndex& index) {
          if (subshape.has_layout() &&
              subshape.layout().memory_space() == Layout::kHostMemorySpace) {
            HloInstruction* parameter_instruction =
                entry_computation->parameter_instruction(i);
            VLOG(1) << "Host parameter #" << i
                    << " streamed into program with shape: "
                    << subshape.ToString(/*print_layout=*/true) << " at index "
                    << index.ToString();
            TF_ASSIGN_OR_RETURN(
                bool result,
                WalkDownHostMemoryOffloadPaths(
                    InstructionAndShapeIndex(parameter_instruction, index),
                    /*insert_copy_before=*/false));
            changed = changed || result;
          }
          return absl::OkStatus();
        }));
  }
  return changed;
}

absl::StatusOr<bool> HostOffloader::HandleMoveToHostCustomCall(
    HloInstruction* custom_call_instruction) {
  if (already_visited_move_to_host_custom_calls_.contains(
          custom_call_instruction)) {
    return false;
  }
  VLOG(1) << "Offloading \"" << custom_call_instruction->operand(0)->name()
          << "\" to host.";
  TF_ASSIGN_OR_RETURN(
      std::vector<InstructionAndShapeIndex> starting_instruction_and_shapes,
      GetStartingInstructions(custom_call_instruction));
  if (starting_instruction_and_shapes.empty()) {
    // Either:
    //  1. This custom call has no users.
    //  2. It is the root of the entry computation.
    // In the case of 1, there is nothing to do. You could argue that we should
    // still copy the data to the host, as it is side effecting. However, that
    // would be wasteful, so we won't do it. In the case of 2, we'll simply
    // insert a copy to host and replace the root instruction with that.
    if (custom_call_instruction == custom_call_instruction->GetModule()
                                       ->entry_computation()
                                       ->root_instruction()) {
      HloInstruction* data_to_copy =
          custom_call_instruction->mutable_operand(0);
      HloInstruction* copy_to_host =
          data_to_copy->parent()->AddInstruction(HloInstruction::CreateUnary(
              data_to_copy->shape(), HloOpcode::kCopy, data_to_copy));
      SetMemorySpace(copy_to_host->mutable_shape(), Layout::kHostMemorySpace);
      TF_RETURN_IF_ERROR(
          custom_call_instruction->ReplaceAllUsesWith(copy_to_host));
      VLOG(2) << absl::StreamFormat(
          "Custom call \"%s\" is entry computation root. Inserted copy \"%s\" "
          "and replaced root instruction.",
          custom_call_instruction->name(), copy_to_host->name());
    }
  }

  // Walk down the graph from each starting instruction.
  for (const InstructionAndShapeIndex& starting_instruction_and_shape :
       starting_instruction_and_shapes) {
    const bool should_insert_copy_before_instruction =
        starting_instruction_and_shape.instruction->opcode() !=
        HloOpcode::kDynamicUpdateSlice;
    TF_ASSIGN_OR_RETURN(
        bool result,
        WalkDownHostMemoryOffloadPaths(starting_instruction_and_shape,
                                       should_insert_copy_before_instruction));
    (void)result;  // This function *will* change the HloModule. We don't care
                   // if WalkDownHostMemoryOffloadPaths changed it or not.
  }

  already_visited_move_to_host_custom_calls_.insert(custom_call_instruction);

  // Remove custom call.
  VLOG(2) << absl::StreamFormat("Removing MoveToHost custom call \"%s\"",
                                custom_call_instruction->name());
  TF_RETURN_IF_ERROR(custom_call_instruction->ReplaceAllUsesWith(
      custom_call_instruction->mutable_operand(0)));
  TF_RETURN_IF_ERROR(custom_call_instruction->parent()->RemoveInstruction(
      custom_call_instruction));
  return true;
}

absl::StatusOr<bool> HostOffloader::HandleMoveToDeviceCustomCall(
    HloInstruction* custom_call_instruction) {
  VLOG(2) << absl::StreamFormat("Removing MoveToDevice custom call \"%s\"",
                                custom_call_instruction->name());
  TF_RETURN_IF_ERROR(custom_call_instruction->ReplaceAllUsesWith(
      custom_call_instruction->mutable_operand(0)));
  TF_RETURN_IF_ERROR(custom_call_instruction->parent()->RemoveInstruction(
      custom_call_instruction));
  move_to_device_custom_calls_to_remove_.insert(custom_call_instruction);
  return true;
}

absl::StatusOr<bool> HostOffloader::InsertCopyBetween(
    const InstructionAndShapeIndex& before_instruction_and_index,
    const InstructionAndShapeIndex& after_instruction_and_index) {
  VLOG(3) << "InsertCopyBetween: " << before_instruction_and_index.ToString()
          << " and " << after_instruction_and_index.ToString();
  bool changed = false;
  HloInstruction* after_instruction = after_instruction_and_index.instruction;

  // Get a list of instructions to insert copies before. Normally, this is just
  // `after_instruction_and_index.instruction`, however, if this instruction is
  // a parameter, then we need to insert the copies before the call sites.
  std::vector<InstructionAndShapeIndex> instructions_to_insert_copies_before;
  if (after_instruction->opcode() == HloOpcode::kParameter) {
    // To insert a copy between an instruction and a parameter means we actually
    // want to insert a copy between the instruction and the call site of the
    // computation with this parameter.
    std::unique_ptr<CallGraph> call_graph =
        CallGraph::Build(after_instruction->GetModule());
    auto callers =
        call_graph->GetComputationCallers(after_instruction->parent());
    for (HloInstruction* caller : callers) {
      const auto indices =
          caller->OperandIndices(before_instruction_and_index.instruction);
      for (int64_t index : indices) {
        // We really use operand index as shape index here for the next step's
        // ReplaceOperandWith().
        instructions_to_insert_copies_before.push_back(
            InstructionAndShapeIndex{caller, {index}});
      }
    }
  } else {
    // Instruction is not a parameter, replacement is straightforward.
    instructions_to_insert_copies_before.push_back(after_instruction_and_index);
  }

  // Insert a copy before each of the above instructions.
  for (const InstructionAndShapeIndex& instruction_and_index :
       instructions_to_insert_copies_before) {
    if (already_inserted_copy_before_.find(instruction_and_index) ==
        already_inserted_copy_before_.end()) {
      HloInstruction* data_to_copy = before_instruction_and_index.instruction;
      HloInstruction* copy_to_host;
      auto it = copies_created_after_.find(data_to_copy);
      if (it == copies_created_after_.end()) {
        // Don't have a copy yet; create it.
        copy_to_host =
            data_to_copy->parent()->AddInstruction(HloInstruction::CreateUnary(
                data_to_copy->shape(), HloOpcode::kCopy, data_to_copy));
        SetMemorySpace(copy_to_host->mutable_shape(), Layout::kHostMemorySpace);
        copies_created_after_[data_to_copy] = copy_to_host;
      } else {
        // We already have a copy which feeds into this instruction.
        copy_to_host = it->second;
      }
      const int64_t operand_index =
          instruction_and_index.shape_index.empty()
              ? 0
              : instruction_and_index.shape_index.front();
      TF_RETURN_IF_ERROR(instruction_and_index.instruction->ReplaceOperandWith(
          operand_index, copy_to_host));
      VLOG(2) << absl::StreamFormat(
          "Inserted copy \"%s\" between \"%s\" and \"%s\"",
          copy_to_host->name(), before_instruction_and_index.ToString(),
          after_instruction_and_index.ToString());
      already_inserted_copy_before_.insert(instruction_and_index);
      changed = true;
    }
  }
  return changed;
}

absl::StatusOr<std::vector<InstructionAndShapeIndex>>
HostOffloader::GetStartingInstructions(
    HloInstruction* custom_call_instruction) {
  // We want to offload the single operand of this custom call to the host.
  // For each user, it either:
  // 1. Feeds into a DynamicUpdateSlice.
  // 2. Does "normal" memory offloading.
  std::vector<InstructionAndShapeIndex> result;
  std::queue<InstructionAndShapeIndex> queue;
  TF_ASSIGN_OR_RETURN(
      const std::vector<InstructionAndShapeIndex> successors_of_custom_call,
      host_offload_utils::GetSuccessors(
          InstructionAndShapeIndex(custom_call_instruction)));
  for (const InstructionAndShapeIndex& successor : successors_of_custom_call) {
    queue.push(successor);
  }
  while (!queue.empty()) {
    InstructionAndShapeIndex instruction_and_shape = queue.front();
    queue.pop();
    HloInstruction* current_instruction = instruction_and_shape.instruction;
    if (current_instruction->opcode() == HloOpcode::kDynamicUpdateSlice) {
      // Found a DynamicUpdateSlice.
      result.push_back(instruction_and_shape);
      continue;
    } else if (!InstructionIsAllowedBetweenMoveToHostAndDus(
                   current_instruction)) {
      // Found the start of "normal" memory offloading.
      result.push_back(instruction_and_shape);
      continue;
    } else {
      // Is a logical bitcast/reshape, we won't offload this yet.
    }
    TF_ASSIGN_OR_RETURN(
        const std::vector<InstructionAndShapeIndex> successors,
        host_offload_utils::GetSuccessors(instruction_and_shape));
    for (const InstructionAndShapeIndex& successor : successors) {
      queue.push(successor);
    }
  }
  return result;
}

absl::StatusOr<bool> HostOffloader::SliceLeadsToMoveToDeviceCustomCall(
    HloInstruction* slice) {
  // Every host-to-device DynamicSlice/Slice must be followed by a MoveToDevice
  // custom call. This function verifiest that.
  CHECK(slice->opcode() == HloOpcode::kDynamicSlice ||
        slice->opcode() == HloOpcode::kSlice)
      << "This function must only be called with a slice or dynamic slice.";
  std::queue<InstructionAndShapeIndex> queue;
  TF_ASSIGN_OR_RETURN(
      const std::vector<InstructionAndShapeIndex> successors_of_slice,
      host_offload_utils::GetSuccessors(InstructionAndShapeIndex(slice)));
  for (const InstructionAndShapeIndex& successor : successors_of_slice) {
    queue.push(successor);
  }
  while (!queue.empty()) {
    InstructionAndShapeIndex instruction_and_shape = queue.front();
    queue.pop();
    HloInstruction* current_instruction = instruction_and_shape.instruction;
    if (current_instruction->opcode() == HloOpcode::kCustomCall &&
        current_instruction->custom_call_target() ==
            memory_annotations::kMoveToDeviceCustomCallTarget) {
      // This path ended with the MoveToDevice custom call. This path is good.
      continue;
    }
    if (!InstructionIsAllowedBetweenDsAndMoveToDevice(current_instruction)) {
      // We were expecting to find a MoveToDevice custom call here, marking the
      // end of host memory offloading, but we did not.
      LOG(WARNING) << absl::StreamFormat(
          "Encountered %s on tensor which is in host memory. %s does not move "
          "the tensor back to device. %s will be converted into host compute.",
          HloOpcodeString(slice->opcode()), slice->name(), slice->name());
      return false;
    }
    TF_ASSIGN_OR_RETURN(
        const std::vector<InstructionAndShapeIndex> successors,
        host_offload_utils::GetSuccessors(instruction_and_shape));
    for (const InstructionAndShapeIndex& successor : successors) {
      queue.push(successor);
    }
  }
  return true;
}

absl::Status HostOffloader::CreateAllocateBufferForDynamicUpdateSlice(
    HloInstruction* dynamic_update_slice) {
  if (dynamic_update_slices_already_allocated_.find(dynamic_update_slice) !=
      dynamic_update_slices_already_allocated_.end()) {
    // Already added an AllocateBuffer for this DynamicUpdateSlice.
    return absl::OkStatus();
  }
  VLOG(2) << absl::StreamFormat(
      "Creating a AllocateBuffer in host memory space for \"%s\"",
      dynamic_update_slice->name());
  // Walk the graph up. We expect to find a broadcast. Also, while walking up
  // the graph, set host memory space on everything between the AllocateBuffer
  // and the DynamicUpdateSlice.
  std::queue<InstructionAndShapeIndex> queue;
  queue.push(InstructionAndShapeIndex(dynamic_update_slice));
  bool found_broadcast = false;
  while (!queue.empty()) {
    InstructionAndShapeIndex instruction_and_shape = queue.front();
    queue.pop();
    VLOG(2) << absl::StreamFormat("Setting %s to have host memory space",
                                  instruction_and_shape.ToString());
    SetMemorySpace(ShapeUtil::GetMutableSubshape(
                       instruction_and_shape.instruction->mutable_shape(),
                       instruction_and_shape.shape_index),
                   Layout::kHostMemorySpace);
    HloInstruction* instruction = instruction_and_shape.instruction;
    if (instruction->opcode() == HloOpcode::kParameter) {
      // If this is a parameter of a while_body, we also need to find the
      // matching parameter in the while_condition and set the memory spaces
      // there.
      std::unique_ptr<CallGraph> call_graph =
          CallGraph::Build(instruction->GetModule());
      const std::vector<HloInstruction*> callers =
          call_graph->GetComputationCallers(instruction->parent());
      for (HloInstruction* caller : callers) {
        if (caller->opcode() == HloOpcode::kWhile) {
          // This parameter belongs to a while.
          CHECK(caller->while_body() == instruction->parent())
              << "We assume that we're starting from the while body";
          HloComputation* while_condition_computation =
              caller->while_condition();
          CHECK(while_condition_computation->num_parameters() == 1)
              << "Expecting While to have just 1 parameter";
          HloInstruction* while_condition_parameter =
              while_condition_computation->parameter_instruction(0);
          VLOG(2) << absl::StreamFormat("Setting %s to have host memory space",
                                        while_condition_parameter->name());
          SetMemorySpace(ShapeUtil::GetMutableSubshape(
                             while_condition_parameter->mutable_shape(),
                             instruction_and_shape.shape_index),
                         Layout::kHostMemorySpace);
          // Walk further down the graph and set the memory spaces of all uses
          // too. This includes verifying that no compute is done on the buffer.
          // Another, better way, to do this, is to walk down the graph starting
          // from the newly created AllocateBuffer and set everything visited as
          // host memory space.
          std::queue<InstructionAndShapeIndex> nested_queue;
          nested_queue.push(InstructionAndShapeIndex(
              while_condition_parameter, instruction_and_shape.shape_index));
          while (!nested_queue.empty()) {
            InstructionAndShapeIndex nested_instruction_and_shape =
                nested_queue.front();
            nested_queue.pop();
            if (!host_offload_utils::IsValidDuringPureMemoryOffload(
                    nested_instruction_and_shape.instruction)) {
              return absl::InvalidArgumentError(absl::StrFormat(
                  "Tensor which is moved to host is used by an invalid "
                  "instruction (\"%s\") during while condition body.",
                  nested_instruction_and_shape.instruction->name()));
            }
            SetMemorySpace(
                ShapeUtil::GetMutableSubshape(
                    nested_instruction_and_shape.instruction->mutable_shape(),
                    nested_instruction_and_shape.shape_index),
                Layout::kHostMemorySpace);
            TF_ASSIGN_OR_RETURN(
                const std::vector<InstructionAndShapeIndex> successors,
                host_offload_utils::GetSuccessors(
                    nested_instruction_and_shape));
            for (const InstructionAndShapeIndex& successor : successors) {
              nested_queue.push(successor);
            }
          }
        }
      }
    } else if (instruction->opcode() == HloOpcode::kDynamicUpdateSlice) {
      // The AllocateBuffer that we're about to create will suffice for every
      // DynamicUpdateSlice we pass through as we walk up the graph.
      dynamic_update_slices_already_allocated_.insert(instruction);
    } else if (instruction->IsCustomCall("AllocateBuffer")) {
      VLOG(2) << absl::StreamFormat(
          "DynamicUpdateSlice \"%s\" already writes into an AllocateBuffer "
          "\"%s\"",
          dynamic_update_slice->name(), instruction->name());
      return absl::OkStatus();
    }
    const std::vector<InstructionAndShapeIndex> predecessors =
        host_offload_utils::GetPredecessors(instruction_and_shape);
    for (const InstructionAndShapeIndex& predecessor : predecessors) {
      HloInstruction* predecessor_instruction = predecessor.instruction;
      if (predecessor_instruction->opcode() == HloOpcode::kBroadcast) {
        // Found a broadcast.
        found_broadcast = true;
        HloInstruction* broadcast_user = instruction_and_shape.instruction;
        HloInstruction* allocate_buffer =
            predecessor_instruction->parent()->AddInstruction(
                HloInstruction::CreateCustomCall(
                    predecessor_instruction->shape(), {}, "AllocateBuffer"));
        SetMemorySpace(allocate_buffer->mutable_shape(),
                       Layout::kHostMemorySpace);
        VLOG(1) << absl::StreamFormat(
            "Created new AllocateBuffer instruction \"%s\" to replace "
            "broadcast \"%s\"'s use at index %s in user \"%s\"",
            allocate_buffer->ToString(), predecessor_instruction->name(),
            instruction_and_shape.shape_index.ToString(),
            broadcast_user->name());
        if (instruction_and_shape.shape_index.size() == 1) {
          // Have a shape index, this broadcast must be going into a tuple
          // (because the shape index is meant to be a tuple index, not a use
          // index). Use only the index from which we arrived here, as any other
          // index might not be expecting host memory.
          CHECK_EQ(instruction->opcode(), HloOpcode::kTuple)
              << "Expecting a tuple when shape index has ndim>0";
          TF_RETURN_IF_ERROR(broadcast_user->ReplaceOperandWith(
              instruction_and_shape.shape_index[0], allocate_buffer));
        } else {
          // Any shape index larger than 1 would mean that the broadcast
          // produces a tuple, which is not possible.
          CHECK_EQ(instruction_and_shape.shape_index.size(), 0)
              << "Only other supported shape index ndim is 0";
          // Ideally, we'd like to know via which index we arrived here, but we
          // do not. We'll look up at which indices this broadcast is used.
          const auto operand_indices =
              broadcast_user->OperandIndices(predecessor_instruction);
          // 0 uses would be a hard error, as that would mean there is a bug in
          // the GetPredecessors function. If there are more than 1 uses, we do
          // not know via which use we arrived here and setting all uses as host
          // memory space could be incorrect.
          CHECK_EQ(operand_indices.size(), 1)
              << "Only a single use it currently supported";
          TF_RETURN_IF_ERROR(broadcast_user->ReplaceOperandWith(
              operand_indices[0], allocate_buffer));
        }
        if (predecessor_instruction->user_count() == 0) {
          // No remaining users. Remove the broadcast.
          VLOG(3) << absl::StreamFormat(
              "Broadcast \"%s\" has no remaining users; removing.",
              predecessor_instruction->name());
          TF_RETURN_IF_ERROR(
              predecessor_instruction->parent()->RemoveInstruction(
                  predecessor_instruction));
        }
      } else {
        queue.push(predecessor);
      }
    }
  }
  if (!found_broadcast) {
    return absl::InvalidArgumentError(
        absl::StrFormat("DynamicUpdateSlice \"%s\"'s first operand is not the "
                        "result of a broadcast.",
                        dynamic_update_slice->name()));
  }
  return absl::OkStatus();
}

absl::Status HostOffloader::DynamifySlice(HloInstruction* slice) {
  std::vector<HloInstruction*> start_constants;
  for (int64_t start : slice->slice_starts()) {
    HloInstruction* constant = slice->parent()->AddInstruction(
        HloInstruction::CreateConstant(LiteralUtil::CreateR0<int32_t>(start)));
    start_constants.push_back(constant);
  }
  std::vector<int64_t> slice_sizes;
  slice_sizes.reserve(slice->slice_limits().size());
  for (int i = 0; i < slice->slice_limits().size(); ++i) {
    slice_sizes.push_back(slice->slice_limits()[i] - slice->slice_starts()[i]);
  }
  HloInstruction* new_ds =
      slice->parent()->AddInstruction(HloInstruction::CreateDynamicSlice(
          slice->shape(), slice->mutable_operand(0), start_constants,
          slice_sizes));
  TF_RETURN_IF_ERROR(slice->ReplaceAllUsesWith(new_ds));
  VLOG(2) << absl::StreamFormat(
      "Changed slice \"%s\" into dynamic slice \"%s\"", slice->name(),
      new_ds->name());
  TF_RETURN_IF_ERROR(slice->parent()->RemoveInstruction(slice));
  return absl::OkStatus();
}

absl::StatusOr<bool> HostOffloader::ApplySchedulingFix(
    HloModule* module,
    const absl::flat_hash_set<absl::string_view>& execution_threads) {
  bool changed = false;
  TF_ASSIGN_OR_RETURN(std::unique_ptr<HloAliasAnalysis> alias_analysis,
                      HloAliasAnalysis::Run(module, alias_info_));
  auto uses_parameter_buffer = [&](HloInstruction* hlo) {
    for (const HloBuffer* buffer : alias_analysis->ComputeBuffersAt(hlo)) {
      for (const HloValue* value : buffer->values()) {
        for (const HloPosition& pos : value->positions()) {
          if (absl::c_linear_search(hlo->parent()->parameter_instructions(),
                                    pos.instruction)) {
            return true;
          }
        }
      }
    }
    return false;
  };
  for (HloComputation* computation :
       module->MakeComputationPostOrder(execution_threads)) {
    if (computation == computation->parent()->entry_computation()) {
      continue;
    }
    for (HloInstruction* instruction :
         computation->MakeInstructionPostOrder()) {
      if (instruction->opcode() != HloOpcode::kDynamicUpdateSlice) {
        continue;
      }
      if (instruction->shape().layout().memory_space() !=
          Layout::kHostMemorySpace) {
        continue;
      }
      // Replace DynamicUpdateSlice's 1st operand with a copy in case it
      // used parameter buffer directly because in case of aliasing with loop
      // parameters control dependencies can mess with scheduling.
      HloInstruction* operand = instruction->mutable_operand(1);
      if (uses_parameter_buffer(operand)) {
        HloInstruction* copy =
            instruction->parent()->AddInstruction(HloInstruction::CreateUnary(
                operand->shape(), HloOpcode::kCopy, operand));
        VLOG(5) << "Added copy " << std::quoted(copy->name())
                << " for DynamicUpdateSlice " << instruction->name()
                << "'s 1st operand " << operand->name();
        TF_RETURN_IF_ERROR(instruction->ReplaceOperandWith(1, copy));
        changed = true;
      }
    }
  }
  return changed;
}

namespace {

absl::Status ValidateAsyncComputationStructure(HloComputation* computation) {
  for (HloInstruction* instr : computation->instructions()) {
    if (instr->opcode() == HloOpcode::kParameter || instr->IsRoot()) {
      continue;
    }

    return absl::InternalError(
        absl::StrCat("Unexpected instruction found in async computation: ",
                     instr->ToString()));
  }

  return absl::OkStatus();
}

// Updates memory space for all outputs of the host offloaded computation
// (associated with `call_start`) that are ONLY used on host. NOTE: We also
// remove redundant copies to host, if any.
absl::StatusOr<bool> UpdateMemorySpaceForHostOffloadedOutputs(
    HloInstruction* call_start,
    ShapeTree<std::vector<InstructionAndShapeIndex>> host_instrs_tree) {
  // Keep track of MoveToHost instructions that need to be removed.
  std::vector<InstructionAndShapeIndex> to_replace;

  HloComputation* called_computation = call_start->async_wrapped_computation();
  TF_RETURN_IF_ERROR(ValidateAsyncComputationStructure(called_computation));
  HloInstruction* root = called_computation->root_instruction();
  Shape* root_shape = root->mutable_shape();

  host_instrs_tree.ForEachMutableElement(
      [&](ShapeIndex output_index,
          std::vector<InstructionAndShapeIndex>* instruction_and_shape_indexes)
          -> void {
        for (InstructionAndShapeIndex& instr_and_shape :
             *instruction_and_shape_indexes) {
          // If instruction is MoveToHost, we will replace usage.
          if (instr_and_shape.instruction->IsCustomCall(
                  memory_annotations::kMoveToHostCustomCallTarget)) {
            to_replace.push_back(instr_and_shape);
            continue;
          }

          SetMemorySpace(ShapeUtil::GetMutableSubshape(
                             instr_and_shape.instruction->mutable_shape(),
                             instr_and_shape.shape_index),
                         Layout::kHostMemorySpace);
        }

        if (!instruction_and_shape_indexes->empty()) {
          // Update the memory space for the output of the computation call
          // itself.
          SetMemorySpace(
              ShapeUtil::GetMutableSubshape(root_shape, output_index),
              Layout::kHostMemorySpace);
        }
      });
  bool modified = false;
  // Remove MoveToHost usage.
  for (InstructionAndShapeIndex& instr_and_shape : to_replace) {
    modified = true;
    HloInstruction* pred = instr_and_shape.instruction->mutable_operand(0);
    TF_RETURN_IF_ERROR(instr_and_shape.instruction->ReplaceAllUsesWith(pred));
  }

  return modified;
}

// Additional checks (does not run IsValidDuringPureMemoryOffload) to determine
// if the respective tensor can be on host.
bool ExtraCheckForValidUsageOnHostForHostOffloadedOutputs(
    const Shape& entry_computation_shape,
    InstructionAndShapeIndex& instruction_and_shape_index) {
  HloInstruction* instruction = instruction_and_shape_index.instruction;
  ShapeIndex& shape_index = instruction_and_shape_index.shape_index;

  // We respect entry computation layout. So for the cases where the
  // outputs are not expected on host, we bail.
  if (instruction->IsRoot() && instruction->parent()->IsEntryComputation()) {
    if (ShapeUtil::GetSubshape(entry_computation_shape, shape_index)
            .layout()
            .memory_space() != Layout::kHostMemorySpace) {
      return false;
    }
  }

  // For custom calls, we conservatively only accept MoveToHost.
  // For MoveToDevice, this could be re-considered, or done as part of a
  // generic redundant copies removal.
  if (instruction->opcode() == HloOpcode::kCustomCall &&
      instruction->custom_call_target() !=
          memory_annotations::kMoveToHostCustomCallTarget) {
    return false;
  }

  // TODO(b/347101407): To also consider host async computations, as we
  // extend GetSuccessors to properly treat it.
  if (instruction->opcode() == HloOpcode::kAsyncStart ||
      instruction->opcode() == HloOpcode::kAsyncDone) {
    return false;
  }

  return true;
}

bool RemoveHostMemorySpaceFromAllShapes(HloModule* module) {
  bool changed = false;
  for (HloComputation* computation : module->computations()) {
    for (HloInstruction* instruction : computation->instructions()) {
      ShapeUtil::ForEachMutableLeafShape(
          instruction->mutable_shape(),
          [&](Shape* subshape, const ShapeIndex& index) {
            if (subshape->has_layout() &&
                subshape->layout().memory_space() == Layout::kHostMemorySpace) {
              subshape->mutable_layout()->set_memory_space(
                  Layout::kDefaultMemorySpace);
              changed = true;
            }
          });
    }
  }
  return changed;
}

}  // namespace

absl::StatusOr<bool> HostOffloader::HandleRedundantCopiesBackToHost(
    const HloModule* module, HloInstruction* instruction) {
  HloAsyncInstruction* call_start = Cast<HloAsyncInstruction>(instruction);

  CHECK_EQ(call_start->users().size(), 1);
  HloInstruction* call_done = call_start->users()[0];

  const Shape& entry_computation_shape =
      module->entry_computation_layout().result_layout().shape();

  // We collect all usages per output index, stopping at any non host
  // instruction.
  Shape* done_shape = call_done->mutable_shape();
  ShapeTree<std::vector<InstructionAndShapeIndex>> host_instrs_tree(done_shape);

  TF_RETURN_IF_ERROR(ShapeUtil::ForEachMutableLeafShapeWithStatus(
      done_shape, [&](Shape* subshape, const ShapeIndex& output_shape_index) {
        std::queue<InstructionAndShapeIndex> queue;
        queue.push(InstructionAndShapeIndex(call_done, output_shape_index));

        // async-start packs the (inputs, outputs, context) in a tuple.
        constexpr int64_t kShapeTupleOutputIndexInAsyncStart = 1;
        std::vector<int32_t> start_shape_index_vec;
        start_shape_index_vec.push_back(kShapeTupleOutputIndexInAsyncStart);
        start_shape_index_vec.insert(start_shape_index_vec.end(),
                                     output_shape_index.begin(),
                                     output_shape_index.end());
        ShapeIndex start_shape_index = {start_shape_index_vec.begin(),
                                        start_shape_index_vec.end()};

        // TODO(b/347101407): Start from async-start and trace through the
        // computation as well in GetSuccessors instead of having to manually
        // add async-done and update the async computation separately.
        host_instrs_tree.mutable_element(output_shape_index)
            ->push_back(
                InstructionAndShapeIndex(call_start, start_shape_index));
        host_instrs_tree.mutable_element(output_shape_index)
            ->push_back(
                InstructionAndShapeIndex(call_done, output_shape_index));

        bool host_only = true;
        // Keep track if the output of the host offloading computation is also
        // an output of the entry computation. Temporaries are conservatively
        // kept on HBM.
        //
        // TODO(b/347101407): Better use AliasAnalysis here to trace host
        // compute outputs to entry compute outputs instead. NOTE: The current
        // algorithm only tracks accepted host offloading operations which
        // operate on the same tensor.
        bool entry_compute_output = false;

        while (!queue.empty() && host_only) {
          InstructionAndShapeIndex instruction_and_shape_index = queue.front();
          queue.pop();

          // TODO(b/347101407): GetSuccessors only follows parameters that alias
          // in async computations. In the cases where it does not, the async
          // computation (start & done) are not returned as we stop before going
          // through the host computation. For now, since we bail if outputs of
          // host computations flow into another host computation, check outside
          // of GetSuccessors and if we do have async-starts successors, bail.
          for (HloInstruction* user :
               instruction_and_shape_index.instruction->users()) {
            if (user->opcode() == HloOpcode::kAsyncStart) {
              host_only = false;
              break;
            }
          }

          TF_ASSIGN_OR_RETURN(
              std::vector<InstructionAndShapeIndex> successors,
              host_offload_utils::GetSuccessors(InstructionAndShapeIndex(
                  instruction_and_shape_index.instruction,
                  instruction_and_shape_index.shape_index)));

          // Check if any of the successors needs to be on device.
          for (InstructionAndShapeIndex& successor : successors) {
            if (!host_offload_utils::IsValidDuringPureMemoryOffload(
                    successor.instruction) ||
                !ExtraCheckForValidUsageOnHostForHostOffloadedOutputs(
                    entry_computation_shape, successor)) {
              host_only = false;
              break;
            }

            if (successor.instruction->IsRoot() &&
                successor.instruction->parent()->IsEntryComputation()) {
              entry_compute_output = true;
            }

            queue.push(successor);
            host_instrs_tree.mutable_element(output_shape_index)
                ->push_back(successor);
          }
        }

        if (!host_only || !entry_compute_output) {
          host_instrs_tree.mutable_element(output_shape_index)->clear();
        }

        return absl::OkStatus();
      }));

  // Update memory space for the host_offloading outputs that never get used on
  // device.
  return UpdateMemorySpaceForHostOffloadedOutputs(call_start, host_instrs_tree);
}

absl::StatusOr<bool> HostOffloader::ProcessNextMoveToHostInstr(
    HloComputation* computation) {
  for (HloInstruction* instruction : computation->MakeInstructionPostOrder()) {
    if (instruction->IsCustomCall(
            memory_annotations::kMoveToHostCustomCallTarget)) {
      TF_ASSIGN_OR_RETURN(bool removed_move_to_host,
                          HandleMoveToHostCustomCall(instruction));
      if (removed_move_to_host) {
        return true;
      }
    }

    if (instruction->has_called_computations()) {
      for (HloComputation* called_comp : instruction->called_computations()) {
        TF_ASSIGN_OR_RETURN(bool removed_move_to_host,
                            ProcessNextMoveToHostInstr(called_comp));
        if (removed_move_to_host) {
          return true;
        }
      }
    }
  }
  return false;
}

absl::StatusOr<bool> HostOffloader::HandleDynamicUpdateSlices() {
  bool changed = false;
  for (HloInstruction* dus : dynamic_update_slices_seen_) {
    // Look at the memory spaces of the operand and update. These should have
    // already been updated by host memory space propagation. Maybe update this
    // DynamicUpdateSlice depending on what memory space they are and whether or
    // not the update had a MoveToHost annotation.
    const int64_t operand_memory_space =
        dus->operand(0)->shape().layout().memory_space();
    const int64_t update_memory_space =
        dus->operand(1)->shape().layout().memory_space();
    const bool host_to_host = update_memory_space == Layout::kHostMemorySpace &&
                              operand_memory_space == Layout::kHostMemorySpace;
    const bool host_to_device =
        update_memory_space == Layout::kHostMemorySpace &&
        operand_memory_space == Layout::kDefaultMemorySpace;
    const bool device_to_host =
        update_memory_space == Layout::kDefaultMemorySpace &&
        operand_memory_space == Layout::kHostMemorySpace;
    const bool device_to_device =
        update_memory_space == Layout::kDefaultMemorySpace &&
        operand_memory_space == Layout::kDefaultMemorySpace;
    if (host_to_device) {
      // This is only supported via host compute.
      host_offload_utils::SetHostComputeFrontendAttribute(*dus);
      changed = true;
    } else if (host_to_host) {
      // Host to host. Execute as host compute. Also set as host memory space.
      host_offload_utils::SetHostComputeFrontendAttribute(*dus);
      SetMemorySpace(dus->mutable_shape(), Layout::kHostMemorySpace);
      changed = true;
    } else if (device_to_host) {
      // Device to host.
      SetMemorySpace(dus->mutable_shape(), Layout::kHostMemorySpace);
      changed = true;
    } else if (device_to_device) {
      // Device to device.
      if (absl::c_linear_search(dynamic_update_slices_seen_with_annotation_,
                                dus)) {
        // This DynamicUpdateSlice is used as a pure memory offload. Create a
        // host AllocateBuffer instruction which this DynamicUpdateSlice will
        // update-slice into.
        TF_RETURN_IF_ERROR(CreateAllocateBufferForDynamicUpdateSlice(dus));
        changed = true;
      }
    }
  }
  return changed;
}

absl::StatusOr<bool> HostOffloader::Run(
    HloModule* module,
    const absl::flat_hash_set<absl::string_view>& execution_threads) {
  // Start by removing all host memory space from all shapes. Host memory space
  // might have been set by other passes, however, this pass is the one which is
  // soley responsible for the propagation of host memory space throughout the
  // entire program.
  bool changed = RemoveHostMemorySpaceFromAllShapes(module);

  // Remove redundant copies to and from host (conservatively) starting
  // from the outputs of the host offloaded computations. Iterate over all
  // instructions and look for XLA host offload annotations.
  bool changed_in_loop;
  for (HloComputation* computation :
       module->MakeNonfusionComputations(execution_threads)) {
    for (HloInstruction* instruction : computation->instructions()) {
      if (host_offload_utils::IsHostAsyncStart(instruction)) {
        TF_ASSIGN_OR_RETURN(changed_in_loop, HandleRedundantCopiesBackToHost(
                                                 module, instruction));
        changed = changed || changed_in_loop;
      }
    }
  }

  TF_ASSIGN_OR_RETURN(const bool input_streaming_changed_module,
                      HandleInputStreaming(module->entry_computation()));
  changed = changed || input_streaming_changed_module;

  // Since we're modifying the graph as we iterate over it, any time we change
  // it, we need to re-run the loop.
  do {
    changed_in_loop = false;
    // Iterate over the computations in the order that they are executed. This
    // ensures we process "MoveToHost" instructions that are at the beginning of
    // a host memory offload instruction chain.
    TF_ASSIGN_OR_RETURN(changed_in_loop, ProcessNextMoveToHostInstr(
                                             module->entry_computation()));
    if (changed_in_loop) {
      changed = true;
    }
  } while (changed_in_loop);

  // For other ops, we can immediately know whether or not they need to be
  // converted to host compute. DynamicUpdateSlices are different because they
  // have multiple operands. Only after finishing all host memory space
  // propagation can we know what to do with the DynamicUpdateSlice.
  TF_ASSIGN_OR_RETURN(bool any_dus_changed, HandleDynamicUpdateSlices());
  changed = changed || any_dus_changed;

  // Remove all MoveToDevice custom calls.
  for (HloComputation* computation :
       module->MakeComputationPostOrder(execution_threads)) {
    for (HloInstruction* instruction :
         computation->MakeInstructionPostOrder()) {
      if (instruction->IsCustomCall(
              memory_annotations::kMoveToDeviceCustomCallTarget)) {
        TF_ASSIGN_OR_RETURN(bool result,
                            HandleMoveToDeviceCustomCall(instruction));
        changed = changed || result;
      }
    }
  }

  TF_ASSIGN_OR_RETURN(bool applied_scheduling_fix,
                      ApplySchedulingFix(module, execution_threads));
  changed = changed || applied_scheduling_fix;

  // Finally, run CSE to do a little cleanup.
  HloCSE cse(/*is_layout_sensitive=*/true);
  TF_ASSIGN_OR_RETURN(bool cse_changed, cse.Run(module, execution_threads));
  changed = changed || cse_changed;

  return changed;
}

}  // namespace xla
