// clang-format off
/*
 * SPDX-FileCopyrightText: Copyright (c) 2023-present NVIDIA CORPORATION & AFFILIATES.
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
// clang-format on
#pragma once

#include <c10/macros/Export.h>
#include <ir/interface_nodes.h>

#include <fusion.h>
#include <ir/base_nodes.h>
#include <mma_type.h>
#include <parallel_type_bitmap.h>

//! Nodes in here should generally not be used by users. They should be behind
//! the scenes and users shouldn't have to be aware of what they do to use the
//! code generator
//!
//! \todo improve implementation bool IterDomain::sameAs(const IterDomain*)
//! \todo Add testing of sameAs functions for these nodes
//!

//! IR header hierarchy
//! 1. utils.h - PolymorphicBase and NonCopyable
//! 2. ir/base_nodes.h - Statement, Expr, and Val
//! 3. ir/internal_base_nodes.h - IterDomain and TensorDomain
//! 4. ir/interface_nodes.h - TensorView and Scalar
//! 5. ** ir/internal_nodes.h ** - Any internal-only IR nodes

namespace nvfuser {

class ViewTransform;
class Scope;
class IrCloner;
struct AnalyzeViewResult;

//! Returns true if both v1 and v2 are scalars, are the same type of scalars,
//! and dispatches to the inherited Val type's `->sameAs` call. e.g. if both
//! vals are `Int` will dispatch to v1->as<Int>()->sameAs(v2.as<Int>())
bool areEqualScalars(Val* v1, Val* v2);

class TORCH_CUDA_CU_API FullOp : public Expr {
 public:
  using Expr::Expr;

  FullOp(IrBuilderPasskey, Val* out, Val* fill_value);

  NVFUSER_DECLARE_CLONE_AND_CREATE

  const char* getOpString() const override {
    return "FullOp";
  }

  std::string toString(int indent_size = 0) const override;
  std::string toInlineString(int indent_size = 0) const override;

  Val* getFillValue() const {
    return inputs().back();
  }
};

class TORCH_CUDA_CU_API SelectOp : public Expr {
 public:
  using Expr::Expr;

  SelectOp(IrBuilderPasskey, Val* out, Val* in, int64_t dim, Val* index);

  NVFUSER_DECLARE_CLONE_AND_CREATE

  const char* getOpString() const override {
    return "SelectOp";
  }

  std::string toString(int indent_size = 0) const override;
  std::string toInlineString(int indent_size = 0) const override;

  TensorView* lookupTv() const {
    return input(0)->as<TensorView>();
  }

  int64_t dim() const {
    return attribute(0)->as<Attribute<int64_t>>()->value;
  }

  IterDomain* getIndexedID() const;

  std::unordered_map<IterDomain*, Val*> getIndexOverridingMap() const {
    return {{getIndexedID(), input(1)}};
  }
};

class TORCH_CUDA_CU_API IndexSelectOp : public Expr {
 public:
  using Expr::Expr;

  IndexSelectOp(IrBuilderPasskey, Val* out, Val* in1, int64_t dim, Val* in3);

  NVFUSER_DECLARE_CLONE_AND_CREATE

  const char* getOpString() const override {
    return "IndexSelectOp";
  }

  std::string toString(int indent_size = 0) const override;
  std::string toInlineString(int indent_size = 0) const override;

  TensorView* lookupTv() const {
    return input(0)->as<TensorView>();
  }

  TensorView* indexTv() const {
    return input(1)->as<TensorView>();
  }

  IterDomain* getIndexedID() const;

  IterDomain* getConsumerOfIndexedID() const;

  int64_t dim() const {
    return attribute(0)->as<Attribute<int64_t>>()->value;
  }
};

class TORCH_CUDA_CU_API TorchGatherOp : public Expr {
 public:
  using Expr::Expr;

  //! Parameter exact_sizes indicates whether the non-indexed domains
  //! of the index tensor have the same extents of those of the input
  //! tensor. It's true in the case of torch.take_along_dim and
  //! numpy_take_along_axis. torch.take_along_axis does not guarantee
  //! they are the same.
  TorchGatherOp(
      IrBuilderPasskey,
      Val* out,
      Val* in,
      int64_t dim,
      Val* index,
      bool exact_sizes);

  NVFUSER_DECLARE_CLONE_AND_CREATE

  const char* getOpString() const override {
    return "TorchGatherOp";
  }

  std::string toString(int indent_size = 0) const override;
  std::string toInlineString(int indent_size = 0) const override;

  TensorView* lookupTv() const {
    return input(0)->as<TensorView>();
  }

  TensorView* indexTv() const {
    return input(1)->as<TensorView>();
  }

  int64_t dim() const {
    return attribute(0)->as<Attribute<int64_t>>()->value;
  }

  IterDomain* getIndexedID() const;

  IterDomain* getConsumerOfIndexedID() const;

  bool exactSizes() const {
    return attribute(1)->as<Attribute<bool>>()->value;
  }
};

class TORCH_CUDA_CU_API ScatterOp : public Expr {
 public:
  using Expr::Expr;
  ScatterOp(
      IrBuilderPasskey,
      ScatterOpType type,
      Val* out,
      Val* self,
      int64_t dim,
      Val* index,
      Val* src);

  NVFUSER_DECLARE_CLONE_AND_CREATE

  const char* getOpString() const override {
    return "ScatterOp";
  }

  std::string toString(int indent_size = 0) const override;
  std::string toInlineString(int indent_size = 0) const override;

  TensorView* selfTv() const {
    return input(0)->as<TensorView>();
  }

  TensorView* indexTv() const {
    return input(1)->as<TensorView>();
  }

  TensorView* srcTv() const {
    return input(2)->as<TensorView>();
  }

  int64_t dim() const {
    return attribute(0)->as<Attribute<int64_t>>()->value;
  }

  IterDomain* getIndexedID() const;

  ScatterOpType getScatterOpType() const {
    return attribute(1)->as<Attribute<ScatterOpType>>()->value;
  }
};

class TORCH_CUDA_CU_API IotaOp : public Expr {
 public:
  using Expr::Expr;

  IotaOp(IrBuilderPasskey, Val* out, Val* length, Val* start, Val* step);

  NVFUSER_DECLARE_CLONE_AND_CREATE

  const char* getOpString() const override {
    return "IotaOp";
  }

  std::string toString(int indent_size = 0) const override;
  std::string toInlineString(int indent_size = 0) const override;

  DataType dtype() const {
    return *start()->getDataType();
  }

  Val* length() const {
    return input(0);
  }

  Val* start() const {
    return input(1);
  }

  Val* step() const {
    return input(2);
  }
};

// Tensor factory for generating identity matrices like
//
// [[1, 0, 0],
//  [0, 1, 0],
//  [0, 0, 1]]
//
// or
//
// [[1, 0, 0],
//  [0, 1, 0],
//  [0, 0, 1],
//  [0, 0, 0]]
//
// or
//
// [[1, 0, 0, 0],
//  [0, 1, 0, 0],
//  [0, 0, 1, 0]]
class TORCH_CUDA_CU_API EyeOp : public Expr {
 public:
  using Expr::Expr;

  EyeOp(IrBuilderPasskey, Val* out, DataType dtype);

  NVFUSER_DECLARE_CLONE_AND_CREATE

  const char* getOpString() const override {
    return "EyeOp";
  }

  std::string toString(int indent_size = 0) const override;
  std::string toInlineString(int indent_size = 0) const override;

  DataType dtype() const {
    return attribute(0)->as<Attribute<DataType>>()->value;
  }
};

//! A specialization for Unary operations. Unary operations take in a single
//! input and produce a single output. Examples include:
//!   1) Casting operation i.e. float(a_val)
//!   2) Negation i.e. val * -1
//!   3) Reduction across a dimension i.e. val.sum(axis=2)
//!   4) split/merge
class TORCH_CUDA_CU_API UnaryOp : public Expr {
 public:
  using Expr::Expr;

  UnaryOp(IrBuilderPasskey, UnaryOpType type, Val* out, Val* in);

  NVFUSER_DECLARE_CLONE_AND_CREATE

  const char* getOpString() const override {
    return "UnaryOp";
  }

  std::vector<EvaluatorValue> evaluate(
      const std::vector<EvaluatorValue>& inputs) const override;

  std::string toString(int indent_size = 0) const override;
  std::string toInlineString(int indent_size = 0) const override;

  Val* out() const {
    return output(0);
  }
  Val* in() const {
    return input(0);
  }

  UnaryOpType getUnaryOpType() const {
    return attribute(0)->as<Attribute<UnaryOpType>>()->value;
  }

 private:
  void printHelper(std::stringstream& ss, std::string input) const;
};

//! A specialization for Binary operations. Binary operations take in two inputs
//! and produce a single output. Examples include:
//!  1) Add/mul/div/mod/sub (A * B)
//!  2) LT (A < B)
class TORCH_CUDA_CU_API BinaryOp : public Expr {
 public:
  using Expr::Expr;

  BinaryOp(IrBuilderPasskey, BinaryOpType type, Val* out, Val* lhs, Val* rhs);

  NVFUSER_DECLARE_CLONE_AND_CREATE

  const char* getOpString() const override {
    return "BinaryOp";
  }

  std::vector<EvaluatorValue> evaluate(
      const std::vector<EvaluatorValue>& inputs) const override;

  std::string toString(int indent_size = 0) const override;
  std::string toInlineString(int indent_size = 0) const override;

  Val* out() const {
    return output(0);
  }
  Val* lhs() const {
    return input(0);
  }
  Val* rhs() const {
    return input(1);
  }

  BinaryOpType getBinaryOpType() const {
    return attribute(0)->as<Attribute<BinaryOpType>>()->value;
  }

 private:
  void printHelper(
      std::stringstream& ss,
      int indent_size,
      std::string lhs,
      std::string rhs) const;
};

class TORCH_CUDA_CU_API TernaryOp : public Expr {
 public:
  using Expr::Expr;

  TernaryOp(
      IrBuilderPasskey,
      TernaryOpType type,
      Val* out,
      Val* in1,
      Val* in2,
      Val* in3);

  NVFUSER_DECLARE_CLONE_AND_CREATE

  const char* getOpString() const override {
    return "TernaryOp";
  }

  std::vector<EvaluatorValue> evaluate(
      const std::vector<EvaluatorValue>& inputs) const override;

  std::string toString(int indent_size = 0) const override;
  std::string toInlineString(int indent_size = 0) const override;

  Val* out() const {
    return output(0);
  }

  Val* in1() const {
    return input(0);
  }
  Val* in2() const {
    return input(1);
  }
  Val* in3() const {
    return input(2);
  }

  TernaryOpType getTernaryOpType() const {
    return attribute(0)->as<Attribute<TernaryOpType>>()->value;
  }

 private:
  void printHelper(
      std::stringstream& ss,
      int indent_size,
      std::string in1,
      std::string in2,
      std::string in3) const;
};

//! A specialization for random number generator (RNG) operations. RNG
//! operations take in no tensor input and produce a single output.
class TORCH_CUDA_CU_API RNGOp : public Expr {
  int64_t getOutputDims() const;

 public:
  struct Attributes {
    // default initialization for clang-tidy
    // cppcoreguidelines-pro-type-member-init
    RNGOpType rtype = RNGOpType::Undefined;
    DataType dtype;
    int rng_offset = 0;

    // TODO: Enable the following in C++20:
    // bool operator==(const Attributes &other) const = default;
    bool operator==(const Attributes& other) const {
      return rtype == other.rtype && dtype == other.dtype &&
          rng_offset == other.rng_offset;
    }
  };

  using Expr::Expr;

  RNGOp(
      IrBuilderPasskey,
      RNGOpType type,
      Val* out,
      DataType dtype,
      std::vector<Val*> parameters = {},
      int rng_offset = 0,
      Val* philox_index = nullptr);

  NVFUSER_DECLARE_CLONE_AND_CREATE

  const char* getOpString() const override {
    return "RNGOp";
  }

  std::string toString(int indent_size = 0) const override;
  std::string toInlineString(int indent_size = 0) const override;

  RNGOpType getRNGOpType() const {
    return attribute(0)->as<Attribute<Attributes>>()->value.rtype;
  }

  DataType dtype() const {
    return attribute(0)->as<Attribute<Attributes>>()->value.dtype;
  }

  int getRNGOffset() const {
    return attribute(0)->as<Attribute<Attributes>>()->value.rng_offset;
  }

  void setRNGOffset(int val) {
    attribute(0)->as<Attribute<Attributes>>()->value.rng_offset = val;
  }

  std::vector<Val*> getParameters() const {
    return {inputs().begin() + getOutputDims(), inputs().end()};
  }

  std::vector<Val*> getShape() const {
    return {inputs().begin(), inputs().begin() + getOutputDims()};
  }

  Val* getPhiloxIndex() const {
    return attributeVal(1);
  }

  int getPhiloxMultiple() const {
    return dtype() == DataType::Double ? 2 : 4;
  }
};

//! Broadcast in to match out. is_broadcast_dims are relative to out. Where
//! is_broadcast_dims.size() == out->nDims().
class TORCH_CUDA_CU_API BroadcastOp : public Expr {
 public:
  using Expr::Expr;

  //! \param out The output tensor
  //! \param in The input tensor
  //! \param is_broadcast_dims True when output dim is a new broadcast domain
  BroadcastOp(
      IrBuilderPasskey,
      Val* out,
      Val* in,
      std::vector<bool> is_broadcast_dims);

  NVFUSER_DECLARE_CLONE_AND_CREATE

  const char* getOpString() const override {
    return "BroadcastOp";
  }

  std::string toString(int indent_size = 0) const override;
  std::string toInlineString(int indent_size = 0) const override;

  Val* out() const {
    return output(0);
  }
  Val* in() const {
    return input(0);
  }

  bool isBroadcastDim(size_t dim) const {
    return getBroadcastDimFlags().at(dim);
  }

  //! The same list passed to the broadcast arithmetic op. Each
  //! element corresponds to an IterDomain of the output tensor and is
  //! true when the IterDomain is a new broadcast domain. Note
  //! that the output tensor may have other broadcast domains whose
  //! flags are false because the input tensor may already have
  //! broadcast domains.
  const std::vector<bool>& getBroadcastDimFlags() const {
    return attribute(0)->as<Attribute<std::vector<bool>>>()->value;
  }
};

//! Squeeze in to match out. is_squeeze_dims are relative to in. Where
//! is_squeeze_dims.size() == in->nDims(). Squeeze is the opposite of
//! broadcast.
class TORCH_CUDA_CU_API SqueezeOp : public Expr {
 public:
  using Expr::Expr;

  //! \param out The output tensor
  //! \param in The input tensor
  //! \param is_squeeze_dims True when input dim is a removed broadcast domain
  SqueezeOp(
      IrBuilderPasskey,
      Val* out,
      Val* in,
      std::vector<bool> is_broadcast_dims);

  NVFUSER_DECLARE_CLONE_AND_CREATE

  const char* getOpString() const override {
    return "SqueezeOp";
  }

  std::string toString(int indent_size = 0) const override;
  std::string toInlineString(int indent_size = 0) const override;

  Val* out() const {
    return output(0);
  }
  Val* in() const {
    return input(0);
  }

  bool isSqueezeDim(size_t dim) const {
    return getSqueezeDimFlags().at(dim);
  }

  //! The same list passed to the squeeze arithmetic op. Each
  //! element corresponds to an IterDomain of the input tensor and is
  //! true when the IterDomain is a broadcast domain that is removed in the
  //! output. Note that the output tensor may still contain broadcast domains
  //! because the input tensor may have broadcast domains that we don't want to
  //! remove (false flag).
  const std::vector<bool>& getSqueezeDimFlags() const {
    return attribute(0)->as<Attribute<std::vector<bool>>>()->value;
  }
};

//! Reduction operation. Out is first initialized to _init. Then
//! reduction_op_type is used to update out as out = reductionOp(out, in).
//! Output's axes marked as reduction will be reduced to produce an output
//! tensor. The output tensors size will be the size of all
//! non-reduction/non-broadcast dimensions.
class TORCH_CUDA_CU_API ReductionOp : public Expr {
 public:
  using Expr::Expr;

  ReductionOp(
      IrBuilderPasskey,
      BinaryOpType reduction_op_type,
      Val* init,
      Val* out,
      Val* in,
      bool is_allreduce = false);

  NVFUSER_DECLARE_CLONE_AND_CREATE

  const char* getOpString() const override {
    return "ReductionOp";
  }

  std::string toString(int indent_size = 0) const override;
  std::string toInlineString(int indent_size = 0) const override;

  Val* out() const {
    return output(0);
  }
  Val* in() const {
    return input(0);
  }
  Val* init() const {
    return attributeVal(0);
  }

  BinaryOpType getReductionOpType() const {
    return attribute(1)->as<Attribute<BinaryOpType>>()->value;
  }

  bool isAllreduce() const {
    return attribute(2)->as<Attribute<bool>>()->value;
  }
};

//! Grouped reduction operation for horizontal fusions. It works like
//! batched GEMMs in the sense that multiple independent reductions are
//! performed together. The main benefit is when reducing tensors across thread
//! blocks, a single grid sync can be done for all individual
//! reductions. As grid sync is very expensive, this can be a
//! significant performance impact.
class TORCH_CUDA_CU_API GroupedReductionOp : public Expr {
 public:
  using Expr::Expr;

  GroupedReductionOp(
      IrBuilderPasskey,
      std::vector<BinaryOpType> reduction_op_types,
      std::vector<Val*> init,
      std::vector<Val*> out,
      std::vector<Val*> in,
      bool is_allreduce = false);

  NVFUSER_DECLARE_CLONE_AND_CREATE

  const char* getOpString() const override {
    return "GroupedReductionOp";
  }

  std::string toString(int indent_size = 0) const override;
  std::string toInlineString(int indent_size = 0) const override;

  //! Number of expressions grouped horizontally. It does not reflect
  //! iteration grouping.
  size_t numHorizontallyGroupedExprs() const {
    return getReductionOpTypes().size();
  }

  std::vector<Val*> initVals() const {
    auto size = numHorizontallyGroupedExprs();
    std::vector<Val*> result;
    result.reserve(size);
    for (auto i : c10::irange(2, 2 + size)) {
      result.emplace_back(attribute(i)->as<Val>());
    }
    return result;
  }

  Val* initVal(size_t index) const {
    return attributeVal(2 + index);
  }

  const std::vector<BinaryOpType>& getReductionOpTypes() const {
    return attribute(0)->as<Attribute<std::vector<BinaryOpType>>>()->value;
  }

  BinaryOpType getReductionOpType(size_t index) const {
    return getReductionOpTypes().at(index);
  }

  bool isAllreduce() const {
    return attribute(1)->as<Attribute<bool>>()->value;
  }

  //! Return the index of the corresponding reduction expression for
  //! a given output val.
  int getExprIndexOfOutput(Val* output_val) const;
};

//! Average, variance and N (count) vals for Welford
class TORCH_CUDA_CU_API WelfordTriplet {
 public:
  //! Names of the Welford triplet vals
  enum class ValName { Avg, Var, N };

  WelfordTriplet() = default;

  WelfordTriplet(Val* avg, Val* var, Val* N) : vals_({avg, var, N}) {}

  Val* const& avg() const {
    return get(ValName::Avg);
  }

  Val*& avg() {
    return get(ValName::Avg);
  }

  TensorView* avgTv() const {
    TORCH_INTERNAL_ASSERT(avg()->isA<TensorView>());
    return avg()->as<TensorView>();
  }

  Val* const& var() const {
    return get(ValName::Var);
  }

  Val*& var() {
    return get(ValName::Var);
  }

  TensorView* varTv() const {
    TORCH_INTERNAL_ASSERT(var()->isA<TensorView>());
    return var()->as<TensorView>();
  }

  Val* const& N() const {
    return get(ValName::N);
  }

  Val*& N() {
    return get(ValName::N);
  }

  TensorView* NTv() const {
    TORCH_INTERNAL_ASSERT(N()->isA<TensorView>());
    return N()->as<TensorView>();
  }

  //! Get the i-th val. Ordering is defined by ValName.
  Val* const& get(int i) const {
    return vals_.at(i);
  }

  //! Get the i-th val. Ordering is defined by ValName.
  Val*& get(int i) {
    return vals_.at(i);
  }

  Val* const& get(ValName name) const {
    return get(valNameToIndex(name));
  }

  Val*& get(ValName name) {
    return get(valNameToIndex(name));
  }

  //! Get the name of a given val in this triplet. None is returned if
  //! not found.
  c10::optional<ValName> getNameOf(Val* val) const;

  //! Return a new triplet with outputs produced by a function applied
  //! to each of this triplet
  template <typename Func>
  WelfordTriplet transform(Func func) const {
    return WelfordTriplet(func(avg()), func(var()), func(N()));
  }

  bool sameAs(const WelfordTriplet& other) const;

  WelfordTriplet clone(IrCloner* ir_cloner) const;

  //! Clone a vector of triplets
  static std::vector<WelfordTriplet> clone(
      const std::vector<WelfordTriplet>& src,
      IrCloner* ir_cloner);

  auto begin() {
    return vals_.begin();
  }

  auto begin() const {
    return vals_.begin();
  }

  auto end() {
    return vals_.end();
  }

  auto end() const {
    return vals_.end();
  }

 private:
  //! Convert a given val name to an index
  static int valNameToIndex(ValName name) {
    return static_cast<int>(name);
  }

  //! Convert a given index to a name
  static ValName indexToValName(int index) {
    TORCH_INTERNAL_ASSERT(index >= 0 && index < 3, "Invalid index: ", index);
    return static_cast<ValName>(index);
  }

 private:
  //! Holds avg, var and N in this order
  std::array<Val*, 3> vals_ = {{nullptr, nullptr, nullptr}};
};

//! Welford Scan operation.
class TORCH_CUDA_CU_API WelfordOp : public Expr {
 public:
  using Expr::Expr;
  static constexpr int kNumAttrs = 4;

  WelfordOp(
      IrBuilderPasskey,
      const WelfordTriplet& output,
      const WelfordTriplet& input,
      const WelfordTriplet& init,
      bool is_fused = false);

  WelfordOp(
      IrBuilderPasskey,
      Val* out_avg,
      Val* out_var,
      Val* out_N,
      Val* in_avg,
      Val* in_var,
      Val* in_N,
      Val* init_avg,
      Val* init_var,
      Val* init_N,
      bool is_fused = false);

  NVFUSER_DECLARE_CLONE_AND_CREATE

  const char* getOpString() const override {
    return "WelfordOp";
  }

  std::string toString(int indent_size = 0) const override;
  std::string toInlineString(int indent_size = 0) const override;

  Val* out() const {
    return outputTriplet().avg();
  }

  Val* in() const {
    return inputTriplet().avg();
  }

  WelfordTriplet outputTriplet() const {
    return WelfordTriplet(outAvg(), outVar(), outN());
  }

  Val* outAvg() const {
    return output(0);
  }

  Val* outVar() const {
    return output(1);
  }

  Val* outN() const {
    return output(2);
  }

  WelfordTriplet inputTriplet() const {
    return WelfordTriplet(inAvg(), inVar(), inN());
  }

  Val* inAvg() const {
    return input(0);
  }

  Val* inVar() const {
    return input(1);
  }

  Val* inN() const {
    return input(2);
  }

  WelfordTriplet initTriplet() const {
    return WelfordTriplet(initAvg(), initVar(), initN());
  }

  Val* initAvg() const {
    return attributeVal(0);
  }

  Val* initVar() const {
    return attributeVal(1);
  }

  Val* initN() const {
    return attributeVal(2);
  }

  bool singleValue() const {
    return inN()->isOneInt();
  }

  bool hasInit() const {
    return !initN()->isZeroInt();
  }

  //! True if using the fused reduction kernel (not implemented yet)
  bool isAllreduce() const {
    return attribute(3)->as<Attribute<bool>>()->value;
  }

  std::vector<Val*> getInitVals() const;

  //! Return the init val for an output val
  Val* getInitValOfOutput(Val* output_val) const;
};

class TORCH_CUDA_CU_API GroupedWelfordOp : public Expr {
 public:
  using Expr::Expr;

  GroupedWelfordOp(
      IrBuilderPasskey,
      std::vector<WelfordTriplet> output_vals,
      std::vector<WelfordTriplet> input_vals,
      std::vector<WelfordTriplet> init_vals,
      bool is_allreduce = false);

  NVFUSER_DECLARE_CLONE_AND_CREATE

  const char* getOpString() const override {
    return "GroupedWelfordOp";
  }

  std::string toString(int indent_size = 0) const override;
  std::string toInlineString(int indent_size = 0) const override;

  //! Number of expressions grouped horizontally. It does not reflect
  //! iteration grouping. As horizontal grouping is not supported,
  //! this always returns 1.
  size_t numHorizontallyGroupedExprs() const {
    return 1;
  }

  Val* out(size_t index) const {
    return outAvg(index);
  }

  Val* in(size_t index) const {
    return inAvg(index);
  }

  std::vector<WelfordTriplet> outputVals() const {
    std::vector<WelfordTriplet> result;
    auto size = outputs().size() / 3;
    result.reserve(size);
    for (auto i : c10::irange(size)) {
      result.emplace_back(outAvg(i), outVar(i), outN(i));
    }
    return result;
  }

  std::vector<WelfordTriplet> inputVals() const {
    std::vector<WelfordTriplet> result;
    auto size = inputs().size() / 3;
    result.reserve(size);
    for (auto i : c10::irange(size)) {
      result.emplace_back(inAvg(i), inVar(i), inN(i));
    }
    return result;
  }

  std::vector<WelfordTriplet> initVals() const {
    std::vector<WelfordTriplet> result;
    auto size = inputs().size() / 3;
    result.reserve(size);
    for (auto i : c10::irange(size)) {
      result.emplace_back(initAvg(i), initVar(i), initN(i));
    }
    return result;
  }

  Val* outAvg(size_t index) const {
    return output(index * 3);
  }

  Val* outVar(size_t index) const {
    return output(index * 3 + 1);
  }

  Val* outN(size_t index) const {
    return output(index * 3 + 2);
  }

  Val* inAvg(size_t index) const {
    return input(index * 3);
  }

  Val* inVar(size_t index) const {
    return input(index * 3 + 1);
  }

  Val* inN(size_t index) const {
    return input(index * 3 + 2);
  }

  Val* initAvg(size_t index) const {
    return attributeVal(1 + index * 3);
  }

  Val* initVar(size_t index) const {
    return attributeVal(2 + index * 3);
  }

  Val* initN(size_t index) const {
    return attributeVal(3 + index * 3);
  }

  //! Return the index of the corresponding welford expression for
  //! a given output val
  int getExprIndexOfOutput(Val* output_val) const;

  //! Return the init val for an output val
  Val* getInitValOfOutput(Val* output_val) const;

  bool singleValue(size_t index) const {
    return inN(index)->isOneInt();
  }

  bool hasInit(size_t index) const {
    return !initN(index)->isZeroInt();
  }

  bool isAllreduce() const {
    return attribute(0)->as<Attribute<bool>>()->value;
  }
};

//! Fused Matmul operation
class TORCH_CUDA_CU_API MmaOp : public Expr {
 public:
  // This is a temporary data structure to for the
  //  scheduling specific parameters that we still need
  //  to store on an mma node. Eventually will only be
  //  the mma macro type that will stay on the IR node
  //  after additional cleaning ups.
  struct OptionsInMma {
    MmaOptions::MacroType macro = MmaOptions::MacroType::NoMMA;
    int accumulator_stride = 0;

    bool operator==(const OptionsInMma& other) const {
      return macro == other.macro &&
          accumulator_stride == other.accumulator_stride;
    }
  };

  using AxesData = std::vector<int64_t>;
  using MmaLayoutOpt = std::optional<MmaOptions::MmaLayout>;
  using Expr::Expr;

  MmaOp(IrBuilderPasskey, Val* out, Val* in_a, Val* in_b, Val* init);

  MmaOp(
      IrBuilderPasskey,
      Val* out,
      Val* in_a,
      Val* in_b,
      Val* init,
      const OptionsInMma& options,
      const MmaLayoutOpt& input_layout);

  NVFUSER_DECLARE_CLONE_AND_CREATE

  const char* getOpString() const override {
    return "MmaOp";
  }

  std::string toString(int indent_size = 0) const override;
  std::string toInlineString(int indent_size = 0) const override;

  Val* out() const {
    return output(0);
  }

  Val* inA() const {
    return input(0);
  }

  Val* inB() const {
    return input(1);
  }

  Val* init() const {
    return attributeVal(0);
  }

  const auto& options() const {
    return attribute(ATTR_POS_OPTS)->as<Attribute<OptionsInMma>>()->value;
  }

  auto accStride() const {
    return options().accumulator_stride;
  }

  void configureOptions(MmaOptions options);

  auto layout() const {
    return attribute(ATTR_POS_INPUT_LAYOUT)
        ->as<Attribute<MmaLayoutOpt>>()
        ->value;
  }

  const auto& mAxes() const {
    return attribute(ATTR_POS_M_AXES)->as<Attribute<AxesData>>()->value;
  }

  const auto& nAxes() const {
    return attribute(ATTR_POS_N_AXES)->as<Attribute<AxesData>>()->value;
  }

  const auto& kAxes() const {
    return attribute(ATTR_POS_K_AXES)->as<Attribute<AxesData>>()->value;
  }

  const auto& batchAxes() const {
    return attribute(ATTR_POS_BATCH_AXES)->as<Attribute<AxesData>>()->value;
  }

 private:
  // Predefined idexes of attributes stored for this IR node, to avoid
  //  magic numbers, based on order in which attributes are initialized
  //  in constructor
  static constexpr size_t ATTR_POS_INIT = 0;
  static constexpr size_t ATTR_POS_OPTS = 1;
  static constexpr size_t ATTR_POS_M_AXES = 2;
  static constexpr size_t ATTR_POS_N_AXES = 3;
  static constexpr size_t ATTR_POS_K_AXES = 4;
  static constexpr size_t ATTR_POS_BATCH_AXES = 5;
  static constexpr size_t ATTR_POS_INPUT_LAYOUT = 6;
};

class TORCH_CUDA_CU_API ExpandOp : public Expr {
 public:
  using Expr::Expr;

  ExpandOp(
      IrBuilderPasskey,
      TensorView* out,
      TensorView* in,
      std::vector<Val*> _expanded_extents);

  NVFUSER_DECLARE_CLONE_AND_CREATE

  const char* getOpString() const override {
    return "ExpandOp";
  }

  std::string toString(int indent_size = 0) const override;
  std::string toInlineString(int indent_size = 0) const override;

  TensorView* out() const {
    return output(0)->as<TensorView>();
  }

  TensorView* in() const {
    return input(0)->as<TensorView>();
  }

  std::vector<Val*> expanded_extents() const {
    return {inputs().begin() + 1, inputs().end()};
  }
};

//! Shift
class TORCH_CUDA_CU_API ShiftOp : public Expr {
 public:
  using Expr::Expr;

  //! \param out
  //! \param in
  //! \param offsets
  ShiftOp(
      IrBuilderPasskey,
      Val* out,
      Val* in,
      std::vector<int> offsets,
      std::vector<int> pad_width);

  NVFUSER_DECLARE_CLONE_AND_CREATE

  const char* getOpString() const override {
    return "ShiftOp";
  }

  std::string toString(int indent_size = 0) const override;
  std::string toInlineString(int indent_size = 0) const override;

  Val* out() const {
    return output(0);
  }
  Val* in() const {
    return input(0);
  }

  int offset(size_t dim) const {
    return offsets().at(dim);
  }

  //! Each of the root axes is shifted by the corresponding value of
  //! offsets. The sign of each value indicates the direction of shifting.
  const std::vector<int>& offsets() const {
    return attribute(0)->as<Attribute<std::vector<int>>>()->value;
  }

  const std::vector<int>& padWidth() const {
    return attribute(1)->as<Attribute<std::vector<int>>>()->value;
  }

  bool hasPadding() const {
    return std::any_of(padWidth().begin(), padWidth().end(), [](const auto p) {
      return p > 0;
    });
  }
};

//! Gather a window around each element.
class TORCH_CUDA_CU_API GatherOp : public Expr {
 public:
  using Expr::Expr;

  GatherOp(
      IrBuilderPasskey,
      Val* out,
      Val* in,
      std::vector<int> window_shape,
      std::vector<std::vector<int>> pad_width);

  NVFUSER_DECLARE_CLONE_AND_CREATE

  const char* getOpString() const override {
    return "GatherOp";
  }

  std::string toString(int indent_size = 0) const override;
  std::string toInlineString(int indent_size = 0) const override;

  Val* out() const {
    return output(0);
  }
  Val* in() const {
    return input(0);
  }

  //! Shape of a window gathered for each element.
  const auto& windowShape() const {
    return attribute(0)->as<Attribute<std::vector<int>>>()->value;
  }

  //! Returns the gather axis that corresponds to an input axis
  int64_t gatherAxis(int64_t axis) const;

  //! The size of zero-padding of each axis.
  const auto& padWidth() const {
    return attribute(1)->as<Attribute<std::vector<std::vector<int>>>>()->value;
  }

  bool hasPadding() const {
    return std::any_of(padWidth().begin(), padWidth().end(), [](const auto& p) {
      return p[0] > 0 || p[1] > 0;
    });
  }
};

class TORCH_CUDA_CU_API ViewAsScalar : public Expr {
 public:
  using Expr::Expr;

  ViewAsScalar(
      IrBuilderPasskey,
      Val* out,
      Val* in,
      IterDomain* vector_id,
      Val* index = nullptr);

  NVFUSER_DECLARE_CLONE_AND_CREATE

  const char* getOpString() const override {
    return "ViewAsScalar";
  }

  std::string toString(int indent_size = 0) const override;
  std::string toInlineString(int indent_size = 0) const override;

  Val* out() const {
    return output(0);
  }

  Val* in() const {
    return input(0);
  }

  // The IterDomain of type VectorComponent newly appended to the output
  IterDomain* vector_id() const {
    return attribute(0)->as<IterDomain>();
  }

  // The index that vector_id_ is lowered into
  Val* index() const {
    return attributeVal(1);
  }
};

class TORCH_CUDA_CU_API ViewOp : public Expr {
 public:
  using Expr::Expr;

  ViewOp(IrBuilderPasskey, Val* out, Val* in);

  NVFUSER_DECLARE_CLONE_AND_CREATE

  const char* getOpString() const override {
    return "ViewOp";
  }

  std::string toString(int indent_size = 0) const override;
  std::string toInlineString(int indent_size = 0) const override;

  Val* out() const {
    return output(0);
  }

  Val* in() const {
    return input(0);
  }
};

//! This operator explicitly models data movement between
//!   state spaces on GPU. Currently the modeled state spaces include
//!   global memory, shared memory and register.
//!
//! The main usage of this op is to facilitate generation of hardware
//!   accelerated memory ops, i.e. ldmatrix, cp.async and more to come.
class TORCH_CUDA_CU_API LoadStoreOp : public Expr {
 public:
  using Expr::Expr;

  LoadStoreOp(IrBuilderPasskey, LoadStoreOpType op_type, Val* out, Val* in);

  NVFUSER_DECLARE_CLONE_AND_CREATE

  const char* getOpString() const override {
    return "LoadStoreOp";
  }

  std::vector<EvaluatorValue> evaluate(
      const std::vector<EvaluatorValue>& inputs) const override;

  std::string toString(int indent_size = 0) const override;
  std::string toInlineString(int indent_size = 0) const override;

  Val* out() const {
    return output(0);
  }

  Val* in() const {
    return input(0);
  }

  LoadStoreOpType opType() const {
    return attribute(0)->as<Attribute<LoadStoreOpType>>()->value;
  }

  bool hasTranspose() const;

  void setOpType(LoadStoreOpType op) {
    attribute(0)->as<Attribute<LoadStoreOpType>>()->value = op;
  }
};

//! Representation a split on an IterDomain by "factor"
//! inner_split dictates if the factor section of the split should be inside the
//! remainer or outside.
class TORCH_CUDA_CU_API Split : public Expr {
 public:
  using Expr::Expr;

  // start_offset and stop_offset are used to express partial
  // split. Only the partial domain from start_offset to stop_offset
  // is split and the outer sub-regions are ignored. Note that both
  // start_offset and stop_offset are distance from the left end and
  // right ends, respectively.
  Split(
      IrBuilderPasskey,
      IterDomain* outer,
      IterDomain* inner,
      IterDomain* in,
      Val* factor,
      bool inner_split = true,
      Val* start_offset = nullptr,
      Val* stop_offset = nullptr);

  NVFUSER_DECLARE_CLONE_AND_CREATE

  const char* getOpString() const override {
    return "Split";
  }

  std::string toString(int indent_size = 0) const override;
  std::string toInlineString(int indent_size = 0) const override;

  IterDomain* outer() const {
    return output(0)->as<IterDomain>();
  }
  IterDomain* inner() const {
    return output(1)->as<IterDomain>();
  }
  IterDomain* in() const {
    return input(0)->as<IterDomain>();
  }
  Val* factor() const {
    return attributeVal(0);
  }

  bool innerSplit() const {
    return attribute(1)->as<Attribute<bool>>()->value;
  }

  //! Start position of the input domain. Non-zero means partial
  //! split. Elements until this offset are ignored.
  Val* startOffset() const {
    TORCH_INTERNAL_ASSERT(attributeVal(2) != nullptr);
    return attributeVal(2);
  }

  //! Offset from extent of the input domain. Non-zero means partial
  //! split. Elements after this offset are ignored.
  Val* stopOffset() const {
    TORCH_INTERNAL_ASSERT(attributeVal(3) != nullptr);
    return attributeVal(3);
  }

  //! Utility function to compute the split extent.
  static Val* extent(Val* in_extent, Val* start_offset, Val* stop_offset);
};

//! Merge the IterDomains outer and inner into one domain, outer and inner
//! dictate which will be traversed first (inner). Both IterDomains must be of
//! the same iter or reduction type, as well as the same parallelization
//! strategy if there is one
class TORCH_CUDA_CU_API Merge : public Expr {
 public:
  using Expr::Expr;

  Merge(
      IrBuilderPasskey,
      IterDomain* out,
      IterDomain* outer,
      IterDomain* inner);

  NVFUSER_DECLARE_CLONE_AND_CREATE

  const char* getOpString() const override {
    return "Merge";
  }

  std::string toString(int indent_size = 0) const override;
  std::string toInlineString(int indent_size = 0) const override;

  IterDomain* out() const {
    return output(0)->as<IterDomain>();
  }
  IterDomain* outer() const {
    return input(0)->as<IterDomain>();
  }
  IterDomain* inner() const {
    return input(1)->as<IterDomain>();
  }
};

//! Applies 2D swizzles on a rectangular tile defined by 2 iterdomains.
class TORCH_CUDA_CU_API Swizzle2D : public Expr {
 public:
  using Expr::Expr;

  Swizzle2D(
      IrBuilderPasskey,
      IterDomain* out_x,
      IterDomain* out_y,
      IterDomain* in_x,
      IterDomain* in_y,
      Swizzle2DType swizzle_type = Swizzle2DType::NoSwizzle,
      SwizzleMode swizzle_mode = SwizzleMode::Data);

  NVFUSER_DECLARE_CLONE_AND_CREATE

  const char* getOpString() const override {
    return "Swizzle2D";
  }

  std::string toString(int indent_size = 0) const override;
  std::string toInlineString(int indent_size = 0) const override;

  // Output iterdomain pair corresponding
  //  to the original input iterdomain pair.
  IterDomain* outX() const {
    return output(0)->as<IterDomain>();
  }

  IterDomain* outY() const {
    return output(1)->as<IterDomain>();
  }

  // Input iterdomain pair.
  IterDomain* inX() const {
    return input(0)->as<IterDomain>();
  }

  IterDomain* inY() const {
    return input(1)->as<IterDomain>();
  }

  // The type of predefined 1-to-1 functions
  //  used for swizzling math.
  auto swizzleType() const {
    return attribute(0)->as<Attribute<Swizzle2DType>>()->value;
  }

  // Swizzle mode of this swizzle instance.
  // [Note on swizzle mode]
  // On the current implementations we support two modes of
  //  swizzle math, namely, data mode and loop mode.
  // `Data` mode swizzling is a swizzle that will change the
  //  data layout in shared memory, likely in global memory buffers
  //  as well in the future. see also IndexSwizzle in index_compute.cpp.
  //
  //  Most important use cases are transpose bank conflict removal, and mma
  //  swizzled shared memory layout. Example illustrated in 1D case:
  //
  // for (int i = 0; i<I; i++){
  //   # This is a `Data` mode swizzle.
  //  Tshared [swizzled(i)] = Tin[i];
  // }
  // # Now Tshared holds swizzled data, i.e. the data layout of
  //    Tshared does not map to Tin with affine relationships.
  //
  // for(int i=0;i<I;i++){
  //   Tout = Tshared[swizzled(i)];
  // }
  //
  // `Loop` mode swizzling does not affect the data layout of any buffer
  //   but only permutes the iteration order of serial or parallel loop.
  // This is useful when we want to designate non-affine mapping of thread
  //   to data or we want to generate non-affine loops.
  // Exampe illustrated in 1D case:
  //   for (int i = 0; i<I; i++){
  //     # This is a `Loop` mode swizzle
  //    Tshared [swizzled(i)] = Tin[swizzled(i)];
  //   }
  // # Now Tshared holds normal data, i.e. it still has
  //   the same data layout as if the swizzle wasn't there.
  //
  // # Consumers of Tshared does not need to know about the
  //   loop swizzle at previous op if not inlined.
  // for(int i=0;i<I;i++){
  //   Tout = Tshared[i];
  // }
  //  TODO: Loop swizzles eventually will be piped through in all mappings
  //  and replay of the fusion IR infrastructure.
  auto swizzleMode() const {
    return attribute(1)->as<Attribute<SwizzleMode>>()->value;
  }
};

//! IterDomain expression to resize
class TORCH_CUDA_CU_API Resize : public Expr {
 public:
  using Expr::Expr;

  // Expand the input domain by left_expand and right_expand for each
  // of the start and end sides, respectively
  Resize(
      IrBuilderPasskey,
      IterDomain* out,
      IterDomain* in,
      Val* left_expand,
      Val* right_expand);

  NVFUSER_DECLARE_CLONE_AND_CREATE

  const char* getOpString() const override {
    return "Resize";
  }

  std::string toString(int indent_size = 0) const override;
  std::string toInlineString(int indent_size = 0) const override;

  IterDomain* out() const {
    return output(0)->as<IterDomain>();
  }

  IterDomain* in() const {
    return input(0)->as<IterDomain>();
  }

  Val* leftExpand() const {
    return attributeVal(0);
  }

  Val* rightExpand() const {
    return attributeVal(1);
  }
};

//! Integer value which has a special name
//!
//! These could be:
//! - threadIdx.x
//! - blockIdx.y
//! - blockDim.z
//! - T3.stride[2]
//!
class TORCH_CUDA_CU_API NamedScalar : public Val {
 public:
  NamedScalar(IrBuilderPasskey passkey, std::string name, DataType dtype);

  NamedScalar(const NamedScalar* src, IrCloner* ir_cloner);

  NVFUSER_DECLARE_CLONE

  const std::string& name() const {
    return name_;
  }

  bool sameAs(const Statement* other) const override;

  std::string toString(int indent_size = 0) const override {
    return name_;
  }

  std::string toInlineString(int indent_size = 0) const override {
    return name_;
  }

  //! Check if this is something like T0.size[1]
  bool isTensorSize() const;

  //! Check if this is something like T0.stride[1]
  bool isTensorStride() const;

  //! Check if this is threadIdx.{x,y,z}
  bool isThreadIdx() const {
    auto p = getParallelIndex();
    return (
        p == ParallelType::TIDx || p == ParallelType::TIDy ||
        p == ParallelType::TIDz);
  }

  //! Check if this is blockIdx.{x,y,z}
  bool isBlockIdx() const {
    auto p = getParallelIndex();
    return (
        p == ParallelType::BIDx || p == ParallelType::BIDy ||
        p == ParallelType::BIDz);
  }

  //! Check if this is blockDim.{x,y,z}
  bool isBlockDim() const {
    auto p = getParallelDim();
    return (
        p == ParallelType::TIDx || p == ParallelType::TIDy ||
        p == ParallelType::TIDz);
  }

  //! Check if this is gridDim.{x,y,z}
  bool isGridDim() const {
    auto p = getParallelDim();
    return (
        p == ParallelType::BIDx || p == ParallelType::BIDy ||
        p == ParallelType::BIDz);
  }

  //! Return the named scalar extent of a parallel dimension (e.g. blockDim.x)
  //! WARNING: Only works with Fusion container at the moment
  static NamedScalar* getParallelDim(ParallelType p_type);

  //! Return the named scalar index of a parallel dimension (e.g. threadIdx.x)
  //! WARNING: Only works with Fusion container at the moment
  static NamedScalar* getParallelIndex(ParallelType p_type);

  //! Return the parallel type of this NamedScalar if it is an extent of a
  //! parallel dimension
  c10::optional<ParallelType> getParallelDim() const;

  //! Return the parallel type of this NamedScalar if it is an index of a
  //! parallel dimension
  c10::optional<ParallelType> getParallelIndex() const;

 private:
  std::string name_;
};

class TORCH_CUDA_CU_API PadOp : public Expr {
 public:
  using Expr::Expr;

  //! Pad a tensor as specified by a vector of integer scalars. For
  //! the actual semantics, see the torch.pad documentation. Note that
  //! unlike torch.pad, the pad_widths vector parameter must contain
  //! width vals for all dimensions. For non-padded dimensions, width
  //! vals should be integer zero.
  PadOp(
      IrBuilderPasskey passkey,
      TensorView* out,
      TensorView* inp,
      const std::vector<Val*>& pad_widths,
      Val* value);

  NVFUSER_DECLARE_CLONE_AND_CREATE

  const char* getOpString() const override {
    return "PadOp";
  }

  std::string toString(int indent_size = 0) const override;
  std::string toInlineString(int indent_size = 0) const override;

  Val* out() const {
    return output(0);
  }

  Val* in() const {
    return input(0);
  }

  Val* value() const {
    return input(1);
  }

  //! Return axes that are actually paded, i.e., those that have
  //! non-zero pad widths
  std::vector<int> getPaddedAxes() const;

  //! Return pad widths of the given axis, which are just zero for non padded
  //! dimensions
  std::pair<Val*, Val*> getPadWidths(int axis) const;

  //! Return the pad widths of all dimensions, including non-padded ones
  std::vector<Val*> getPadWidths() const;

 private:
  //! Offset of pad_width inputs in the input vector
  int getPadWidthInputOffset() const {
    return 2;
  }

  //! Iterator to the first pad_width input
  auto getPadWidthInputBegin() const {
    return inputs().cbegin() + getPadWidthInputOffset();
  }

  //! Iterator to the end of the pad_width inputs
  auto getPadWidthInputEnd() const {
    return inputs().cend();
  }
};

// Similar to at::indexing::Slice
struct Slice {
  Val* start = nullptr;
  Val* stop = nullptr;
  Val* step = nullptr;
};

class TORCH_CUDA_CU_API SliceOp : public Expr {
 public:
  using Expr::Expr;

  SliceOp(
      IrBuilderPasskey passkey,
      TensorView* out,
      TensorView* inp,
      const std::vector<Slice>& ranges);

  NVFUSER_DECLARE_CLONE_AND_CREATE

  const char* getOpString() const override {
    return "SliceOp";
  }

  std::string toString(int indent_size = 0) const override;
  std::string toInlineString(int indent_size = 0) const override;

  Val* out() const {
    return output(0);
  }

  Val* in() const {
    return input(0);
  }

  std::vector<Slice> getRanges() const;

 private:
  //! Offset of ranges input in the input vector
  int getRangeInputOffset() const {
    return 1;
  }

  //! Iterator to the first range inputs
  auto getRangeInputBegin() const {
    return inputs().cbegin() + getRangeInputOffset();
  }

  //! Iterator to the end of the range inputs
  auto getRangeInputEnd() const {
    return inputs().cend();
  }
};

class TORCH_CUDA_CU_API CatOp : public Expr {
 public:
  using Expr::Expr;

  CatOp(
      IrBuilderPasskey passkey,
      Val* out,
      const std::vector<Val*>& inputs,
      int concatenated_dim);

  //! Create a cat op with the index and predicates for codegen. Only
  //! used for the Kernel container
  CatOp(
      IrBuilderPasskey passkey,
      Val* out,
      const std::vector<Val*>& inputs,
      int concatenated_dim,
      Val* concatenated_domain_index,
      const std::vector<Bool*>& preds);

  NVFUSER_DECLARE_CLONE_AND_CREATE

  const char* getOpString() const override {
    return "CatOp";
  }

  std::string toString(int indent_size = 0) const override;
  std::string toInlineString(int indent_size = 0) const override;

  int concatenatedDim() const {
    return attribute(0)->as<Attribute<int>>()->value;
  }

  //! The index val that determines which input tensor should be used
  //! to fill the particular output position of this expression. Only
  //! valid after indexing
  Val* getConcatenatedDomainIndex() const;

  //! Gets a Bool indicating if the input tensor specified by
  //! tensor_idx should be used to fill the output tensor. Only valid
  //! with the Kernel container
  Bool* getPred(int input_idx) const;
};

} // namespace nvfuser