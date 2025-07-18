/* Copyright 2017 The OpenXLA Authors.

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

#include "xla/service/hlo_graph_dumper.h"

#include <string>

#include "absl/container/flat_hash_map.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "xla/hlo/ir/hlo_computation.h"
#include "xla/hlo/ir/hlo_instruction.h"
#include "xla/hlo/ir/hlo_module.h"
#include "xla/hlo/ir/hlo_opcode.h"
#include "xla/hlo/testlib/hlo_hardware_independent_test_base.h"
#include "xla/hlo/testlib/test.h"
#include "xla/literal_util.h"
#include "xla/tsl/platform/statusor.h"
#include "xla/xla.pb.h"

namespace xla {
namespace {

using absl::StrCat;
using ::testing::HasSubstr;

using HloGraphDumperTest = HloHardwareIndependentTestBase;

std::string TestName() {
  return ::testing::UnitTest::GetInstance()->current_test_info()->name();
}

TEST_F(HloGraphDumperTest, NestedFusion) {
  HloComputation::Builder b("b");

  // Build param0 + param1 + param2 + param3 + param4.
  auto shape = ShapeUtil::MakeShape(F32, {10, 100});
  std::vector<HloInstruction*> params;
  for (int i = 0; i <= 4; ++i) {
    params.push_back(b.AddInstruction(
        HloInstruction::CreateParameter(i, shape, StrCat("param", i))));
  }
  std::vector<HloInstruction*> sums;
  sums.push_back(b.AddInstruction(HloInstruction::CreateBinary(
      shape, HloOpcode::kAdd, params[0], params[1])));
  for (int i = 0; i <= 2; ++i) {
    sums.push_back(b.AddInstruction(HloInstruction::CreateBinary(
        shape, HloOpcode::kAdd, sums[i], params[i + 2])));
  }
  HloModuleConfig config;
  HloModule m(TestName(), config);
  m.AddEntryComputation(b.Build());
  HloComputation* root_computation = m.entry_computation();

  // Fuse into fusion(param0 + param1 + param2 + param3 + param4).
  auto* outer_fusion = root_computation->CreateFusionInstruction(
      {sums[3], sums[2], sums[1], sums[0]}, HloInstruction::FusionKind::kLoop);

  // Fusing invalidates the pointers in sums -- the instructions are cloned when
  // they're moved to the new computation.  Get the updated pointers to sums.
  std::vector<HloInstruction*> fused_sums;
  for (auto* instr : outer_fusion->fused_instructions_computation()
                         ->MakeInstructionPostOrder()) {
    if (instr->opcode() == HloOpcode::kAdd) {
      fused_sums.push_back(instr);
    }
  }

  // Fuse into fusion(fusion(param0 + param1 + param2) + param3 + param4).
  auto* inner_fusion =
      outer_fusion->fused_instructions_computation()->CreateFusionInstruction(
          {fused_sums[1], fused_sums[0]}, HloInstruction::FusionKind::kLoop);

  // Generate the graph; all nodes should be present.
  TF_ASSERT_OK_AND_ASSIGN(
      std::string graph,
      RenderGraph(*root_computation, /*label=*/"", DebugOptions(),
                  RenderedGraphFormat::kDot));
  for (const HloComputation* computation :
       {root_computation,  //
        inner_fusion->fused_instructions_computation(),
        outer_fusion->fused_instructions_computation()}) {
    for (const HloInstruction* instruction : computation->instructions()) {
      EXPECT_THAT(graph, HasSubstr(instruction->name()));
    }
  }

  // Dump a neighborhood around one of the inner sum nodes.  We don't really
  // care that the outer nodes are omitted -- whether they are or not is based
  // fiddly heuristics -- but we do care that the node we asked for is printed.
  const HloInstruction* inner_sum = nullptr;
  for (const HloInstruction* instruction :
       inner_fusion->fused_instructions_computation()->instructions()) {
    if (instruction->opcode() == HloOpcode::kAdd) {
      inner_sum = instruction;
      break;
    }
  }
  ASSERT_NE(inner_sum, nullptr);
  TF_ASSERT_OK_AND_ASSIGN(std::string neighborhood_graph,
                          RenderNeighborhoodAround(*inner_sum, /*radius=*/1,
                                                   RenderedGraphFormat::kDot));
  EXPECT_THAT(neighborhood_graph, HasSubstr(inner_sum->name()));
}

TEST_F(HloGraphDumperTest, Constant) {
  HloComputation::Builder b("b");
  auto instruction = b.AddInstruction(
      HloInstruction::CreateConstant(LiteralUtil::CreateR0<float>(-42)));
  instruction->SetAndSanitizeName("i_am_a_constant_root_instruction");
  HloModuleConfig config;
  HloModule m(TestName(), config);
  HloComputation* root_computation = m.AddEntryComputation(b.Build());
  TF_ASSERT_OK_AND_ASSIGN(
      std::string graph,
      RenderGraph(*root_computation, /*label=*/"an_empty_graph", DebugOptions(),
                  RenderedGraphFormat::kDot));
  EXPECT_THAT(graph, HasSubstr("an_empty_graph"));
}

TEST_F(HloGraphDumperTest, TupleConstant) {
  Shape tuple_shape = ShapeUtil::MakeTupleShape(
      {ShapeUtil::MakeShape(F32, {3, 2}), ShapeUtil::MakeShape(S32, {4, 5})});
  HloComputation::Builder b("b");
  auto constant = b.AddInstruction(
      HloInstruction::CreateConstant(Literal::CreateFromShape(tuple_shape)));
  auto gte = b.AddInstruction(HloInstruction::CreateGetTupleElement(
      ShapeUtil::MakeShape(F32, {3, 2}), constant, 0));

  HloModuleConfig config;
  HloModule m(TestName(), config);
  HloComputation* root_computation = m.AddEntryComputation(b.Build(gte));
  TF_ASSERT_OK_AND_ASSIGN(
      std::string graph,
      RenderGraph(*root_computation, /*label=*/"tuple_constant", DebugOptions(),
                  RenderedGraphFormat::kDot));
  EXPECT_THAT(graph, HasSubstr("tuple_constant"));
  EXPECT_THAT(graph, HasSubstr("constant (f32[3,2], s32[4,5])"));
}

TEST_F(HloGraphDumperTest, Compare) {
  const char* hlo_string = R"(
    HloModule comp

    ENTRY comp {
      param.0 = f32[10] parameter(0)
      param.1 = f32[10] parameter(1)
      ROOT lt = pred[10] compare(param.0, param.1), direction=LT
    })";
  TF_ASSERT_OK_AND_ASSIGN(auto module,
                          ParseAndReturnVerifiedModule(hlo_string));
  TF_ASSERT_OK_AND_ASSIGN(
      std::string graph,
      RenderGraph(*module->entry_computation(), /*label=*/"tuple_constant",
                  DebugOptions(), RenderedGraphFormat::kDot));
  EXPECT_THAT(graph, HasSubstr("direction=LT"));
}

TEST_F(HloGraphDumperTest, HasStatisticsViz) {
  const char* hlo_string = R"(
    HloModule comp

    ENTRY comp {
      param.0 = f32[10] parameter(0), statistics={visualizing_index=0,stat-0=0.5}
      param.1 = f32[10] parameter(1), statistics={visualizing_index=1,stat-0=55.5,stat-1=44.4}
      ROOT lt = pred[10] compare(param.0, param.1), direction=LT
    })";
  TF_ASSERT_OK_AND_ASSIGN(auto module,
                          ParseAndReturnVerifiedModule(hlo_string));

  // Just check that it doesn't crash.
  TF_ASSERT_OK_AND_ASSIGN(
      std::string graph,
      RenderGraph(*module->entry_computation(), /*label=*/"tuple_constant",
                  DebugOptions(), RenderedGraphFormat::kDot));
}

TEST_F(HloGraphDumperTest, RootIsConstant) {
  const char* hlo_string = R"(
HloModule indexed_conditional

%then_branch (empty: ()) -> f32[] {
  %empty = () parameter(0)
  ROOT %then = f32[] constant(1)
}

%else_branch (empty.1: ()) -> f32[] {
  %empty.1 = () parameter(0)
  ROOT %else = f32[] constant(2)
}

ENTRY %conditional_select (constant: pred[]) -> (f32[]) {
  %constant = pred[] parameter(0)
  %emptytuple = () tuple()
  %conditional = f32[] conditional(pred[] %constant, () %emptytuple, () %emptytuple), true_computation=%then_branch, false_computation=%else_branch
  ROOT %t = (f32[]) tuple(f32[] %conditional)
})";
  TF_ASSERT_OK_AND_ASSIGN(auto module,
                          ParseAndReturnVerifiedModule(hlo_string));
  // Just check that it doesn't crash.
  TF_ASSERT_OK_AND_ASSIGN(
      std::string graph,
      RenderGraph(*module->entry_computation(), /*label=*/"tuple_constant",
                  DebugOptions(), RenderedGraphFormat::kDot));
}

TEST_F(HloGraphDumperTest, ShowCallers) {
  const char* hlo_string = R"(
    command_buffer {
      ROOT root = f32[16] parameter(0)
    }
    ENTRY comp {
      p0 = f32[16] parameter(0)
      ROOT call.1 = f32[16] call(p0), to_apply=command_buffer
    })";
  TF_ASSERT_OK_AND_ASSIGN(auto module,
                          ParseAndReturnVerifiedModule(hlo_string));
  TF_ASSERT_OK_AND_ASSIGN(
      std::string graph, RenderGraph(*module->entry_computation(),
                                     /*label=*/"command_buffer", DebugOptions(),
                                     RenderedGraphFormat::kDot));
  EXPECT_THAT(graph, HasSubstr("ENTRY computation"));

  TF_ASSERT_OK_AND_ASSIGN(
      graph,
      RenderGraph(*module->entry_computation()->root_instruction()->to_apply(),
                  /*label=*/"command_buffer", DebugOptions(),
                  RenderedGraphFormat::kDot));
  EXPECT_THAT(graph, HasSubstr("Caller instructions: call.1"));
}

TEST_F(HloGraphDumperTest, OverrideColors) {
  const char* hlo_string = R"(
    HloModule comp

    ENTRY comp {
      param.0 = f32[10] parameter(0)
      param.1 = f32[10] parameter(1)
      ROOT lt = pred[10] compare(param.0, param.1), direction=LT
    })";
  TF_ASSERT_OK_AND_ASSIGN(auto module,
                          ParseAndReturnVerifiedModule(hlo_string));
  // Create a color map with color and stats
  absl::flat_hash_map<const HloInstruction*, ColorStats> color_map;
  ColorStats color_stats_1;
  color_stats_1.color = "#A9C343";
  color_stats_1.stats = absl::StrFormat("%.3f", 1.11);
  ColorStats color_stats_2;
  color_stats_2.color = "#BC8A3F";
  color_stats_2.stats = absl::StrFormat("%.3f", 2.22);
  color_map[module->entry_computation()->GetInstructionWithName("param.0")] =
      color_stats_1;
  color_map[module->entry_computation()->GetInstructionWithName("param.1")] =
      color_stats_2;

  HloRenderOptions hlo_render_options;
  hlo_render_options.override_node_colors = true;
  TF_ASSERT_OK_AND_ASSIGN(
      std::string graph,
      RenderGraph(*module->entry_computation(), /*label=*/"tuple_constant",
                  DebugOptions(), RenderedGraphFormat::kDot, hlo_render_options,
                  color_map));
  EXPECT_THAT(graph, HasSubstr("#A9C343"));
  EXPECT_THAT(graph, HasSubstr("1.110"));
  EXPECT_THAT(graph, HasSubstr("#BC8A3F"));
  EXPECT_THAT(graph, HasSubstr("2.220"));
}

TEST_F(HloGraphDumperTest, AnnotateCalledComputationsParameters) {
  const char* hlo_string = R"(
    command_buffer.0 {
      p0 = f32[1024] parameter(0)
      add.123 = f32[1024] add(p0, p0)
      mul.456 = f32[1024] multiply(add.123, p0)
      ROOT tuple = (f32[1024], f32[1024]) tuple(add.123, mul.456)
    }
    command_buffer.1 {
      p0 = f32[1024] parameter(0)  // Output of add.123, but hard to tell due to
                                   // get-tuple-element, call, bitcast, etc.
      p1 = f32[1024] parameter(1)  // Output of mul.456.
      ROOT mul = f32[1024] multiply(p1, p0)
    }
    ENTRY comp {
      p0 = f32[1024] parameter(0)
      call.0 = (f32[1024], f32[1024]) call(p0), to_apply=command_buffer.0
      gte.0 = f32[1024] get-tuple-element(call.0), index=0
      gte.1 = f32[1024] get-tuple-element(call.0), index=1
      bitcast = f32[32,32] bitcast(gte.0)
      tuple = (f32[1024], f32[32,32]) tuple(gte.1, bitcast)
      gte.2 = f32[1024] get-tuple-element(tuple), index=0
      gte.3 = f32[32,32] get-tuple-element(tuple), index=1
      bitcast.1 = f32[1024] bitcast(gte.3)
      ROOT call.1 = f32[1024] call(bitcast.1, gte.2), to_apply=command_buffer.1
    })";
  TF_ASSERT_OK_AND_ASSIGN(auto module,
                          ParseAndReturnVerifiedModule(hlo_string));
  TF_ASSERT_OK_AND_ASSIGN(
      std::string graph,
      RenderGraph(*module->entry_computation()->root_instruction()->to_apply(),
                  /*label=*/"command buffer", DebugOptions(),
                  RenderedGraphFormat::kDot));
  EXPECT_THAT(graph, HasSubstr("<b>Parameter 0</b><br/>"
                               "<i>from add.123 in command_buffer.0</i>"));
  EXPECT_THAT(graph, HasSubstr("<b>Parameter 1</b><br/>"
                               "<i>from mul.456 in command_buffer.0</i>"));
}

TEST_F(HloGraphDumperTest, AnnotateCalledComputationsParametersTuple) {
  const char* hlo_string = R"(
    command_buffer {
      p0 = (f32[1024], f32[1024]) parameter(0)
      gte.0 = f32[1024] get-tuple-element(p0), index=0
      gte.1 = f32[1024] get-tuple-element(p0), index=1
      ROOT tuple = (f32[1024], f32[1024]) tuple(gte.0, gte.1)
    }
    ENTRY comp {
      p0 = f32[1024] parameter(0)
      add.123 = f32[1024] add(p0, p0)
      mul.456 = f32[1024] multiply(p0, p0)
      tuple.1 = (f32[1024], f32[1024]) tuple(add.123, mul.456)
      call.0 = (f32[1024], f32[1024]) call(tuple.1), to_apply=command_buffer
    })";
  TF_ASSERT_OK_AND_ASSIGN(auto module,
                          ParseAndReturnVerifiedModule(hlo_string));
  TF_ASSERT_OK_AND_ASSIGN(
      std::string graph,
      RenderGraph(*module->entry_computation()->root_instruction()->to_apply(),
                  /*label=*/"command buffer", DebugOptions(),
                  RenderedGraphFormat::kDot));
  // Annotate the parameter as `tuple.1`, rather than `add.123` or `mul.456`,
  // because both are being passed at the same time.
  EXPECT_THAT(graph, HasSubstr("<b>Parameter 0</b><br/>"
                               "<i>from tuple.1 in the ENTRY computation</i>"));
}

}  // anonymous namespace
}  // namespace xla
