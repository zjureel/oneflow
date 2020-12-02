/*
Copyright 2020 The OneFlow Authors. All rights reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/
#include "oneflow/core/job_rewriter/job_pass.h"
#include "oneflow/core/job_rewriter/autograd.h"
#include "oneflow/core/job/job_builder.h"
#include "oneflow/core/job/scope.h"
#include "oneflow/core/vm/symbol_storage.h"
#include "oneflow/core/common/util.h"
#include "oneflow/core/common/protobuf.h"
#include "oneflow/core/common/container_util.h"
#include "oneflow/core/common/balanced_splitter.h"
#include "oneflow/core/job/foreign_callback.h"
#include "oneflow/core/framework/interpreter.h"
#include "oneflow/core/framework/user_op_attr.cfg.h"
#include "oneflow/core/framework/instructions_builder.h"

namespace oneflow {

namespace {

class StagePartitionStragety {
 public:
  StagePartitionStragety() = default;
  ~StagePartitionStragety() = default;
  virtual Maybe<void> Apply(Job* job, JobPassCtx* ctx) const = 0;
};

class StagePartitionPass final : public JobPass {
 public:
  StagePartitionPass() = default;
  ~StagePartitionPass() = default;

  Maybe<void> Apply(Job* job, JobPassCtx* ctx) const override {
    if (!IsEnabled(*ctx)) { return Maybe<void>::Ok(); }
    const std::string& partition_strategy = ctx->job_desc().String("stage_partition_strategy");
    std::unique_ptr<const StagePartitionStragety> strategy;
    strategy.reset(NewObj<std::string, StagePartitionStragety>(partition_strategy));
    return strategy->Apply(job, ctx);
  }

  bool IsEnabled(const JobPassCtx& ctx) const {
    return ctx.job_desc().IsTrain() && ctx.job_desc().Bool("enable_stage_partition");
  }
};

REGISTER_JOB_PASS("StagePartition", StagePartitionPass);

#define REGISTER_SSP_PARTITION_STRATEGY(strategy_name, strategy_type)        \
  REGISTER_CLASS_CREATOR(std::string, strategy_name, StagePartitionStragety, \
                         ([] { return new strategy_type(); }));

class DisableStagePartitionStrategy : public StagePartitionStragety {
 public:
  DisableStagePartitionStrategy() = default;
  ~DisableStagePartitionStrategy() = default;

  Maybe<void> Apply(Job* job, JobPassCtx*) const override { return Maybe<void>::Ok(); }
};
REGISTER_SSP_PARTITION_STRATEGY("disable", DisableStagePartitionStrategy);

class NaiveSequantialStagePartitionStrategy : public StagePartitionStragety {
 public:
  NaiveSequantialStagePartitionStrategy() = default;
  ~NaiveSequantialStagePartitionStrategy() = default;

  Maybe<void> Apply(Job* job, JobPassCtx* ctx) const override {
    auto op_graph = JUST(OpGraph::New(*job));
    JobBuilder job_builder(job);
    std::function<Maybe<int64_t>(int64_t old_scope, int64_t stage_scope)> GetMergedScopeSymbolId;
    MakeGetterGetMergedScopeSymbolId(&GetMergedScopeSymbolId);
    JUST(ForEachStageScope4TrainableFwOp(
        op_graph, ctx->job_desc(),
        [&](const OpNode* op_node, int64_t stage_scope_symbol_id) -> Maybe<void> {
          const auto& old_op_conf = op_node->op().op_conf();
          CHECK_OR_RETURN(old_op_conf.has_scope_symbol_id());
          int64_t merged_scope_symbol_id =
              JUST(GetMergedScopeSymbolId(old_op_conf.scope_symbol_id(), stage_scope_symbol_id));
          const auto& merged_scope =
              JUST(Global<vm::SymbolStorage<Scope>>::Get()->MaybeGet(merged_scope_symbol_id));
          // Sets scope_symbol_id
          std::vector<OperatorConf> op_confs(1);
          auto* op_conf = &op_confs.at(0);
          op_conf->CopyFrom(old_op_conf);
          op_conf->set_scope_symbol_id(merged_scope_symbol_id);
          job_builder.MutOpsOnlyOnce(op_confs);
          // Sets parallel_conf
          const auto& parallel_desc = JUST(merged_scope.GetParallelDesc(*op_conf));
          const auto& op_name = op_node->op().op_name();
          job_builder.MutParallelConfOnlyOnce(op_name, parallel_desc.parallel_conf());
          return Maybe<void>::Ok();
        }));
    return Maybe<void>::Ok();
  }

 private:
  void MakeGetterGetMergedScopeSymbolId(
      std::function<Maybe<int64_t>(int64_t old_scope, int64_t stage_scope)>* GetMergedScopeSymbolId)
      const {
    using CacheT = HashMap<std::pair<int64_t, int64_t>, int64_t>;
    auto old7stage2merged = std::make_shared<CacheT>();
    *GetMergedScopeSymbolId = [old7stage2merged, this](int64_t old_scope_id,
                                                       int64_t stage_scope_id) -> Maybe<int64_t> {
      std::pair<int64_t, int64_t> old7stage(old_scope_id, stage_scope_id);
      const auto& iter = old7stage2merged->find(old7stage);
      if (iter != old7stage2merged->end()) { return iter->second; }
      int64_t merge_scope_symbol_id = JUST(MergeScope(old_scope_id, stage_scope_id));
      old7stage2merged->emplace(old7stage, merge_scope_symbol_id);
      return merge_scope_symbol_id;
    };
  }

  // Returns scope_symbol_id
  Maybe<int64_t> MergeScope(int64_t old_scope_id, int64_t stage_scope_id) const {
    const auto& storage = *Global<vm::SymbolStorage<Scope>>::Get();
    const auto& old_scope = JUST(storage.MaybeGet(old_scope_id));
    const auto& stage_scope = JUST(storage.MaybeGet(stage_scope_id));
    cfg::ScopeProto merged_scope;
    merged_scope.InitFromProto(old_scope.scope_proto());
    merged_scope.set_parent_scope_symbol_id(old_scope_id);
    merged_scope.set_device_parallel_desc_symbol_id(
        stage_scope.scope_proto().device_parallel_desc_symbol_id());
    merged_scope.set_host_parallel_desc_symbol_id(
        stage_scope.scope_proto().host_parallel_desc_symbol_id());
    auto* map = merged_scope.mutable_attr_name2attr_value();
    (*map)["stage_placement_id"].set_at_int64(stage_scope.Int64("stage_placement_id"));
    (*map)["stage_weight_buffer_size"].set_at_int64(stage_scope.Int64("stage_weight_buffer_size"));
    int64_t symbol_id = 0;
    JUST(LogicalInterpreter().Run([&](InstructionsBuilder* builder) -> Maybe<void> {
      symbol_id = JUST(builder->FindOrCreateSymbolId<cfg::ScopeProto>(merged_scope));
      return Maybe<void>::Ok();
    }));
    // TODO(lixinqi): Remove this urgly code after most python code migrated into cpp code
    {
      ScopeProto scope_proto;
      merged_scope.ToProto(&scope_proto);
      Global<ForeignCallback>::Get()->AddScopeToPyStorage(symbol_id, scope_proto.DebugString());
    }
    return symbol_id;
  }

  Maybe<void> ForEachStageScope4TrainableFwOp(
      const OpGraph& op_graph, const JobDesc& job_desc,
      const std::function<Maybe<void>(const OpNode*, int64_t scope_symbol_id)>& Handler) const {
    // Sequantialize trainable forward ops
    std::list<std::unique_ptr<std::vector<OpNode*>>> sequantial_trainable_fw_ops;
    JUST(GetSequantialTrainableFwOps(op_graph, &sequantial_trainable_fw_ops));
    // Gets stage partition config
    std::vector<int64_t> stage_partition_scope_ids;
    JUST(GetStagePartitionScopeIds(job_desc, &stage_partition_scope_ids));
    // Partition to stages
    std::function<Maybe<int64_t>(int64_t)> Stage4Depth;
    int64_t num_stages = stage_partition_scope_ids.size();
    JUST(GetStageDepth2Stage(sequantial_trainable_fw_ops, num_stages, &Stage4Depth));
    int64_t depth = 0;
    for (const auto& fused_vec : sequantial_trainable_fw_ops) {
      int64_t stage = JUST(Stage4Depth(depth));
      int64_t scope_symbol_id = JUST(VectorAt(stage_partition_scope_ids, stage));
      for (OpNode* op_node : *fused_vec) { JUST(Handler(op_node, scope_symbol_id)); }
      ++depth;
    }
    return Maybe<void>::Ok();
  }

  Maybe<void> GetSequantialTrainableFwOps(
      const OpGraph& op_graph,
      std::list<std::unique_ptr<std::vector<OpNode*>>>* sequantial_trainable_fw_ops) const {
    HashMap<OpNode*, std::unique_ptr<std::vector<OpNode*>>> backbone_op2fused_ops;
    JUST(GetBackboneOp2FusedOps(op_graph, &backbone_op2fused_ops));
    std::list<OpNode*> starts;
    {
      const auto& ForEachOut = [&](OpNode* node, const std::function<void(OpNode*)>& Handler) {
        node->ForEachNodeOnOutEdge([&](OpNode* out_node) {
          if (backbone_op2fused_ops.count(out_node) > 0) { Handler(out_node); }
        });
      };
      const auto& IsSinkNode = [&](OpNode* node) {
        size_t out_num = 0;
        ForEachOut(node, [&](OpNode*) { ++out_num; });
        return out_num == 0;
      };
      for (const auto& pair : backbone_op2fused_ops) {
        if (IsSinkNode(pair.first)) { starts.push_back(pair.first); }
      }
    }
    const auto& ForEachIn = [&](OpNode* node, const std::function<void(OpNode*)>& Handler) {
      node->ForEachNodeOnInEdge([&](OpNode* in_node) {
        if (backbone_op2fused_ops.count(in_node) > 0) { Handler(in_node); }
      });
    };
    // Traverses reverserly
    op_graph.BfsForEachNode(starts, ForEachIn, [&](OpNode* op_node) {
      const auto& iter = backbone_op2fused_ops.find(op_node);
      CHECK(iter != backbone_op2fused_ops.end());
      sequantial_trainable_fw_ops->emplace_front(std::move(iter->second));
    });
    return Maybe<void>::Ok();
  }

  Maybe<void> GetStageDepth2Stage(
      const std::list<std::unique_ptr<std::vector<OpNode*>>>& sequantial_trainable_fw_ops,
      int64_t num_stages, std::function<Maybe<int64_t>(int64_t)>* Stage4Depth) const {
    int64_t num_ops = 0;
    for (const auto& vec : sequantial_trainable_fw_ops) { num_ops += vec->size(); }
    BalancedSplitter bs(num_ops, num_stages);
    std::vector<int64_t> stage2expected_num_ops_from_start(num_stages);
    for (int64_t i = 0; i < num_stages; ++i) {
      int64_t last = (i == 0 ? 0 : stage2expected_num_ops_from_start.at(i - 1));
      stage2expected_num_ops_from_start.at(i) = bs.At(i).size() + last;
    }
    auto depth2stage = std::make_shared<HashMap<int64_t, int64_t>>();
    {
      int64_t stage = 0;
      int64_t depth = 0;
      int64_t num_ops_from_start = 0;
      for (const auto& vec : sequantial_trainable_fw_ops) {
        if (num_ops_from_start > stage2expected_num_ops_from_start.at(stage)) { ++stage; }
        (*depth2stage)[depth] = stage;
        ++depth;
        num_ops_from_start += vec->size();
      }
      CHECK_EQ(stage, num_stages - 1);
      CHECK_EQ(depth, sequantial_trainable_fw_ops.size());
    }
    *Stage4Depth = [depth2stage](int64_t depth) -> Maybe<int64_t> {
      const auto& iter = depth2stage->find(depth);
      CHECK_OR_RETURN(iter != depth2stage->end());
      return iter->second;
    };
    return Maybe<void>::Ok();
  }

  Maybe<void> GetTrainableFwOps(const OpGraph& op_graph, HashSet<OpNode*>* trainable_fw_ops) const {
    std::function<bool(OpNode*)> NeedBackwardOp;
    JUST(MakePredicatorNeedBackwardOp(op_graph, &NeedBackwardOp));
    op_graph.ForEachNode([&](OpNode* node) {
      if (NeedBackwardOp(node)) { trainable_fw_ops->insert(node); }
    });
    return Maybe<void>::Ok();
  }

  Maybe<void> GetBackboneOp2FusedOps(
      const OpGraph& op_graph,
      HashMap<OpNode*, std::unique_ptr<std::vector<OpNode*>>>* backbone_op2fused_ops) const {
    // Gets trainable forward ops.
    HashSet<OpNode*> trainable_fw_ops;
    JUST(GetTrainableFwOps(op_graph, &trainable_fw_ops));
    // Gets backbone ops.
    HashSet<OpNode*> backbone_op_nodes;
    JUST(GetBackBoneOps(op_graph, trainable_fw_ops, &backbone_op_nodes));
    // Fuses other forward ops to backbone ops.
    HashMap<OpNode*, OpNode*> other_fw_op2backbone_op;
    JUST(FuseOtherFwOpsToBackboneOps(op_graph, backbone_op_nodes, &other_fw_op2backbone_op));
    for (OpNode* backbone_op_node : backbone_op_nodes) {
      (*backbone_op2fused_ops)[backbone_op_node].reset(new std::vector<OpNode*>{backbone_op_node});
    }
    for (const auto& pair : other_fw_op2backbone_op) {
      (*backbone_op2fused_ops)[pair.second]->push_back(pair.first);
    }
    return Maybe<void>::Ok();
  }

  // subgraph trainable_fw_ops can be regarded as DAG whose source nodes are variable op nodes and
  // whose sink nodes are loss op nodes.
  //
  // A op node is called backbone op node in trainable_fw_ops if:
  //    a) it has two input in subgraph trainable_fw_ops;
  //    b) or it has at least one backbone op as input
  Maybe<void> GetBackBoneOps(const OpGraph& op_graph, const HashSet<OpNode*>& trainable_fw_ops,
                             HashSet<OpNode*>* backbone_op_nodes) const {
    std::list<OpNode*> starts;
    {
      const auto& ForEachIn = [&](OpNode* node, const std::function<void(OpNode*)>& Handler) {
        node->ForEachNodeOnInEdge([&](OpNode* in_node) {
          if (trainable_fw_ops.count(in_node) > 0) { Handler(in_node); }
        });
      };
      const auto& GetInputSize = [&](OpNode* node) {
        size_t input_size = 0;
        ForEachIn(node, [&](OpNode*) { ++input_size; });
        return input_size;
      };
      for (OpNode* op_node : trainable_fw_ops) {
        if (GetInputSize(op_node) > 1) { starts.push_back(op_node); }
      }
    }
    const auto& ForEachOut = [&](OpNode* node, const std::function<void(OpNode*)>& Handler) {
      node->ForEachNodeOnOutEdge([&](OpNode* out_node) {
        if (trainable_fw_ops.count(out_node) > 0) { Handler(out_node); }
      });
    };
    op_graph.BfsForEachNode(starts, ForEachOut,
                            [&](OpNode* node) { backbone_op_nodes->insert(node); });
    return Maybe<void>::Ok();
  }

  Maybe<void> FuseOtherFwOpsToBackboneOps(
      const OpGraph& op_graph, const HashSet<OpNode*>& backbone_op_nodes,
      HashMap<OpNode*, OpNode*>* other_fw_op2backbone_op) const {
    const auto& ForEachOtherNext = [&](OpNode* node, const std::function<void(OpNode*)>& Handler) {
      node->ForEachNodeOnInOutEdge([&](OpNode* next_node) {
        if (backbone_op_nodes.count(next_node) > 0) { return; }
        // It's safe to update container other_fw_op2backbone_op when traversing.
        if (other_fw_op2backbone_op->count(next_node) > 0) { return; }
        // Traverses other nodes.
        Handler(next_node);
      });
    };
    JUST(BfsForEachBackboneOp(op_graph, backbone_op_nodes, [&](OpNode* backbone_op_node) {
      op_graph.BfsForEachNode({backbone_op_node}, ForEachOtherNext, [&](OpNode* other) {
        if (backbone_op_nodes.count(other) > 0) { return; }
        (*other_fw_op2backbone_op)[other] = backbone_op_node;
      });
    }));
    return Maybe<void>::Ok();
  }

  Maybe<void> BfsForEachBackboneOp(const OpGraph& op_graph,
                                   const HashSet<OpNode*>& backbone_op_nodes,
                                   const std::function<void(OpNode*)>& Handler) const {
    std::list<OpNode*> starts;
    {
      const auto& ForEachIn = [&](OpNode* node, const std::function<void(OpNode*)>& Handler) {
        node->ForEachNodeOnInEdge([&](OpNode* in_node) {
          if (backbone_op_nodes.count(in_node) > 0) { Handler(in_node); }
        });
      };
      const auto& IsSource = [&](OpNode* node) {
        size_t in_size = 0;
        ForEachIn(node, [&](OpNode*) { ++in_size; });
        return in_size == 0;
      };
      for (OpNode* op_node : backbone_op_nodes) {
        if (IsSource(op_node)) { starts.push_back(op_node); }
      }
    }
    const auto& ForEachOut = [&](OpNode* node, const std::function<void(OpNode*)>& Handler) {
      node->ForEachNodeOnOutEdge([&](OpNode* out_node) {
        if (backbone_op_nodes.count(out_node) > 0) { Handler(out_node); }
      });
    };
    op_graph.BfsForEachNode(starts, ForEachOut, Handler);
    return Maybe<void>::Ok();
  }

  Maybe<void> GetStagePartitionScopeIds(const JobDesc& job_desc,
                                        std::vector<int64_t>* stage_partition_scope_ids) const {
    const auto& scope_ids = job_desc.ListInt64("stage_partition_scope_ids");
    CHECK_GT_OR_RETURN(scope_ids.size(), 0);
    stage_partition_scope_ids->assign(scope_ids.begin(), scope_ids.end());
    return Maybe<void>::Ok();
  }
};
REGISTER_SSP_PARTITION_STRATEGY("naive_sequantial", NaiveSequantialStagePartitionStrategy);

}  // namespace

}  // namespace oneflow
