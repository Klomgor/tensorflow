// RUN: xla-translate -split-input-file -mlir-hlo-to-hlo-text %s | FileCheck %s

// CHECK-LABEL: ENTRY %main.{{.*}} (Arg_0.1: f32[], Arg_1.2: f32[4]) -> f32[4,4]
func.func public @main(%arg0: tensor<f32> {mhlo.sharding = ""}, %arg1: tensor<4xf32> {mhlo.sharding = "\08\03\1A\01\02\22\02\00\01"}) -> (tensor<4x4xf32> {mhlo.sharding = "\08\03\1A\02\02\01\22\02\00\01"}) {
  // CHECK-NEXT: %Arg_1.2 = f32[4] parameter(1), sharding={devices=[2]0,1}
  // CHECK-NEXT: %Arg_0.1 = f32[] parameter(0), sharding={replicated}
  %0 = "mhlo.broadcast_in_dim"(%arg0) <{broadcast_dimensions = dense<> : tensor<0xi64>}> : (tensor<f32>) -> tensor<4xf32>
  %1 = mhlo.multiply %arg1, %0 : tensor<4xf32>
  %2 = "mhlo.broadcast_in_dim"(%1) <{broadcast_dimensions = dense<0> : tensor<1xi64>}> : (tensor<4xf32>) -> tensor<4x4xf32>
  // CHECK: ROOT {{.*}}, sharding={devices=[2,1]0,1}
  func.return %2 : tensor<4x4xf32>
}

// -----

// CHECK-LABEL: ENTRY %main.{{.*}} ({{[^,]*}}: f32[5,8,128]) -> f32[5,8,128]
func.func @main(%arg0: tensor<5x8x128xf32> {mhlo.sharding = "\08\03\1A\03\01\02\01\22\02\00\01"}) -> (tensor<5x8x128xf32> {mhlo.sharding = "\08\03\1A\03\01\02\01\22\02\00\01"}) {
  // CHECK-NEXT: %Arg_0.1 = f32[5,8,128] parameter(0), sharding={devices=[1,2,1]0,1}
  // CHECK-NEXT: %custom-call.2 = f32[5,8,128] custom-call(%Arg_0.1), custom_call_target="Sharding", sharding={devices=[1,2,1]0,1}
  // CHECK-NEXT: %tuple.3 = (f32[5,8,128]) tuple(%custom-call.2)
  // CHECK-SAME: sharding={{\{}}{devices=[1,2,1]0,1}}
  // CHECK-NEXT: ROOT %get-tuple-element.4 = f32[5,8,128] get-tuple-element(%tuple.3), index=0
  // CHECK-SAME: sharding={devices=[1,2,1]0,1}
  %0 = "mhlo.custom_call"(%arg0) {call_target_name = "Sharding",
				  mhlo.sharding = "\08\03\1A\03\01\02\01\22\02\00\01"
				 } : (tensor<5x8x128xf32>) -> tensor<5x8x128xf32>
  func.return %0 : tensor<5x8x128xf32>
}

// -----

// CHECK-LABEL: ENTRY %main.{{.*}} ({{[^,]*}}: f32[4,4]) -> (f32[4,4], f32[4,4])
func.func @main(%arg0: tensor<4x4xf32>) -> (tensor<4x4xf32> {mhlo.sharding = "\08\03\1A\03\02\01\02\22\04\00\01\02\03B\01\00"}, tensor<4x4xf32>) {
  // CHECK-NEXT: %Arg_0.1 = f32[4,4] parameter(0)
  // CHECK-NEXT: [[RESHAPE_0:%.*]] = f32[4,4] reshape(%Arg_0.1), sharding={devices=[2,1,2]0,1,2,3 last_tile_dim_replicate}
  // CHECK-NEXT: [[RESHAPE_1:%.*]] = f32[4,4] reshape(%Arg_0.1)
  // CHECK-NOT:  sharding
  // CHECK-NEXT: ROOT {{%.*}} = (f32[4,4], f32[4,4]) tuple(%Arg_0.1, %Arg_0.1)
  // CHECK-SAME: sharding={{\{}}{devices=[2,1,2]0,1,2,3 last_tile_dim_replicate}, {replicated}}
  return %arg0, %arg0 : tensor<4x4xf32>, tensor<4x4xf32>
}

// -----

// CHECK-LABEL: ENTRY %main.{{.*}} () -> f32[4]
func.func @main() -> (tensor<4xf32>) {
  // CHECK-NEXT: %constant.1 = f32[] constant(3.1415925)
  // CHECK-NEXT: %broadcast.2 = f32[4] broadcast(%constant.1), dimensions={}, sharding={devices=[2]0,1}
  // CHECK-NEXT: ROOT %add.3 = f32[4] add(%broadcast.2, %broadcast.2)
  %0 = mhlo.constant {mhlo.sharding = "{devices=[2]0,1}"} dense<3.1415926> : tensor<4xf32>
  %1 = mhlo.add %0, %0 : tensor<4xf32>
  return %1 : tensor<4xf32>
}

// -----

// CHECK-LABEL: ENTRY %main.{{.*}} () -> f32[12,24,36]
func.func @main() -> (tensor<12x24x36xf32>) {
  // CHECK-NEXT: %constant.1 = f32[] constant(3.1415925)
  // CHECK-NEXT: %broadcast.2 = f32[12,24,36] broadcast(%constant.1), dimensions={}, sharding={devices=[1,2,1]0,1}
  // CHECK-NEXT: ROOT %add.3 = f32[12,24,36] add(%broadcast.2, %broadcast.2)
  %0 = mhlo.constant {mhlo.sharding = "{devices=[1,2,1]0,1}"} dense<3.1415926> : tensor<12x24x36xf32>
  %1 = mhlo.add %0, %0 : tensor<12x24x36xf32>
  return %1 : tensor<12x24x36xf32>
}

// -----

// CHECK-LABEL: ENTRY %main.{{.*}} (Arg_0.1: u64[2]) -> (u64[2], u32[512,4])
func.func @main(%arg0: tensor<2xui64>) -> (tensor<2xui64> {mhlo.sharding = "{devices=[2,16]<=[32] last_tile_dim_replicate}"}, tensor<512x4xui32> {mhlo.sharding = "{devices=[4,8]<=[32]}"}) {
  // CHECK-NEXT: %Arg_0.1 = u64[2] parameter(0)
  // CHECK-NEXT: %rng-bit-generator.2 = (u64[2], u32[512,4]) rng-bit-generator(%Arg_0.1), algorithm=rng_default, sharding={{\{}}{replicated}, {devices=[8,4]<=[32]}}
  // CHECK-NEXT: %get-tuple-element.3 = u64[2] get-tuple-element(%rng-bit-generator.2), index=0, sharding={replicated}
  // CHECK-NEXT: %add.5 = u64[2] add(%get-tuple-element.3, %get-tuple-element.3)
  // CHECK-NEXT: %reshape.6 = u64[2] reshape(%add.5)
  // CHECK-NEXT: %get-tuple-element.4 = u32[512,4] get-tuple-element(%rng-bit-generator.2), index=1, sharding={devices=[8,4]<=[32]}
  // CHECK-NEXT: %reshape.7 = u32[512,4] reshape(%get-tuple-element.4)
  // CHECK-NEXT: ROOT %tuple.8 = (u64[2], u32[512,4]) tuple(%add.5, %get-tuple-element.4), sharding={{\{}}{devices=[2,16]<=[32] last_tile_dim_replicate}, {devices=[4,8]<=[32]}}
  %output_state, %output = "mhlo.rng_bit_generator"(%arg0) <{rng_algorithm = #mhlo.rng_algorithm<DEFAULT>}> {mhlo.sharding = "{{replicated}, {devices=[8,4]<=[32]}}"} : (tensor<2xui64>) -> (tensor<2xui64>, tensor<512x4xui32>)
  %0 = mhlo.add %output_state, %output_state : tensor<2xui64>
  return %0, %output : tensor<2xui64>, tensor<512x4xui32>
}

// -----

// CHECK-LABEL: ENTRY %main.{{.*}} (Arg_0.1: u64[2]) -> (u64[2], u32[512,4])
func.func @main(%arg0: tensor<2xui64>) -> (tensor<2xui64>, tensor<512x4xui32>) {
  // CHECK-NEXT: %Arg_0.1 = u64[2] parameter(0)
  // CHECK-NEXT: %rng-bit-generator.2 = (u64[2], u32[512,4]) rng-bit-generator(%Arg_0.1), algorithm=rng_default, sharding={{\{}}{replicated}, {replicated}}
  // CHECK-NEXT: %get-tuple-element.3 = u64[2] get-tuple-element(%rng-bit-generator.2), index=0, sharding={replicated}
  // CHECK-NEXT: %add.5 = u64[2] add(%get-tuple-element.3, %get-tuple-element.3)
  // CHECK-NEXT: %get-tuple-element.4 = u32[512,4] get-tuple-element(%rng-bit-generator.2), index=1, sharding={replicated}
  // CHECK-NEXT: ROOT %tuple.6 = (u64[2], u32[512,4]) tuple(%add.5, %get-tuple-element.4)
  %output_state, %output = "mhlo.rng_bit_generator"(%arg0) <{rng_algorithm = #mhlo.rng_algorithm<DEFAULT>}> {mhlo.sharding = "{replicated}"} : (tensor<2xui64>) -> (tensor<2xui64>, tensor<512x4xui32>)
  %0 = mhlo.add %output_state, %output_state : tensor<2xui64>
  return %0, %output : tensor<2xui64>, tensor<512x4xui32>
}

// -----

// CHECK-LABEL: HloModule main

// CHECK:      %[[BODY:region_0.[0-9]+]] ([[ARG:Arg_.[0-9]+]]: s32[]) -> s32[] {
// CHECK-NEXT:   %[[ARG]] = s32[] parameter(0), sharding={replicated}
// CHECK-NEXT:   %[[ADD:add.[0-9]+]] = s32[] add(%[[ARG]], %[[ARG]])
// CHECK-NEXT:   %[[TUPLE:tuple.[0-9]+]] = (s32[]) tuple(%[[ADD]])
// CHECK-NEXT:   ROOT %get-tuple-element.{{[0-9]+}} = s32[] get-tuple-element(%[[TUPLE]]), index=0, sharding={replicated}

// CHECK:      %[[COND:region_1.[0-9]+]] ([[ARG:Arg_.[0-9]+]]: s32[]) -> pred[] {
// CHECK-NEXT:   %[[ARG]] = s32[] parameter(0), sharding={replicated}
// CHECK-NEXT:   ROOT %compare.{{[0-9]+}} = pred[] compare(%[[ARG]], %[[ARG]]), direction=LT

// CHECK:      ENTRY %main.{{[0-9]+}} ([[ARG:Arg_0.[0-9]+]]: s32[]) -> s32[] {
// CHECK-NEXT:   %[[ARG]] = s32[] parameter(0)
// CHECK-NEXT:   ROOT %while.10 = s32[] while(%[[ARG]]), condition=%[[COND]], body=%[[BODY]], sharding={replicated}

func.func @main(%arg0: tensor<i32>) -> tensor<i32> {
  %0 = mhlo.while(%iterArg = %arg0) : tensor<i32> attributes {mhlo.sharding = "{replicated}"}
    cond {
    %1 = mhlo.compare LT, %iterArg, %iterArg : (tensor<i32>, tensor<i32>) -> tensor<i1>
    mhlo.return %1 : tensor<i1>
  } do {
    %1 = mhlo.add %iterArg, %iterArg : tensor<i32>
    mhlo.return %1 : tensor<i32>
  }
  func.return %0 : tensor<i32>
}

// -----

// CHECK-LABEL: HloModule main

// CHECK:      %[[BODY:region_0.[0-9]+]] ([[ARG_TUPLE:arg_tuple.[0-9]+]]: (s32[], f32[4], f32[4])) -> (s32[], f32[4], f32[4]) {
// CHECK-NEXT:   %[[ARG_TUPLE]] = (s32[], f32[4], f32[4]) parameter(0)
// CHECK-SAME:     sharding={{\{}}{replicated}, {devices=[2,2]<=[4] last_tile_dim_replicate}, {devices=[4]<=[4]}}
// CHECK-NEXT:   %[[GTE0:get-tuple-element.[0-9]+]] = s32[] get-tuple-element(%[[ARG_TUPLE]]), index=0, sharding={replicated}
// CHECK-NEXT:   %[[GTE1:get-tuple-element.[0-9]+]] = f32[4] get-tuple-element(%[[ARG_TUPLE]]), index=1, sharding={devices=[2,2]<=[4] last_tile_dim_replicate}
// CHECK-NEXT:   %[[GTE2:get-tuple-element.[0-9]+]] = f32[4] get-tuple-element(%[[ARG_TUPLE]]), index=2, sharding={devices=[4]<=[4]}
// CHECK-NEXT:   %[[ADD:add.[0-9]+]] = f32[4] add(%[[GTE1]], %[[GTE2]])
// CHECK-NEXT:   ROOT %tuple.{{[0-9]+}} = (s32[], f32[4], f32[4]) tuple(%[[GTE0]], %[[ADD]], %[[GTE2]])
// CHECK-SAME:     sharding={{\{}}{replicated}, {devices=[2,2]<=[4] last_tile_dim_replicate}, {devices=[4]<=[4]}}

// CHECK:      %[[COND:region_1.[0-9]+]] ([[ARG_TUPLE:arg_tuple.[0-9]+]]: (s32[], f32[4], f32[4])) -> pred[] {
// CHECK-NEXT:   %[[ARG_TUPLE]] = (s32[], f32[4], f32[4]) parameter(0)
// CHECK-SAME:     sharding={{\{}}{replicated}, {devices=[2,2]<=[4] last_tile_dim_replicate}, {devices=[4]<=[4]}}
// CHECK-NEXT:   %[[GTE15:get-tuple-element.[0-9]+]] = f32[4] get-tuple-element(%[[ARG_TUPLE]]), index=1, sharding={devices=[2,2]<=[4] last_tile_dim_replicate}
// CHECK-NEXT:   %[[GTE16:get-tuple-element.[0-9]+]] = f32[4] get-tuple-element(%[[ARG_TUPLE]]), index=2, sharding={devices=[4]<=[4]}
// CHECK-NEXT:   %[[GTE14:get-tuple-element.[0-9]+]] = s32[] get-tuple-element(%[[ARG_TUPLE]]), index=0, sharding={replicated}
// CHECK-NEXT:   ROOT %compare.{{[0-9]+}} = pred[] compare(%[[GTE14]], %[[GTE14]]), direction=LT

// CHECK:      ENTRY %main.{{[0-9]+}} ([[ARG0:Arg_0.[0-9]+]]: s32[], [[ARG1:Arg_1.[0-9]+]]: f32[4], [[ARG2:Arg_2.[0-9]+]]: f32[4]) -> (f32[4], f32[4]) {
// CHECK-NEXT:   %[[ARG0]] = s32[] parameter(0)
// CHECK-NEXT:   %[[ARG1]] = f32[4] parameter(1)
// CHECK-NEXT:   %[[ARG2]] = f32[4] parameter(2)
// CHECK-NEXT:   %[[TUPLE:tuple.[0-9]+]] = (s32[], f32[4], f32[4]) tuple(%[[ARG0]], %[[ARG1]], %[[ARG2]])
// CHECK-SAME:     sharding={{\{}}{replicated}, {devices=[2,2]<=[4] last_tile_dim_replicate}, {devices=[4]<=[4]}}
// CHECK-NEXT:   %[[WHILE:while.[0-9]+]] = (s32[], f32[4], f32[4]) while(%[[TUPLE]]), condition=%[[COND]], body=%[[BODY]]
// CHECK-SAME:     sharding={{\{}}{replicated}, {devices=[2,2]<=[4] last_tile_dim_replicate}, {devices=[4]<=[4]}}
// CHECK-NEXT:   %[[GTE19:get-tuple-element.[0-9]+]] = s32[] get-tuple-element(%[[WHILE]]), index=0, sharding={replicated}
// CHECK-NEXT:   %[[GTE20:get-tuple-element.[0-9]+]] = f32[4] get-tuple-element(%[[WHILE]]), index=1, sharding={devices=[2,2]<=[4] last_tile_dim_replicate}
// CHECK-NEXT:   %[[GTE21:get-tuple-element.[0-9]+]] = f32[4] get-tuple-element(%[[WHILE]]), index=2, sharding={devices=[4]<=[4]}
// CHECK-NEXT:   ROOT %tuple.{{[0-9]+}} = (f32[4], f32[4]) tuple(%[[GTE20]], %[[GTE21]])

func.func @main(%arg0: tensor<i32>, %arg1: tensor<4xf32>, %arg2: tensor<4xf32>) -> (tensor<4xf32>, tensor<4xf32>) {
  %0:3 = mhlo.while(%iterArg = %arg0, %iterArg_0 = %arg1, %iterArg_1 = %arg2) : tensor<i32>, tensor<4xf32>, tensor<4xf32>
    attributes {mhlo.sharding = "{{replicated},{devices=[2,2]<=[4] last_tile_dim_replicate},{devices=[4]<=[4]}}"}
    cond {
    %1 = mhlo.compare LT, %iterArg, %iterArg : (tensor<i32>, tensor<i32>) -> tensor<i1>
    mhlo.return %1 : tensor<i1>
  } do {
    %1 = mhlo.add %iterArg_0, %iterArg_1 : tensor<4xf32>
    mhlo.return %iterArg, %1, %iterArg_1 : tensor<i32>, tensor<4xf32>, tensor<4xf32>
  }
  func.return %0#1, %0#2 : tensor<4xf32>, tensor<4xf32>
}

// -----

// CHECK-LABEL: HloModule main

// CHECK:      %[[BODY:region_0.[0-9]+]] ([[ARG_TUPLE:arg_tuple.*]]: (s32[], f32[4], f32[4])) -> (s32[], f32[4], f32[4]) {
// CHECK-NEXT:   %[[ARG_TUPLE]] = (s32[], f32[4], f32[4]) parameter(0)
// CHECK-SAME:     sharding={{\{}}{manual}, {manual}, {manual}}
// CHECK-NEXT:   %[[GTE7:get-tuple-element.*]] = s32[] get-tuple-element(%[[ARG_TUPLE]]), index=0, sharding={manual}
// CHECK-NEXT:   %[[GTE8:get-tuple-element.*]] = f32[4] get-tuple-element(%[[ARG_TUPLE]]), index=1, sharding={manual}
// CHECK-NEXT:   %[[GTE9:get-tuple-element.*]] = f32[4] get-tuple-element(%[[ARG_TUPLE]]), index=2, sharding={manual}
// CHECK-NEXT:   %[[ADD:add.*]] = f32[4] add(%[[GTE8]], %[[GTE9]])
// CHECK-NEXT:   ROOT %tuple.{{.*}} = (s32[], f32[4], f32[4]) tuple(%[[GTE7]], %[[ADD]], %[[GTE9]])
// CHECK-SAME:     sharding={{\{}}{manual}, {manual}, {manual}}

// CHECK:      %[[COND:region_1.[0-9]+]] ([[ARG_TUPLE:arg_tuple.*]]: (s32[], f32[4], f32[4])) -> pred[] {
// CHECK-NEXT:   %[[ARG_TUPLE]] = (s32[], f32[4], f32[4]) parameter(0)
// CHECK-SAME:     sharding={{\{}}{manual}, {manual}, {manual}}
// CHECK-NEXT:   %[[GTE15:get-tuple-element.*]] = f32[4] get-tuple-element(%[[ARG_TUPLE]]), index=1, sharding={manual}
// CHECK-NEXT:   %[[GTE16:get-tuple-element.*]] = f32[4] get-tuple-element(%[[ARG_TUPLE]]), index=2, sharding={manual}
// CHECK-NEXT:   %[[GTE14:get-tuple-element.*]] = s32[] get-tuple-element(%[[ARG_TUPLE]]), index=0, sharding={manual}
// CHECK-NEXT:   ROOT %compare.{{.*}} = pred[] compare(%[[GTE14]], %[[GTE14]]), direction=LT

// CHECK:      ENTRY %main.{{.*}} ([[ARG0:Arg_0.*]]: s32[], [[ARG1:Arg_1.*]]: f32[4], [[ARG2:Arg_2.*]]: f32[4]) -> (f32[4], f32[4]) {
// CHECK-NEXT:   %[[ARG0]] = s32[] parameter(0)
// CHECK-NEXT:   %[[ARG1]] = f32[4] parameter(1)
// CHECK-NEXT:   %[[ARG2]] = f32[4] parameter(2)
// CHECK-NEXT:   %[[TUPLE:tuple.*]] = (s32[], f32[4], f32[4]) tuple(%[[ARG0]], %[[ARG1]], %[[ARG2]])
// CHECK-SAME:     sharding={{\{}}{manual}, {manual}, {manual}}
// CHECK-NEXT:   %[[WHILE:while.*]] = (s32[], f32[4], f32[4]) while(%[[TUPLE]]), condition=%[[COND]], body=%[[BODY]]
// CHECK-SAME:     sharding={{\{}}{manual}, {manual}, {manual}}
// CHECK-NEXT:   %[[GTE19:get-tuple-element.*]] = s32[] get-tuple-element(%[[WHILE]]), index=0, sharding={manual}
// CHECK-NEXT:   %[[GTE20:get-tuple-element.*]] = f32[4] get-tuple-element(%[[WHILE]]), index=1, sharding={manual}
// CHECK-NEXT:   %[[GTE21:get-tuple-element.*]] = f32[4] get-tuple-element(%[[WHILE]]), index=2, sharding={manual}
// CHECK-NEXT:   ROOT %tuple.{{.*}} = (f32[4], f32[4]) tuple(%[[GTE20]], %[[GTE21]])

func.func @main(%arg0: tensor<i32>, %arg1: tensor<4xf32>, %arg2: tensor<4xf32>) -> (tensor<4xf32>, tensor<4xf32>) {
  %0:3 = mhlo.while(%iterArg = %arg0, %iterArg_0 = %arg1, %iterArg_1 = %arg2) : tensor<i32>, tensor<4xf32>, tensor<4xf32>
    attributes {mhlo.sharding = "{manual}"}
    cond {
    %1 = mhlo.compare LT, %iterArg, %iterArg : (tensor<i32>, tensor<i32>) -> tensor<i1>
    mhlo.return %1 : tensor<i1>
  } do {
    %1 = mhlo.add %iterArg_0, %iterArg_1 : tensor<4xf32>
    mhlo.return %iterArg, %1, %iterArg_1 : tensor<i32>, tensor<4xf32>, tensor<4xf32>
  }
  func.return %0#1, %0#2 : tensor<4xf32>, tensor<4xf32>
}

// -----

// CHECK-LABEL: HloModule main

// CHECK:      %[[BRANCH0:region_0.*]] ([[ARG_TUPLE:arg_tuple.*]]: (f32[4], f32[4])) -> (f32[4], f32[4]) {
// CHECK-NEXT:   %[[ARG_TUPLE]] = (f32[4], f32[4]) parameter(0), sharding={{\{}}{devices=[2,2]<=[4] last_tile_dim_replicate}, {replicated}}
// CHECK-NEXT:   %[[GTE10:get-tuple-element.*]] = f32[4] get-tuple-element(%[[ARG_TUPLE]]), index=0, sharding={devices=[2,2]<=[4] last_tile_dim_replicate}
// CHECK-NEXT:   %[[GTE11:get-tuple-element.*]] = f32[4] get-tuple-element(%[[ARG_TUPLE]]), index=1
// CHECK-NEXT:   ROOT %tuple.{{.*}} = (f32[4], f32[4]) tuple(%[[GTE10]], %[[GTE11]]), sharding={{\{}}{replicated}, {devices=[4]<=[4]}}

// CHECK:      %[[BRANCH1:region_1.*]] ([[ARG_TUPLE:arg_tuple.*]]: (f32[4], f32[4])) -> (f32[4], f32[4]) {
// CHECK-NEXT:   %[[ARG_TUPLE]] = (f32[4], f32[4]) parameter(0), sharding={{\{}}{replicated}, {devices=[4]<=[4]}}
// CHECK-NEXT:   %[[GTE15:get-tuple-element.*]] = f32[4] get-tuple-element(%[[ARG_TUPLE]]), index=0, sharding={replicated}
// CHECK-NEXT:   %[[GTE16:get-tuple-element.*]] = f32[4] get-tuple-element(%[[ARG_TUPLE]]), index=1, sharding={devices=[4]<=[4]}
// CHECK-NEXT:   ROOT %tuple.{{.*}} = (f32[4], f32[4]) tuple(%[[GTE15]], %[[GTE16]]), sharding={{\{}}{replicated}, {devices=[4]<=[4]}}

// CHECK:      ENTRY %main.{{.*}} ([[ARG0:Arg_0.*]]: s32[], [[ARG1:Arg_1.*]]: f32[4], [[ARG2:Arg_2.*]]: f32[4], [[ARG3:Arg_3.*]]: f32[4], [[ARG4:Arg_4.*]]: f32[4]) -> (f32[4], f32[4]) {
// CHECK-NEXT:   %[[ARG0]] = s32[] parameter(0)
// CHECK-NEXT:   %[[ARG1]] = f32[4] parameter(1), sharding={devices=[2,2]<=[4] last_tile_dim_replicate}
// CHECK-NEXT:   %[[ARG2]] = f32[4] parameter(2)
// CHECK-NEXT:   %[[TUPLE6:tuple.*]] = (f32[4], f32[4]) tuple(%[[ARG1]], %[[ARG2]]), sharding={{\{}}{devices=[2,2]<=[4] last_tile_dim_replicate}, {replicated}}
// CHECK-NEXT:   %[[ARG3]] = f32[4] parameter(3), sharding={replicated}
// CHECK-NEXT:   %[[ARG4]] = f32[4] parameter(4), sharding={devices=[4]<=[4]}
// CHECK-NEXT:   %[[TUPLE7:tuple.*]] = (f32[4], f32[4]) tuple(%[[ARG3]], %[[ARG4]]), sharding={{\{}}{replicated}, {devices=[4]<=[4]}}
// CHECK-NEXT:   %[[COND:conditional.*]] = (f32[4], f32[4]) conditional(%[[ARG0]], %[[TUPLE6]], %[[TUPLE7]]), branch_computations={%[[BRANCH0]], %[[BRANCH1]]},
// CHECK-SAME:     sharding={{\{}}{replicated}, {devices=[4]<=[4]}}
// CHECK-NEXT:   %[[GTE19:get-tuple-element.*]] = f32[4] get-tuple-element(%[[COND]]), index=0, sharding={replicated}
// CHECK-NEXT:   %[[GTE20:get-tuple-element.*]] = f32[4] get-tuple-element(%[[COND]]), index=1, sharding={devices=[4]<=[4]}
// CHECK-NEXT:   ROOT %tuple.{{.*}} = (f32[4], f32[4]) tuple(%[[GTE19]], %[[GTE20]])

func.func @main(%arg0: tensor<i32>,
                %arg1: tensor<4xf32> {mhlo.sharding = "{devices=[2,2]<=[4] last_tile_dim_replicate}"},
                %arg2: tensor<4xf32>,
                %arg3: tensor<4xf32> {mhlo.sharding = "{replicated}"},
                %arg4: tensor<4xf32> {mhlo.sharding = "{devices=[4]<=[4]}"}) -> (tensor<4xf32>, tensor<4xf32>) {
  %0:2 = "mhlo.case"(%arg0) ( {
    mhlo.return %arg1, %arg2 : tensor<4xf32>, tensor<4xf32>
  },  {
    mhlo.return %arg3, %arg4 : tensor<4xf32>, tensor<4xf32>
  }) {mhlo.sharding = "{{replicated},{devices=[4]<=[4]}}"} : (tensor<i32>) -> (tensor<4xf32>, tensor<4xf32>)
  func.return %0#0, %0#1 : tensor<4xf32>, tensor<4xf32>
}

// -----

// Test export mhlo::CaseOp with no results and captured variables that have
// shardings.

// CHECK-LABEL: HloModule main

// CHECK:      %[[EMPTY_BRANCH:.*]] ({{.*}}: ()) -> () {
// CHECK-NEXT:   %[[ARG:.*]] = () parameter(0)
// CHECK-NEXT:   ROOT %{{.*}} = () tuple()
// CHECK-NEXT: }

// CHECK:      %[[CALLBACK_BRANCH:.*]] ({{.*}}: (s32[], s64[8])) -> () {
// CHECK-NEXT:   %[[ARG:.*]] = (s32[], s64[8]) parameter(0)
// CHECK-NEXT:   %[[ELEMENT_0:.*]] = s32[] get-tuple-element(%[[ARG]]), index=0, sharding={manual}
// CHECK-NEXT:   %[[ELEMENT_1:.*]] = s64[8] get-tuple-element(%[[ARG]]), index=1, sharding={manual}
// CHECK-NEXT:   %{{.*}} = () custom-call(%[[ELEMENT_0]], %[[ELEMENT_1]]),
// CHECK-SAME{LITERAL}: custom_call_target="xla_ffi_python_cpu_callback", sharding={{manual}}
// CHECK-NEXT:   ROOT %{{.*}} = () tuple()
// CHECK-NEXT: }

// CHECK: ENTRY {{.*}} ({{.*}}: s32[], {{.*}}: s64[8], {{.*}}: pred[]) -> () {
// CHECK-NEXT:    %[[ARG_2:.*]] = pred[] parameter(2)
// CHECK-NEXT:    %[[CONVERT:.*]] = s32[] convert(%[[ARG_2]]), sharding={manual}
// CHECK-NEXT:    %[[EMPTY_TUPLE:.*]] = () tuple()
// CHECK-NEXT:    %[[ARG_0:.*]] = s32[] parameter(0)
// CHECK-NEXT:    %[[FULL_TO_SHARD:.*]] = s32[] custom-call(%[[ARG_0]]), custom_call_target="SPMDFullToShardShape", sharding={manual}
// CHECK-NEXT:    %[[ARG_1:.*]] = s64[8] parameter(1), sharding={manual}
// CHECK-NEXT:    %[[ARG_TUPLE:.*]] = (s32[], s64[8]) tuple(%[[FULL_TO_SHARD]], %[[ARG_1]]),
// CHECK-SAME{LITERAL}: sharding={{manual}, {manual}}
// CHECK-NEXT:    %{{.*}} = () conditional(%[[CONVERT]], %[[EMPTY_TUPLE]], %[[ARG_TUPLE]]), branch_computations={%[[EMPTY_BRANCH]], %[[CALLBACK_BRANCH]]},
// CHECK-SAME{LITERAL}: sharding={{replicated}}
// CHECK-NEXT:    ROOT %{{.*}} = () tuple()
// CHECK-NEXT: }



func.func @main(%arg0: tensor<i32>, %arg1: tensor<8xi64> {mhlo.sharding = "{manual}"}, %arg3: tensor<i1>) {
  %0 = mhlo.custom_call @SPMDFullToShardShape(%arg0) {mhlo.sharding = "{manual}"} : (tensor<i32>) -> tensor<i32>
  %1 = mhlo.convert %arg3 {mhlo.sharding = "{manual}"} : (tensor<i1>) -> tensor<i32>
  "mhlo.case"(%1) ({
    mhlo.return
  }, {
    mhlo.custom_call @xla_ffi_python_cpu_callback(%0, %arg1) {mhlo.sharding = "{manual}"} : (tensor<i32>, tensor<8xi64>) -> ()
    mhlo.return
  }) {mhlo.sharding = "{replicated}"} : (tensor<i32>) -> ()
  return
}

// -----

// CHECK-LABEL: HloModule main

// CHECK:      %[[BRANCH0:region_0.*]] ([[ARG:Arg_.*]]: f32[4]) -> f32[4] {
// CHECK-NEXT:   ROOT %[[ARG]] = f32[4] parameter(0)

// CHECK:      %[[BRANCH1:region_1.*]] ([[ARG:Arg_.*]]: f32[4]) -> f32[4] {
// CHECK-NEXT:   ROOT %[[ARG]] = f32[4] parameter(0)

// CHECK:      ENTRY %main.{{.*}} ([[ARG0:Arg_0.*]]: s32[], [[ARG1:Arg_1.*]]: f32[4], [[ARG2:Arg_2.*]]: f32[4]) -> f32[4] {
// CHECK-NEXT:   %[[ARG0]] = s32[] parameter(0)
// CHECK-NEXT:   %[[ARG1]] = f32[4] parameter(1), sharding={devices=[2,2]<=[4] last_tile_dim_replicate}
// CHECK-NEXT:   %[[ARG2]] = f32[4] parameter(2)
// CHECK-NEXT:   ROOT %conditional.{{.*}} = f32[4] conditional(%[[ARG0]], %[[ARG1]], %[[ARG2]]), branch_computations={%[[BRANCH0]], %[[BRANCH1]]}
func.func @main(%arg0: tensor<i32>,
                %arg1: tensor<4xf32> {mhlo.sharding = "{devices=[2,2]<=[4] last_tile_dim_replicate}"},
                %arg2: tensor<4xf32>) -> tensor<4xf32> {
  %0 = "mhlo.case"(%arg0) ( {
    mhlo.return %arg1 : tensor<4xf32>
  },  {
    mhlo.return %arg2 : tensor<4xf32>
  }) : (tensor<i32>) -> tensor<4xf32>
  func.return %0 : tensor<4xf32>
}

// -----

// CHECK-LABEL: HloModule main

// CHECK:      %[[BRANCH0:region_0.*]] ([[ARG_TUPLE:arg_tuple.*]]: (f32[4], f32[4])) -> (f32[4], f32[4]) {
// CHECK-NEXT:   %[[ARG_TUPLE]] = (f32[4], f32[4]) parameter(0), sharding={{\{}}{devices=[2,2]<=[4] last_tile_dim_replicate}, {replicated}}
// CHECK-NEXT:   %[[GTE10:get-tuple-element.*]] = f32[4] get-tuple-element(%[[ARG_TUPLE]]), index=0, sharding={devices=[2,2]<=[4] last_tile_dim_replicate}
// CHECK-NEXT:   %[[GTE11:get-tuple-element.*]] = f32[4] get-tuple-element(%[[ARG_TUPLE]]), index=1
// CHECK-NEXT:   ROOT %tuple.{{.*}} = (f32[4], f32[4]) tuple(%[[GTE10]], %[[GTE11]]), sharding={{\{}}{replicated}, {devices=[4]<=[4]}}

// CHECK:      %[[BRANCH1:region_1.*]] ([[ARG_TUPLE:arg_tuple.*]]: (f32[4], f32[4])) -> (f32[4], f32[4]) {
// CHECK-NEXT:   %[[ARG_TUPLE]] = (f32[4], f32[4]) parameter(0), sharding={{\{}}{replicated}, {devices=[4]<=[4]}}
// CHECK-NEXT:   %[[GTE15:get-tuple-element.*]] = f32[4] get-tuple-element(%[[ARG_TUPLE]]), index=0, sharding={replicated}
// CHECK-NEXT:   %[[GTE16:get-tuple-element.*]] = f32[4] get-tuple-element(%[[ARG_TUPLE]]), index=1, sharding={devices=[4]<=[4]}
// CHECK-NEXT:   ROOT %tuple.{{.*}} = (f32[4], f32[4]) tuple(%[[GTE15]], %[[GTE16]]), sharding={{\{}}{replicated}, {devices=[4]<=[4]}}

// CHECK:      ENTRY %main.{{.*}} ([[ARG0:Arg_0.*]]: pred[], [[ARG1:Arg_1.*]]: f32[4], [[ARG2:Arg_2.*]]: f32[4], [[ARG3:Arg_3.*]]: f32[4], [[ARG4:Arg_4.*]]: f32[4]) -> (f32[4], f32[4]) {
// CHECK-NEXT:   %[[ARG0]] = pred[] parameter(0)
// CHECK-NEXT:   %[[ARG1]] = f32[4] parameter(1), sharding={devices=[2,2]<=[4] last_tile_dim_replicate}
// CHECK-NEXT:   %[[ARG2]] = f32[4] parameter(2)
// CHECK-NEXT:   %[[TUPLE6:tuple.*]] = (f32[4], f32[4]) tuple(%[[ARG1]], %[[ARG2]]), sharding={{\{}}{devices=[2,2]<=[4] last_tile_dim_replicate}, {replicated}}
// CHECK-NEXT:   %[[ARG3]] = f32[4] parameter(3), sharding={replicated}
// CHECK-NEXT:   %[[ARG4]] = f32[4] parameter(4), sharding={devices=[4]<=[4]}
// CHECK-NEXT:   %[[TUPLE7:tuple.*]] = (f32[4], f32[4]) tuple(%[[ARG3]], %[[ARG4]]), sharding={{\{}}{replicated}, {devices=[4]<=[4]}}
// CHECK-NEXT:   %conditional.18 = (f32[4], f32[4]) conditional(%[[ARG0]], %[[TUPLE6]], %[[TUPLE7]]), true_computation=%[[BRANCH0]], false_computation=%[[BRANCH1]],
// CHECK-SAME:     sharding={{\{}}{replicated}, {devices=[4]<=[4]}}
// CHECK-NEXT:   %[[GTE19:get-tuple-element.*]] = f32[4] get-tuple-element(%conditional.18), index=0, sharding={replicated}
// CHECK-NEXT:   %[[GTE20:get-tuple-element.*]] = f32[4] get-tuple-element(%conditional.18), index=1, sharding={devices=[4]<=[4]}
// CHECK-NEXT:   ROOT %tuple.{{.*}} = (f32[4], f32[4]) tuple(%[[GTE19]], %[[GTE20]])

func.func @main(%arg0: tensor<i1>,
                %arg1: tensor<4xf32> {mhlo.sharding = "{devices=[2,2]<=[4] last_tile_dim_replicate}"},
                %arg2: tensor<4xf32>,
                %arg3: tensor<4xf32> {mhlo.sharding = "{replicated}"},
                %arg4: tensor<4xf32> {mhlo.sharding = "{devices=[4]<=[4]}"}) -> (tensor<4xf32>, tensor<4xf32>) {
  %0:2 = "mhlo.if"(%arg0) ( {
    mhlo.return %arg1, %arg2 : tensor<4xf32>, tensor<4xf32>
  },  {
    mhlo.return %arg3, %arg4 : tensor<4xf32>, tensor<4xf32>
  }) {mhlo.sharding = "{{replicated},{devices=[4]<=[4]}}"} : (tensor<i1>) -> (tensor<4xf32>, tensor<4xf32>)
  func.return %0#0, %0#1 : tensor<4xf32>, tensor<4xf32>
}

// -----

// CHECK-LABEL: HloModule main

// CHECK:      %[[TRUE:region_0.*]] ([[ARG:Arg_.*]]: f32[4]) -> f32[4] {
// CHECK-NEXT:   ROOT %[[ARG]] = f32[4] parameter(0)

// CHECK:      %[[FALSE:region_1.*]] ([[ARG:Arg_.*]]: f32[4]) -> f32[4] {
// CHECK-NEXT:   ROOT %[[ARG]] = f32[4] parameter(0)

// CHECK:      ENTRY %main.{{.*}} ([[ARG0:Arg_0.*]]: pred[], [[ARG1:Arg_1.*]]: f32[4], [[ARG2:Arg_2.*]]: f32[4]) -> f32[4] {
// CHECK-NEXT:   %[[ARG0]] = pred[] parameter(0)
// CHECK-NEXT:   %[[ARG1]] = f32[4] parameter(1), sharding={devices=[2,2]<=[4] last_tile_dim_replicate}
// CHECK-NEXT:   %[[ARG2]] = f32[4] parameter(2)
// CHECK-NEXT:   ROOT %conditional.{{.*}} = f32[4] conditional(%[[ARG0]], %[[ARG1]], %[[ARG2]]), true_computation=%[[TRUE]], false_computation=%[[FALSE]]

func.func @main(%arg0: tensor<i1>,
                %arg1: tensor<4xf32> {mhlo.sharding = "{devices=[2,2]<=[4] last_tile_dim_replicate}"},
                %arg2: tensor<4xf32>) -> tensor<4xf32> {
  %0 = "mhlo.if"(%arg0) ( {
    mhlo.return %arg1 : tensor<4xf32>
  },  {
    mhlo.return %arg2 : tensor<4xf32>
  }) : (tensor<i1>) -> tensor<4xf32>
  func.return %0 : tensor<4xf32>
}

// -----

// CHECK-LABEL: ENTRY %main.{{.*}} ({{[^,]*}}: f32[5,8,128]) -> (f32[5,8,128], f32[5,8,128])
func.func @main(%arg0: tensor<5x8x128xf32> {mhlo.sharding = "{devices=[1,2,1]0,1}"}) -> (tuple<tensor<5x8x128xf32>, tensor<5x8x128xf32>> {mhlo.sharding = "{{devices=[1,2,1]0,1}, {replicated}}"}) {
  // CHECK-NEXT: %Arg_0.1 = f32[5,8,128] parameter(0), sharding={devices=[1,2,1]0,1}
  // CHECK-NEXT: %custom-call.2 = (f32[5,8,128], f32[5,8,128]) custom-call(%Arg_0.1), custom_call_target="Sharding", sharding={{\{}}{devices=[1,2,1]0,1}, {replicated}}
  // CHECK-NEXT: %tuple.3 = ((f32[5,8,128], f32[5,8,128])) tuple(%custom-call.2)
  // CHECK-SAME: sharding={{\{}}{devices=[1,2,1]0,1}, {replicated}}
  // CHECK-NEXT: ROOT %get-tuple-element.4 = (f32[5,8,128], f32[5,8,128]) get-tuple-element(%tuple.3), index=0
  // CHECK-SAME: sharding={{\{}}{devices=[1,2,1]0,1}, {replicated}}
  %0 = "mhlo.custom_call"(%arg0) {call_target_name = "Sharding",
				  mhlo.sharding = "{{devices=[1,2,1]0,1}, {replicated}}"
				 } : (tensor<5x8x128xf32>) -> (tuple<tensor<5x8x128xf32>, tensor<5x8x128xf32>>)
  func.return %0 : tuple<tensor<5x8x128xf32>, tensor<5x8x128xf32>>
}

// -----

// CHECK-LABEL: ENTRY %main.{{.*}} ({{[^,]*}}: f32[5,8,128]) -> f32[5,8,128]
func.func @main(%arg0: tensor<5x8x128xf32> {mhlo.sharding = "{devices=[1,2,1]0,1}"}) -> (tensor<5x8x128xf32> {mhlo.sharding = "{devices=[2,1,1]0,1}"}) {
  // CHECK-NEXT: %Arg_0.1 = f32[5,8,128] parameter(0), sharding={devices=[1,2,1]0,1}
  // CHECK-NEXT: %tuple.2 = (f32[5,8,128]) tuple(%Arg_0.1)
  // CHECK-SAME: sharding={{\{}}{devices=[1,2,1]0,1}}
  // CHECK-NEXT: ROOT %get-tuple-element.3 = f32[5,8,128] get-tuple-element(%tuple.2), index=0
  // CHECK-SAME: sharding={devices=[2,1,1]0,1}
  func.return %arg0 : tensor<5x8x128xf32>
}
