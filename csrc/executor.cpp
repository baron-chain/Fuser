// clang-format off
/*
 * SPDX-FileCopyrightText: Copyright (c) 2023-present NVIDIA CORPORATION & AFFILIATES.
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
// clang-format on

#include <executor.h>

#include <codegen.h>
#include <debug.h>
#include <device_lower/analysis/bank_conflict.h>
#include <driver_api.h>
#include <executor_kernel_arg.h>
#include <executor_utils.h>
#include <fusion_profiler.h>
#include <global_allocator.h>
#include <instrumentation.h>
#include <ir/all_nodes.h>
#include <ir/utils.h>
#include <iter_visitor.h>
#include <kernel_ir.h>
#include <multidevice/communication.h>
#include <multidevice/communicator.h>
#include <multidevice/lower_communication.h>
#include <multidevice/utils.h>
#include <options.h>
#include <polymorphic_value.h>
#include <serde/utils.h>
#include <tensor_metadata.h>
#include <utils.h>

#include <ATen/core/LegacyTypeDispatch.h>
#include <ATen/cuda/CUDAContext.h>
#include <ATen/cuda/llvm_jit_strings.h>
#include <ATen/native/cuda/jit_utils.h>
#include <c10/core/DeviceGuard.h>
#include <c10/cuda/CUDAFunctions.h>
#include <c10/cuda/CUDAStream.h>
#include <c10/util/irange.h>

#include <cmath>
#include <cstring>
#include <fstream>

namespace nvfuser {

bool fill_allocation_with_nan_ = false;

bool shouldFillAllocationWithNan() {
  return fill_allocation_with_nan_;
}

void setFillAllocationWithNan(bool value) {
  fill_allocation_with_nan_ = value;
}

namespace {

static const char* defineIndexType(PrimDataType index_type) {
  if (index_type == DataType::Int32) {
    return "typedef int nvfuser_index_t;\n";
  } else if (index_type == DataType::Int) {
    return "typedef int64_t nvfuser_index_t;\n";
  } else {
    NVF_ERROR(false, "invalid indexing type: ", index_type);
  }
}

static const char* defineTypes() {
  return R"(
using int8_t = signed char;
using uint8_t = unsigned char;
using int16_t = short int;
using uint16_t = unsigned short int;
using int32_t = int;
using uint32_t = unsigned int;
using int64_t = long long int;
using uint64_t = unsigned long long int;

// Modified from cuda.h
struct TensorMap {
  alignas(64)
  uint64_t opaque[16];
};
)";
}

static const std::string& includeStdComplex() {
  static std::string result = std::string(R"ESCAPE(
#ifdef __NVCC__
#include <complex>
#endif // __NVCC__
)ESCAPE");
  return result;
}

// When executing nvFuser with: NVFUSER_EXTERNAL_SRC=file1.cu,file2.cu
// This function retrieves structured code from the specified files.
// The files should be comma-separated, and their order corresponds to the
// fusion_id order. If the provided number of files is fewer than the fusion
// segments, the function will resort to the available files in sequence
// and issue a warning.
std::string getStructuredCodeFromExternalFiles(const int64_t fusion_id) {
  auto external_code_path = getNvFuserEnv("EXTERNAL_SRC");
  if (!external_code_path) {
    return "";
  }
  std::string all_external_code_paths(external_code_path);
  if (all_external_code_paths.empty() || fusion_id < 1) {
    return "";
  }
  auto getExternalCodeFile =
      [fusion_id](const std::string& input) -> std::string {
    std::stringstream ss(input);
    std::string token;
    int64_t count = 0;
    while (std::getline(ss, token, ',')) {
      if (++count == fusion_id) {
        return token;
      }
    }
    debug()
        << "Didn't find requested external source code. Will use generated code!\n"
        << "Number of source code files should equal the number of fusion segments.\n"
        << "External source code filenames should be delineated with commas, e.g.: file1.cu,file2.cu.\n";
    return "";
  };

  std::string single_code_path = getExternalCodeFile(all_external_code_paths);
  if (single_code_path.empty()) {
    return "";
  }
  std::ifstream cuda_src(single_code_path);
  if (!cuda_src.is_open()) {
    debug() << "Failed to open external source file: " << single_code_path
            << std::endl;
    return "";
  }
  debug() << "--------> Compiling external CUDA code: " << single_code_path
          << std::endl;

  std::stringstream buffer;
  buffer << cuda_src.rdbuf();
  return buffer.str();
}
} // namespace

FusionExecutor::FusionExecutor()
    : communicator_(&Communicator::getInstance()) {}

std::unique_ptr<PrecomputedValues>& FusionExecutor::
    evaluatorPrecomputedValues() {
  if (!evaluator_precomputed_values_) {
    evaluator_precomputed_values_ =
        std::make_unique<PrecomputedValues>(lowered_->kernel());
  }
  return evaluator_precomputed_values_;
}

std::string FusionExecutor::getStructuredCode(
    const std::string& kernel_str,
    PrimDataType index_type) const {
  // generating cuda code;
  std::string code = "";
  code += includeStdComplex();
  code += std::string("namespace {\n") + defineTypes() +
      defineIndexType(index_type) + executor_utils::kernelPreamble() +
      kernel_str + "}\n";

  if (isDebugDumpEnabled(DebugDumpOption::CudaKernel)) {
    debug() << "\n======= Codegen output for kernel: " << kernelName()
            << " =======\n\n"
            << kernel_str << "\n======================================\n\n";
  } else if (isDebugDumpEnabled(DebugDumpOption::CudaFull)) {
    debug() << "\n======= Codegen output for kernel: " << kernelName()
            << " =======\n\n"
            << code << "\n======================================\n\n";
  }
  if (isDebugDumpEnabled(DebugDumpOption::CudaToFile) ||
      isDebugDumpEnabled(DebugDumpOption::DebugInfo)) {
    std::stringstream file_name;
    file_name << "__tmp_kernel_" << kernel_id_ << ".cu";
    debug() << "PRINTING: " << file_name.str() << std::endl;
    std::ofstream out(file_name.str());
    out << code << std::endl;
    out.close();
  }

  return code;
}

std::string FusionExecutor::getStructuredCode() const {
  return getStructuredCode(kernelString(), kernel()->indexType());
}

void FusionExecutor::compileFusion(
    Fusion* fusion,
    const KernelArgumentHolder& args,
    const LaunchParams& launch_constraints,
    CompileParams compile_params,
    ScheduleHeuristic heuristic,
    int64_t fusion_id,
    int64_t concrete_id,
    int64_t runtime_id,
    int64_t group_id) {
  FUSER_PERF_SCOPE("FusionExecutor::compileFusion");

  NVF_ERROR(
      !fusion->outputs().empty(), "No output found for this kernel, aborting.");

  // TODO: refactor the options_ passed through
  options_.device = c10::Device(c10::DeviceType::CUDA, args.getDeviceIndex());

  if (isExpressionEvaluated(fusion)) {
    fusion_ = std::make_unique<Fusion>(*fusion);
    return;
  }

  const std::vector<Expr*>& exprs = fusion->exprs();
  if (std::all_of(exprs.begin(), exprs.end(), [](Expr* e) {
        return isResharding(e) && isLowerableToCommunication(e);
      })) {
    host_ir_container_ = std::make_unique<hir::HostIrContainer>();
    IrCloner cloner = Fusion::copy(fusion, host_ir_container_.get());
    for (Expr* e : exprs) {
      std::vector<Communication*> communications =
          lowerCommunication(cloner.clone(e));
      for (auto* communication : communications) {
        host_ir_container_->pushBackTopLevelExprs(communication);
      }
    }
    return;
  }

  // NOTE: Profiling needs to be started below the isExpressionEvaluated query
  // given the conditional can exit early from compilation.
  if (isProfilerEnabled()) {
    NVF_CHECK(
        group_id >= 0,
        "An invalid segment id is passed to FusionProfiler!:",
        group_id);
    FusionProfiler::segment(group_id).startCompile(args.getDeviceIndex());
  }

  for (auto out : fusion->outputs()) {
    const auto logical_domain = out->as<TensorView>()->getLogicalDomain();
    // walking through outputs to see if output shapes are dependent on
    // non-tensor inputs. For which case, we should have disabled output
    // allocation, since the caching id only looks at tensor shapes.
    // See issue https://github.com/csarofeen/pytorch/issues/2002
    std::vector<Val*> output_extents;
    for (const auto id : logical_domain) {
      Val* extent = nullptr;
      if (id->isReduction() || id->isStride() || id->isDeviceDim()) {
        continue;
      } else if (id->isBroadcast() && id->hasExpandedExtent()) {
        extent = id->expandedExtent();
      } else {
        extent = id->extent();
      }
      output_extents.emplace_back(extent);
    }
    auto dependencies = InputsOf::outputs(output_extents);
    if (std::any_of(dependencies.begin(), dependencies.end(), [](Val* val) {
          return val->isFusionInput();
        })) {
      // TODO: parameter cache is too big a hammer here. We should consider
      // separate the caching logic of output sizes & launch params. Since
      // output size dependency should only invalidate the output sizes
      disable_parameter_cache_ = true;
      break;
    }
  }

  if (isDebugDumpEnabled(DebugDumpOption::FusionIr)) {
    fusion->print();
  } else if (isDebugDumpEnabled(DebugDumpOption::FusionIrMath)) {
    fusion->printMath();
  }

  //! Force index_type to int and disable magic zero if we detect that the
  //! kernel contains any TMA memory operations.
  bool has_cp_async_bulk = std::any_of(exprs.begin(), exprs.end(), [](Expr* e) {
    return ir_utils::isCpAsyncBulk(e);
  });

  // Disable magic zero if there are any TMA operations in Fusion
  if (has_cp_async_bulk) {
    compile_params.enable_magic_zero = false;
  }

  // Set the index type of compile params if not already set. If set,
  // make sure the compile param type is valid with the given kernel
  // arguments.
  auto arg_index_type = args.getSmallestIndexTypeOfArguments();
  if (compile_params.index_type.has_value()) {
    // If the int32 compilation is requested, but the arguments demand
    // int64, that's an error
    NVF_ERROR(
        !(compile_params.index_type.value() == PrimDataType::Int32 &&
          arg_index_type == PrimDataType::Int),
        "Compilation with int32 is requested but int64 is required for the arguments");
    NVF_ERROR(
        !has_cp_async_bulk ||
            (compile_params.index_type.value() == PrimDataType::Int32),
        "Compilation with int64 is requested but int32 is required because ",
        "of TMA operations.");

  } else if (arg_index_type == PrimDataType::Int) {
    // If the given compile option doesn't specify the index type, and
    // the arguments require 64-bit indexing, we need to use 64-bit
    // indexing. Note that if the arg type is 32-bit, it doesn't mean
    // it's safe to use 32-bit for the whole kernel, so unless it's
    // specified through CompileParams, we do not use 32-bit indexing.
    compile_params.index_type = arg_index_type;
    NVF_ERROR(
        !has_cp_async_bulk,
        "Compilation with int64 is required based on input arguments, but ",
        "int32 is required because of TMA operations.");
  } else if (has_cp_async_bulk) {
    // TMA operations require 32-bit indexing.
    compile_params.index_type = PrimDataType::Int32;
  }

  c10::DeviceGuard dg(options_.device);

  NVF_ERROR(
      options_.device.is_cuda(), "Provided device to CUDA fuser is the CPU.");
  auto properties = at::cuda::getDeviceProperties(options_.device.index());
  // TODO: These properties should be set as part of the constructor so that it
  // can be const
  device_smem_limit_ = static_cast<int64_t>(properties->sharedMemPerBlockOptin);
  warp_size_ = properties->warpSize;

  lowered_ = std::make_unique<GpuLower>(fusion, compile_params);
  for (const auto& hook : lowering_hooks_) {
    hook(lowered_.get());
  }
  lowered_->run();

  kir::Kernel* kernel = lowered_->kernel();

  for (const auto& hook : post_lowering_hooks_) {
    hook(kernel);
  }
  createKernelId(heuristic, fusion_id, concrete_id, runtime_id, group_id);
  setUsedTVs();

  if (isDebugDumpEnabled(DebugDumpOption::KernelIr)) {
    kernel->print();
  }

  if (isDebugDumpEnabled(DebugDumpOption::BankConflictInfo)) {
    auto bank_conflict_info = getBankConflictInfo(kernel);
    if (bank_conflict_info.empty()) {
      debug() << "===== No bank confliction =====" << std::endl;
    } else {
      debug() << "======= Bank confliction =======" << std::endl;
      for (auto info : bank_conflict_info) {
        debug() << "Expr: " << info.first->toString() << std::endl;
        auto conflict = info.second;
        if (conflict.first > 1) {
          debug() << "input conflict: " << conflict.first << " way, ";
        }
        if (conflict.second > 1) {
          debug() << "output conflict: " << conflict.second << " way";
        }
        debug() << std::endl;
      }
      debug() << "================================" << std::endl;
    }
  }

  kernel_code_ = codegen::generateCudaKernel(kernel, kernelName());

  // If NVFUSER_EXTERNAL_SRC is set, utilize the external source code.
  // If the loaded external source code is empty, revert to the default codegen.
  // The external_structured_code is moved to structured_code and explicitly
  // cleared to avoid use-after-move scenarios.
  // Note: we index these with getGlobalFusionCount() instead of fusion_id_ in
  // order to match the numbering of files output with
  // NVFUSER_DUMP=cuda_to_file
  auto structured_code =
      getStructuredCodeFromExternalFiles(getGlobalFusionCount());
  if (structured_code.empty()) {
    structured_code = getStructuredCode();
  }

  const kir::KernelSummary& kernel_summary = kernel->summary();

  // TODO: this replicates the target GPU version computation from
  // executor_utils.
  std::pair<int64_t, int64_t> target_arch;
  bool compile_to_sass = false;
  executor_utils::queryTargetGPUVersion(
      properties,
      std::ref(target_arch.first),
      std::ref(target_arch.second),
      compile_to_sass);

  NVF_CHECK(
      target_arch >= kernel_summary.min_device_version,
      "Target compute capability is ",
      target_arch.first,
      ".",
      target_arch.second,
      " but this fusion requires at least ",
      kernel_summary.min_device_version.first,
      ".",
      kernel_summary.min_device_version.second,
      ". Reason: ",
      kernel_summary.min_device_version_reason);

  // We currently shouldn't allocate any more shared mem
  //  tensors statically but could keep this path if
  //  needed in later development.
  if (!kernel_summary.static_smem_allocations.empty()) {
    ExpressionEvaluator static_evaluator;
    const auto static_smem_size = computeSharedMemory(
        static_evaluator,
        kernel_summary.static_smem_allocations,
        kernel->indexType());
    NVF_ERROR(
        static_smem_size < max_static_smem_,
        "The static shared memory allocation is larger than available memory.");
  }

  if (kernel_summary.has_dynamic_local_memory_allocations) {
    std::stringstream ss;
    ss << "Allocations must be based on constant integers for local memory. However, found: ";
    for (auto alloc : kernel_summary.dynamic_lmem_allocations) {
      ss << alloc->buffer()->toString() << ", ";
    }
    ss << " have dynamic allocations but are placed in local memory.";
    NVF_ERROR(false, ss.str());
  }

  // TODO: pass block_size here;
  std::optional<int64_t> dynamic_smem = std::nullopt;
  std::optional<int64_t> block_size = std::nullopt;
  if (!args.empty()) {
    auto expr_eval = executor_utils::bindInputs(args, kernel);
    auto launch_params = computeLaunchParams(
        launch_constraints, expr_eval, warp_size_, kernel->indexType());
    block_size = launch_params.nThreads();
    dynamic_smem = launch_params.smem();
    NVF_ERROR(block_size > 0, "launch param inferred block size < 0");
  }

  // TODO: high water mark should be computed via occupancy API after
  // compilation.

  // Basically setting high water mark as 1 when we don't provide args for
  // compilation, it will just generate a kernel that gets ditched at the first
  // run - not great. We should have better heuristics.
  block_size_high_water_mark_ = std::max<int64_t>(
      (block_size.has_value() ? block_size.value() : 1),
      block_size_high_water_mark_);
  maxrregcount_high_water_mark_ = compile_params.maxrregcount;
  compiled_kernel_ = executor_utils::getCompiledKernel(
      kernel_code_,
      structured_code,
      kernelName(),
      kernel_id_,
      compile_params,
      block_size);
  NVF_ERROR(validKernelId(), "Invalid kernel id for FusionExecutor.");

  // These should be nullopt at this point, but reset just in case
  resetCompiledKernelProperties();

  // If the dynamic shmem size is known, make sure the compiled kernel
  // has at least that size of dynamic shmem
  if (dynamic_smem.has_value()) {
    ensureAvailableDynamicSmemSize(dynamic_smem.value());
  }

  if (isDebugDumpEnabled(DebugDumpOption::Sass)) {
    debug() << disassembledKernelSASS() << std::endl;
  }
  if (isProfilerEnabled()) {
    FusionProfiler::segment(group_id).stopCompile();
  }
}

namespace {

void fillTensorWithNan(at::Tensor& t) {
  switch (t.scalar_type()) {
    case at::ScalarType::Byte:
      t.fill_(0xFF);
      break;
    case at::ScalarType::Char:
      t.fill_(0x7F);
      break;
    case at::ScalarType::Short:
      t.fill_(0x7FFF);
      break;
    case at::ScalarType::Int:
      t.fill_(0x7FFFFFFF);
      break;
    case at::ScalarType::Long:
      t.fill_(0x7FFFFFFFFFFFFFFFL);
      break;
    case at::ScalarType::Bool:
      t.fill_(true);
      break;
    case at::ScalarType::Half:
    case at::ScalarType::Float:
    case at::ScalarType::Double:
    case at::ScalarType::BFloat16:
    case at::ScalarType::Float8_e4m3fn:
    case at::ScalarType::Float8_e5m2:
      t.fill_(std::nan(""));
      break;
    case at::ScalarType::ComplexHalf:
    case at::ScalarType::ComplexFloat:
    case at::ScalarType::ComplexDouble:
      t.fill_(c10::complex<double>(std::nan(""), std::nan("")));
      break;
    default:
      NVF_ERROR(false, "Unknown dtype");
  }
}

std::vector<int64_t> getContiguousStrides(
    const std::vector<int64_t>& sizes,
    const std::vector<bool>& expand_flags) {
  NVF_ERROR(sizes.size() == expand_flags.size());

  std::vector<int64_t> strides(sizes.size());
  int64_t cur_stride = 1;
  for (auto i = sizes.size(); i > 0; --i) {
    auto size = sizes.at(i - 1);
    NVF_ERROR(
        size >= 0,
        "Positive size is assumed non-negative but received: ",
        size);

    int64_t stride = cur_stride;

    // If expanded, stride is 0
    if (expand_flags.at(i - 1)) {
      stride = 0;
    } else if (size == 0) {
      // If the size is 0, the stride is 1.
      stride = 1;
    } else {
      cur_stride *= size;
    }

    strides.at(i - 1) = stride;
  }

  return strides;
}

// Infer the size and stride of each dimension
std::pair<std::vector<int64_t>, std::vector<int64_t>> inferShape(
    const TensorView* tv,
    std::vector<Val*> symbolic_sizes,
    std::vector<bool> expand_flags,
    ExpressionEvaluator& expr_eval) {
  // Allocate should be provided for intermediates. We just need to
  // grab a chunk of memory of the size dicatated by
  // Allocate::shape(). Fusion outputs do not come with Allocate and
  // need to be allocated while taking expanded broadcasts into
  // account.

  std::vector<int64_t> concrete_sizes(symbolic_sizes.size(), 0);

  for (const auto i : c10::irange(symbolic_sizes.size())) {
    auto symbolic_size = symbolic_sizes.at(i);
    const auto inferred_val = expr_eval.evaluate(symbolic_size);
    NVF_ERROR(
        inferred_val.hasValue(),
        "Could not launch kernel as program could not infer ",
        symbolic_size->toInlineString(),
        "(",
        symbolic_size->toString(),
        ") for the buffer ",
        tv->toString());

    auto concrete_size = inferred_val.as<int64_t>();
    concrete_sizes.at(i) = concrete_size;
  }

  auto strides = getContiguousStrides(concrete_sizes, expand_flags);

  return {concrete_sizes, strides};
}

// Infer the shape of an intemediate tensor using kir::Allocate. This
// is not ideal but still necessary when tensors are expanded with halo
std::pair<std::vector<int64_t>, std::vector<int64_t>> inferShapeOfIntermediate(
    const TensorView* tv,
    const kir::Allocate* alloc,
    ExpressionEvaluator& expr_eval) {
  // The allocation domain represents the logical allocation domain,
  // bu its actual allocation size may be different, e.g., for
  // supporting halo accesses. The actual size is currently computed
  // when creating the Allocate expr.
  NVF_ERROR(alloc != nullptr);
  const auto& symbolic_sizes = alloc->shape();
  // For intermediate tensors, we just need to allocate a memory chunk
  // of the specified size. Broadcast expansion does not need to be considered.
  const auto expand_flags = std::vector<bool>(symbolic_sizes.size(), false);

  return inferShape(tv, symbolic_sizes, expand_flags, expr_eval);
}

class ForwardTraverseFromAllocToLogical {
  at::Tensor tensor_;
  ExpressionEvaluator& ee_;
  std::list<IterDomain*>& frontier_;

  // Forward traverse split from allocation to logical. Needs to, for example,
  // view tensor with shape [..., 15, ...] as [..., 3, 5, ...]
  void handle(Split* split) {
    auto in = split->in();
    auto inner = split->inner();
    auto outer = split->outer();
    auto factor = ee_.evaluate(split->factor()).as<int64_t>();
    auto in_it = std::find(frontier_.begin(), frontier_.end(), in);
    // NVF_ERROR(in_it != frontier_.end());
    if (in_it == frontier_.end()) {
      // TODO: We should get rid of this return and enable the above assert.
      // Note [Allocation domain on both side of logical]
      // For cases where the allocation domain is on both side of logical, for
      // example, in Tensor3d_To_NHWC4d_FwdBwd_CUDA:
      // [alloc,root]   [alloc,root]           [root]
      //          \     /                      /    |
      //         [logical]                  split   [logical]
      //                                    /  \         |
      //                      [alloc,logical] [logical]  |
      //                                             \   |
      //                                             [alloc]
      // I have no idea why StmtSort::getExprsBetween is not returning the
      // expected set of exprs, but for now, I will just skip these illegal
      // exprs.
      return;
    }
    // view tensor
    int64_t dim = std::distance(frontier_.begin(), in_it);
    std::vector<int64_t> new_shape;
    for (auto i : c10::irange(tensor_.dim())) {
      if (i == dim) {
        new_shape.emplace_back(-1);
        new_shape.emplace_back(factor);
      } else {
        new_shape.emplace_back(tensor_.size(i));
      }
    }
    tensor_ = tensor_.view(new_shape);
    // update frontier
    frontier_.insert(in_it, outer);
    frontier_.insert(in_it, inner);
    frontier_.erase(in_it);
  }

  // Forward traverse split from allocation to logical. Needs to, for example,
  // view tensor with shape [..., 3, 5, ...] as [..., 15, ...]
  void handle(Merge* merge) {
    auto inner = merge->inner();
    auto outer = merge->outer();
    auto out = merge->out();
    auto inner_it = std::find(frontier_.begin(), frontier_.end(), inner);
    auto outer_it = std::find(frontier_.begin(), frontier_.end(), outer);
    // NVF_ERROR(inner_it != frontier_.end());
    // NVF_ERROR(outer_it != frontier_.end());
    if (inner_it == frontier_.end() || outer_it == frontier_.end()) {
      // TODO: see [Allocation domain on both side of logical]
      return;
    }
    int64_t inner_dim = std::distance(frontier_.begin(), inner_it);
    int64_t outer_dim = std::distance(frontier_.begin(), outer_it);
    int64_t left = std::min(inner_dim, outer_dim);
    // view the tensor
    if (outer_dim + 1 != inner_dim) {
      // need to permute the tensor in order to do a merging view
      // before: [..., outer, ..., inner, ...]
      // after: [..., outer, inner, ...]
      std::vector<int64_t> dims;
      int64_t i = 0;
      while (i < tensor_.dim() && i != left) {
        dims.emplace_back(i);
        i++;
      }
      dims.emplace_back(outer_dim);
      dims.emplace_back(inner_dim);
      while (i < tensor_.dim()) {
        if (i != outer_dim && i != inner_dim) {
          dims.emplace_back(i);
        }
        i++;
      }
      tensor_ = tensor_.permute(dims);
    }
    std::vector<int64_t> new_shape;
    for (auto i : c10::irange(tensor_.dim())) {
      if (i == left) {
        new_shape.emplace_back(-1);
      } else if (i != left + 1) {
        new_shape.emplace_back(tensor_.size(i));
      }
    }
    tensor_ = tensor_.view(new_shape);
    // update frontier
    if (inner_dim < outer_dim) {
      *inner_it = out;
      frontier_.erase(outer_it);
    } else {
      *outer_it = out;
      frontier_.erase(inner_it);
    }
  }

  void handle(Expr* expr) {
    if (auto split = dynamic_cast<Split*>(expr)) {
      handle(split);
    } else if (auto merge = dynamic_cast<Merge*>(expr)) {
      handle(merge);
    } else {
      NVF_ERROR(false, "Unsupported transormation in allocation domain");
    }
  }

 public:
  ForwardTraverseFromAllocToLogical(
      at::Tensor tensor,
      ExpressionEvaluator& ee,
      std::list<IterDomain*>& frontier)
      : tensor_(std::move(tensor)), ee_(ee), frontier_(frontier) {}

  at::Tensor run(
      const std::vector<IterDomain*>& logical,
      const std::vector<IterDomain*>& alloc) {
    auto forward_exprs = StmtSort::getExprsBetween(
        {alloc.begin(), alloc.end()}, {logical.begin(), logical.end()});
    for (auto expr : forward_exprs) {
      handle(expr);
    }
    return tensor_;
  }
};

// Backward traverse is similar to forward traverse, but we need to do opposite
// transformations.
class BackwardTraverseFromAllocToLogical {
  at::Tensor tensor_;
  ExpressionEvaluator& ee_;
  std::list<IterDomain*>& frontier_;

  // Backward traverse split from allocation to logical. Needs to, for example,
  // view tensor with shape [..., 3, 5, ...] as [..., 15, ...]
  void handle(Split* split) {
    auto inner = split->inner();
    auto outer = split->outer();
    auto in = split->in();
    auto inner_it = std::find(frontier_.begin(), frontier_.end(), inner);
    auto outer_it = std::find(frontier_.begin(), frontier_.end(), outer);
    // NVF_ERROR(inner_it != frontier_.end());
    // NVF_ERROR(outer_it != frontier_.end());
    if (inner_it == frontier_.end() || outer_it == frontier_.end()) {
      // TODO: see [Allocation domain on both side of logical]
      return;
    }
    int64_t inner_dim = std::distance(frontier_.begin(), inner_it);
    int64_t outer_dim = std::distance(frontier_.begin(), outer_it);
    int64_t left = std::min(inner_dim, outer_dim);
    // view the tensor
    if (outer_dim + 1 != inner_dim) {
      // need to permute the tensor in order to do a merging view
      // before: [..., outer, ..., inner, ...]
      // after: [..., outer, inner, ...]
      std::vector<int64_t> dims;
      int64_t i = 0;
      while (i < tensor_.dim() && i != left) {
        dims.emplace_back(i);
        i++;
      }
      dims.emplace_back(outer_dim);
      dims.emplace_back(inner_dim);
      while (i < tensor_.dim()) {
        if (i != outer_dim && i != inner_dim) {
          dims.emplace_back(i);
        }
        i++;
      }
      tensor_ = tensor_.permute(dims);
    }
    std::vector<int64_t> new_shape;
    for (auto i : c10::irange(tensor_.dim())) {
      if (i == left) {
        new_shape.emplace_back(-1);
      } else if (i != left + 1) {
        new_shape.emplace_back(tensor_.size(i));
      }
    }
    tensor_ = tensor_.view(new_shape);
    // update frontier
    if (inner_dim < outer_dim) {
      *inner_it = in;
      frontier_.erase(outer_it);
    } else {
      *outer_it = in;
      frontier_.erase(inner_it);
    }
  }

  // Backward traverse split from allocation to logical. Needs to, for example,
  // view tensor with shape [..., 15, ...] as [..., 3, 5, ...]
  void handle(Merge* merge) {
    auto out = merge->out();
    auto inner = merge->inner();
    auto outer = merge->outer();
    auto factor = ee_.evaluate(inner->extent()).as<int64_t>();
    auto out_it = std::find(frontier_.begin(), frontier_.end(), out);
    // NVF_ERROR(out_it != frontier_.end());
    if (out_it == frontier_.end()) {
      // TODO: see [Allocation domain on both side of logical]
      return;
    }
    // view tensor
    int64_t dim = std::distance(frontier_.begin(), out_it);
    std::vector<int64_t> new_shape;
    for (auto i : c10::irange(tensor_.dim())) {
      if (i == dim) {
        new_shape.emplace_back(-1);
        new_shape.emplace_back(factor);
      } else {
        new_shape.emplace_back(tensor_.size(i));
      }
    }
    tensor_ = tensor_.view(new_shape);
    // update frontier
    frontier_.insert(out_it, outer);
    frontier_.insert(out_it, inner);
    frontier_.erase(out_it);
  }

  void handle(Expr* expr) {
    if (auto split = dynamic_cast<Split*>(expr)) {
      handle(split);
    } else if (auto merge = dynamic_cast<Merge*>(expr)) {
      handle(merge);
    } else {
      NVF_ERROR(false, "Unsupported transormation in allocation domain");
    }
  }

 public:
  BackwardTraverseFromAllocToLogical(
      at::Tensor tensor,
      ExpressionEvaluator& ee,
      std::list<IterDomain*>& frontier)
      : tensor_(std::move(tensor)), ee_(ee), frontier_(frontier) {}

  at::Tensor run(
      const std::vector<IterDomain*>& logical,
      const std::vector<IterDomain*>& alloc) {
    auto backward_exprs = StmtSort::getExprsBetween(
        {logical.begin(), logical.end()}, {alloc.begin(), alloc.end()});
    std::reverse(backward_exprs.begin(), backward_exprs.end());
    for (auto expr : backward_exprs) {
      handle(expr);
    }
    return tensor_;
  }
};

// Start from a tensor whose dimensions are consistent with the allocation
// domain of tv, apply a sequence of view/permute to the tensor to transform it
// into a format whose dimensions are consistent with the logical domain of tv.
// For example, if the logical domain is [I1, I2], and the allocation domain is
// [I2*I1], then we will allocate as [I2*I1], then do a tensor.view(I2, I1).t()
// to get a tensor whose semantics is [I1, I2] but its memory is [I2*I1].
// Another example, if the logical domain is [I1*I2] and the allocation domain
// is [I1, I2], then we will allocate as [I1, I2] and do a tensor.view(I1*I2) to
// get a tensor whose semantics is [I1*I2] but memory is [I1,I2]
at::Tensor transformOutputFromAllocationToLogical(
    at::Tensor tensor,
    TensorView* tv,
    ExpressionEvaluator& ee) {
  // Ignore reductions because reductions does not exist in tensor's definition
  auto logical = TensorDomain::noReductions(tv->getLogicalDomain());
  auto alloc = TensorDomain::noReductions(tv->getMaybeAllocationDomain());
  // Traverse all affine transformations from allocation domain. Because
  // allocation domain can be before or after the logical domain, we need both a
  // forward and a backward traverse.
  std::list<IterDomain*> frontier(alloc.begin(), alloc.end());
  NVF_ERROR(tensor.dim() == (int64_t)frontier.size());
  tensor = ForwardTraverseFromAllocToLogical(tensor, ee, frontier)
               .run(logical, alloc);
  tensor = BackwardTraverseFromAllocToLogical(tensor, ee, frontier)
               .run(logical, alloc);
  NVF_ERROR(frontier.size() == logical.size());
  // Now that all affine transformations are handled, and frontiers should
  // contain the same set of IDs as logical. We still need to do a final
  // permutation so that their orders are also consistent.
  std::unordered_map<IterDomain*, int64_t> current_dims;
  int64_t counter = 0;
  for (auto id : frontier) {
    current_dims[id] = counter++;
  }
  std::vector<int64_t> dims;
  dims.reserve(frontier.size());
  for (auto id : logical) {
    dims.emplace_back(current_dims.at(id));
  }
  return tensor.permute(dims);
}

// Infer the sizes and strides of an output tensor
std::pair<std::vector<int64_t>, std::vector<int64_t>> inferShapeOfOutput(
    TensorView* tv,
    ExpressionEvaluator& expr_eval) {
  // Fusion outputs do not come with Allocate and
  // need to be allocated while taking expanded broadcasts into
  // account.

  std::vector<Val*> symbolic_sizes;
  std::vector<bool> expand_flags;

  // Allocate the allocation domain
  for (const auto id : tv->getMaybeAllocationDomain()) {
    if (id->isReduction() || id->isStride()) {
      continue;
    }

    if (id->isDeviceDim()) {
      symbolic_sizes.push_back(id->container()->oneVal());
    } else {
      symbolic_sizes.push_back(id->getMaybeExpandedExtent());
    }
    if (id->hasExpandedExtent()) {
      NVF_ERROR(
          id->isBroadcast(),
          "Non-broadcast domain should not have an expanded extent: ",
          id->toString());
      expand_flags.push_back(true);
    } else {
      expand_flags.push_back(false);
    }
  }

  auto size_stride = inferShape(tv, symbolic_sizes, expand_flags, expr_eval);
  if (!tv->hasAllocation()) {
    return size_stride;
  }
  auto options =
      c10::TensorOptions().device(c10::Device(c10::DeviceType::Meta));
  auto meta_tensor =
      at::empty_strided(size_stride.first, size_stride.second, options);
  // TODO(jiej): we should refactor it here, there's no need to use
  // meta_tensor at all, size + stride should be used directly in the
  // `transformOutputFromAllocationToLogical`
  meta_tensor =
      transformOutputFromAllocationToLogical(meta_tensor, tv, expr_eval);
  return {meta_tensor.sizes().vec(), meta_tensor.strides().vec()};
}

// Allocate an `at::Tensor` for `out_info` or compute it as an alias.
at::Tensor allocateOutput(
    const FusionExecutor::GlobalBufferInfo& out_info,
    const AliasInfo& alias_info,
    const c10::Device& device,
    ExpressionEvaluator& ee) {
  // Handle a fusion with duplicated outputs.
  TensorView* out_tv = out_info.tv;
  if (ee.isKnown(out_tv)) {
    return ee.evaluate(out_tv).as<at::Tensor>();
  }

  std::optional<at::Tensor> aliased_io_tensor = std::nullopt;
  Val* aliased_io = alias_info.aliased_io;
  if (aliased_io != nullptr) {
    NVF_ERROR(
        aliased_io->isFusionInput() || aliased_io->isFusionOutput(),
        aliased_io->toInlineString(),
        " is expected to be a fusion input/output. `ee.evaluate` ",
        "an intermediate tensor may involve GPU computation to materialize it ",
        "to global memory.");
    const PolymorphicValue& aliased_io_val = ee.evaluate(aliased_io);
    NVF_ERROR(
        aliased_io_val.is<at::Tensor>(),
        "Alias io only supports tensor. Found ",
        PolymorphicValue_functions::toString(aliased_io_val));
    aliased_io_tensor = aliased_io_val.as<at::Tensor>();
  }

  switch (alias_info.type) {
    case AllocationType::New: {
      auto alloc_tensor = at::native::empty_strided_cuda(
          out_info.sizes,
          out_info.strides,
          out_info.type,
          c10::nullopt,
          device,
          c10::nullopt);
      if (shouldFillAllocationWithNan()) {
        fillTensorWithNan(alloc_tensor);
      }
      return alloc_tensor;
    }
    case AllocationType::ReuseBuffer:
      // Unlike for `AllocationType::Evaluate`, don't use
      // ExpressionEvaluator to compute the output tensor. This is because
      // the output tensor may hold different data from the input, e.g., an
      // updated running mean.  `ExpressionEvaluator::evaluate(out_tv)`
      // would trigger non-trivial host computation.
      return aliased_io_tensor.value();
    case AllocationType::Evaluate: {
      auto out_tensor = ee.evaluate(out_tv).as<at::Tensor>();
      if (aliased_io_tensor.has_value()) {
        NVF_ERROR(
            out_tensor.is_alias_of(aliased_io_tensor.value()),
            "ExpressionEvaluator failed to evaluate ",
            out_tv->toString(),
            " as an alias of ",
            aliased_io->toString());
        inferAndValidateAllocationSizesAndStrides(out_tensor, out_tv, ee);
      }
      return out_tensor;
    }
    default:
      NVF_ERROR(false, "Unrecognized AllocationType.");
  }
}

// Allocate output tensors for a given fusion. Outputs may alias inputs, in
// that case output tensors are shallow copies of the aliased inputs
std::vector<at::Tensor> allocateOutputs(
    const Fusion* fusion,
    const std::vector<FusionExecutor::GlobalBufferInfo>& output_info,
    const c10::Device& device,
    ExpressionEvaluator& ee) {
  FUSER_PERF_SCOPE("executor.cpp::allocateOutputs");

  const auto num_outs = output_info.size();

  // Sort the outputs so we compute aliases after allocating non-aliases. The
  // order between aliases can be arbitrary. E.g.,
  //
  // ```
  // non_alias_out = ...
  // alias_out_0 = reshape(non_alias_out, ...)
  // alias_out_1 = reshape(alias_out_0, ...)
  // ```
  //
  // It's fine to compute `alias_out_1` before computing `alias_out_0`: when we
  // compute `alias_out_1`, `alias_out_0` will be recursively
  // `ExpressionEvaluator::evaluate`ed. However, `non_alias_out` must be
  // allocated first so `alias_out_*` can refer them.
  std::vector<std::pair<int64_t, Val*>> sorted_outs;
  sorted_outs.reserve(num_outs);
  for (const auto out_index : c10::irange(num_outs)) {
    sorted_outs.emplace_back(out_index, fusion->outputs()[out_index]);
  }
  std::sort(
      sorted_outs.begin(),
      sorted_outs.end(),
      [fusion](
          const std::pair<int64_t, Val*>& lhs,
          const std::pair<int64_t, Val*>& rhs) {
        return (
            fusion->getOutputAlias(lhs.second).type == AllocationType::New &&
            fusion->getOutputAlias(rhs.second).type != AllocationType::New);
      });

  std::vector<at::Tensor> out_tensors(num_outs);
  for (const auto& [out_index, out] : sorted_outs) {
    at::Tensor out_tensor = allocateOutput(
        output_info[out_index], fusion->getOutputAlias(out), device, ee);
    // Bind `out_tensor` so
    // 1. duplicated outputs map to the same tensor,
    // 2. an output that aliases another output can be evaluated via
    // ExpressionEvaluator cheaply.
    ee.bind(out, out_tensor);
    out_tensors[out_index] = out_tensor;
  }
  return out_tensors;
}
} // namespace

int64_t FusionExecutor::computeSharedMemory(
    ExpressionEvaluator& expr_eval,
    const std::vector<const kir::Allocate*>& buffers,
    DataType index_type,
    int64_t smem_offset) {
  int64_t total = smem_offset;
  // align smem_offset at 16 bytes
  smem_offset = (smem_offset + 15) & (~15);
  for (auto smem_alloc : buffers) {
    // If this buffer aliases another buffer,
    // then do not allocate memory for this buffer.
    if (smem_alloc->alias() == nullptr) {
      NVF_ERROR(
          smem_alloc->address(),
          "Smem address is not set for buffer T",
          smem_alloc->buffer()->name());
      const auto address_val = expr_eval.evaluate(smem_alloc->address());
      NVF_ERROR(
          address_val.hasValue(),
          "Failed to evaluate the address ",
          smem_alloc->address()->toInlineString(),
          " of shared memory buffer T",
          smem_alloc->buffer()->name());
      NVF_ERROR(
          address_val.is<int64_t>(),
          "Address val ",
          smem_alloc->address()->toInlineString(),
          " of shared memory buffer T",
          smem_alloc->buffer()->name(),
          " should be int64 but found ",
          address_val);
      const auto size_val = expr_eval.evaluate(smem_alloc->size());
      NVF_ERROR(
          size_val.hasValue(),
          "Failed to evaluate the size ",
          smem_alloc->size(),
          " of shared memory buffer - T",
          smem_alloc->buffer()->name());

      const auto first_byte = smem_offset + address_val.as<int64_t>();
      const auto data_size =
          dataTypeSize(smem_alloc->buffer()->dtype(), index_type);
      const int64_t size_bytes = size_val.as<int64_t>() * data_size;
      const auto last_byte = first_byte + size_bytes;

      total = std::max(total, last_byte);
    }
  }
  return total;
}

LaunchParams FusionExecutor::computeLaunchParams(
    const LaunchParams& launch_constraints,
    ExpressionEvaluator& expr_eval,
    const int64_t warp_size,
    DataType index_type) {
  FUSER_PERF_SCOPE("FusionExecutor::computeLaunchParams");
  NVF_ERROR(warp_size > 0, "WARP_SIZE should be larger than 0");

  LaunchParams launch_params;

  auto data_cache = compileTimeDataCache();

  auto lower = lowered_.get();
  auto& used_tvs = getUsedTVs();
  auto parallel_binding_ids_entry =
      executor_utils::caching::ExecutorCompileTimeEntry<
          executor_utils::caching::ParallelBindingIterDomains>(
          data_cache, [&used_tvs, &lower]() {
            return std::make_unique<std::vector<IterDomain*>>(
                executor_utils::getParallelBindingsIterDomains(
                    lower, used_tvs));
          });
  auto& parallel_binding_ids = parallel_binding_ids_entry.get();

  auto parallel_iter_extent_entry =
      executor_utils::caching::ExecutorCompileTimeEntry<
          executor_utils::caching::ParallelIterExtentMap>(
          data_cache, [&parallel_binding_ids]() {
            return executor_utils::getParallelIterExtents(parallel_binding_ids);
          });
  auto& parallel_iter_extents = parallel_iter_extent_entry.get();

  const auto& simplified_parallel_iter_extents =
      lower->parallelDimensionMap().getMap();

  // TODO: Need to redesign this part a bit to
  //   find the right place to trigger evaluate
  if (expr_eval.precomputedValues()) {
    expr_eval.precomputedValues()->bindParallelExtents(
        parallel_iter_extents, launch_constraints);
    expr_eval.precomputedValues()->evaluate();
  }

  // If any dimension was set in launch constraints we need to run through
  // IterDomains that have been parallelized, and bind those values. Or make
  // sure if they could be inferred the inference matches what was set.
  for (auto& entry : parallel_iter_extents) {
    auto p_type = entry.first;
    if (launch_constraints.hasDim(p_type)) {
      auto parallel_extents = entry.second;
      for (auto extent : parallel_extents) {
        auto inferred_val = expr_eval.evaluate(extent);
        if (inferred_val.hasValue()) {
          // This value could have been inferred, make sure it was set right.
          bool valid =
              inferred_val.as<int64_t>() == launch_constraints.getDim(p_type) ||
              launch_constraints.getRawVal(p_type) == -1;
          if (!useFallback() && !valid) {
            TORCH_WARN_ONCE(
                "Cannot validate parallelization scheme, "
                "this may be due to mixed broadcast axes that are parallelized.");
          }
        } else if (!expr_eval.precomputedValues()) {
          expr_eval.bind(extent, launch_constraints.getDim(p_type));
        }
        if (!launch_params.hasDim(p_type)) {
          // Bind the launch constraint into our evaluation context
          launch_params.bind(launch_constraints.getDim(p_type), p_type);
          // Makes sure the p-types bound to evaluators are the
          //  final values that will become the actual launch
          //  param size to ensure accurate smem buffer size
          //  computation.
          expr_eval.bind(p_type, launch_constraints.getDim(p_type));
        }
      }
    }
  }

  // Run through the rest of the parallel IterDomains and infer their size
  for (auto [p_type, extent] : simplified_parallel_iter_extents) {
    FUSER_PERF_SCOPE("FusionExecutor::ParallelBindingResolution");
    auto val = expr_eval.evaluate(extent);
    NVF_ERROR(
        val.hasValue(),
        "Tried to evaluate the extent, ",
        extent->toInlineString(),
        " for the ptype: ",
        p_type,
        " to set launch bounds but could not.");

    if (val > 0) {
      expr_eval.bind(p_type, val);
      launch_params.bind(val.as<int64_t>(), p_type);
    }
  }

  // Re-run the integer machine with all
  //  the thread sizes now determined.
  if (expr_eval.precomputedValues()) {
    expr_eval.precomputedValues()->evaluate();
  }

  const auto kernel = lowered_->kernel();
  const auto& kernel_summary = kernel->summary();

  // Calculate Dynamic Shared Memory Size
  // Add workspace for reduction and broadcast
  int64_t reduction_broadcast_workspace = 0;
  const bool has_workspace = kernel_summary.has_block_reductions ||
      kernel_summary.has_grid_reductions ||
      kernel_summary.has_block_broadcasts || kernel_summary.has_grid_broadcasts;
  if (has_workspace &&
      kernel_summary.largest_smem_data_type != DataType::Null) {
    // Not using nThreads here since it does not handle uninitialized value

    // TODO: here is an optimization opportunity since welford uses int64_t for
    // N while the data type is not neccessarily double. But it may need more
    // work on the alignment
    const int welford_factor =
        kernel_summary.has_block_welford || kernel_summary.has_grid_welford ? 3
                                                                            : 1;
    // in outer reduction, may group iteration domain, e.g. when vectorized.
    const int64_t grouped_iter_factor = kernel_summary.num_grouped_iterations;

    NVF_CHECK(
        !(kernel_summary.has_iter_grouped_reductions && welford_factor == 3),
        "can't have welford and iter grouped reductions at the same time! Should be handled by grouped welford!");

    reduction_broadcast_workspace =
        (int64_t)dataTypeSize(
            kernel_summary.largest_smem_data_type, index_type) *
        grouped_iter_factor * welford_factor * launch_params.bdimx() *
        launch_params.bdimy() * launch_params.bdimz();

    if (kernel_summary.has_outer_grouped_grid_welford) {
      reduction_broadcast_workspace = std::max(
          reduction_broadcast_workspace,
          (int64_t)kernel_summary.outer_grouped_grid_welford_largest_smem_size);
    }
  }

  const auto dynamic_smem_size = computeSharedMemory(
      expr_eval,
      kernel_summary.dynamic_smem_allocations,
      index_type,
      reduction_broadcast_workspace);

  // Check that requested smem size can be dynamically allocated.
  //  This check is only done once a kernel has been compiled, since
  //  maybe_available_dynamic_smem_ needs to be evaluated on
  //  a compiled kernel.
  if (hasCompiledKernel()) {
    validateDynamicSmemSize(dynamic_smem_size);
  }

  launch_params.setSmem(dynamic_smem_size);

  return launch_params;
}

std::vector<FusionExecutor::GlobalBufferInfo> FusionExecutor::
    getIntermediateBufferInfo(
        ExpressionEvaluator& expr_eval,
        DataType index_type) {
  std::vector<GlobalBufferInfo> global_buffers;

  const auto kernel = lowered_->kernel();
  const auto& kernel_summary = kernel->summary();

  for (auto alloc : kernel_summary.global_allocations) {
    NVF_ERROR(
        alloc->buffer()->isA<TensorView>(),
        "Cannot allocate global buffers that are not tensors.");
    auto tv = alloc->buffer()->as<TensorView>();
    if (tv->isFusionOutput()) {
      continue;
    }
    GlobalBufferInfo info;
    info.tv = tv;
    info.zero_init = alloc->zeroInit();
    info.resets_to_zero = alloc->resetsToZero();
    // TODO: Allocation size needs to consider both expanded domains
    // as well as halo. Currently, allocation of tensors with halo is
    // only supported by inferShapeOfIntermediate, whereas expanded
    // domains are only supported by inferShapeOfOutput. Until the
    // halo support is revisited, use the former for all tensors
    // unless expanded and the latter otherwise. This assumes there's
    // no expanded domains with halo, which is fine for now.
    const auto has_expanded_domains = std::any_of(
        tv->getMaybeAllocationDomain().begin(),
        tv->getMaybeAllocationDomain().end(),
        [](IterDomain* id) { return id->hasExpandedExtent(); });
    std::tie(info.sizes, info.strides) = has_expanded_domains
        ? inferShapeOfOutput(tv, expr_eval)
        : inferShapeOfIntermediate(tv, alloc, expr_eval);
    auto dtype = (tv->dtype() == DataType::Index ? index_type : tv->dtype());
    info.type = data_type_to_aten(dtype);

    // Remember the tensor buffer used for storing kernel profile
    if (isOptionEnabled(EnableOption::KernelProfile) &&
        tv == kernel->profile().getBuffer()) {
      info.is_profile_buffer = true;
    }

    global_buffers.emplace_back(info);
  }

  return global_buffers;
}

namespace {

//! Return information necessary for allocating output tensors. Input
//! and output tensors are allowed to alias each other, which is
//! specified by the list of int pairs of input and output indices
std::vector<FusionExecutor::GlobalBufferInfo> getOutputBufferInfo(
    const KernelArgumentHolder& args,
    ExpressionEvaluator& expr_eval,
    DataType index_dtype,
    const Fusion* fusion) {
  std::vector<FusionExecutor::GlobalBufferInfo> outputs;
  outputs.reserve(fusion->outputs().size());
  NVF_ERROR(
      args.size() == fusion->inputs().size(),
      "fusion arguments length does not match runtime arguments.");
  for (const auto out_i : c10::irange(fusion->outputs().size())) {
    auto out_val = fusion->outputs()[out_i];
    NVF_ERROR(
        out_val->isA<TensorView>(),
        "Cannot allocate outputs that are not tensors.");

    FusionExecutor::GlobalBufferInfo info;
    info.tv = out_val->as<TensorView>();
    std::tie(info.sizes, info.strides) = inferShapeOfOutput(info.tv, expr_eval);
    auto dtype =
        (info.tv->dtype() == DataType::Index ? index_dtype : info.tv->dtype());
    info.type = data_type_to_aten(dtype);

    outputs.emplace_back(info);
  }
  return outputs;
}

} // namespace

std::vector<at::Tensor> allocOutputSpace(
    const at::ArrayRef<c10::IValue>& inputs,
    Fusion* fusion,
    const c10::Device& device) {
  auto fusion_inputs = KernelArgumentHolder::createKernelArgumentHolder(inputs);
  auto expr_eval = executor_utils::bindInputs(fusion_inputs, fusion);

  auto output_info =
      getOutputBufferInfo(fusion_inputs, expr_eval, PrimDataType::Int, fusion);

  return allocateOutputs(fusion, output_info, device, expr_eval);
}

void FusionExecutor::setUsedTVs() {
  auto used_vals = fusion()->usedMathVals();
  auto used_tvs = ir_utils::filterByType<TensorView>(used_vals);
  used_tvs_.clear();
  used_tvs_.insert(used_tvs_.begin(), used_tvs.begin(), used_tvs.end());
}

KernelArgumentHolder FusionExecutor::inferOutputSizes(
    Fusion* fusion,
    const KernelArgumentHolder& args,
    PrecomputedValues* evaluator_precomputed_values) {
  FUSER_PERF_SCOPE("FusionExecutor::inferOutputSizes");
  ExpressionEvaluator expr_eval;

  std::unique_ptr<PrecomputedValues> evaluator_precomputed_values_up = nullptr;
  if (evaluator_precomputed_values == nullptr) {
    evaluator_precomputed_values_up =
        std::make_unique<PrecomputedValues>(fusion);
    evaluator_precomputed_values_up->bindInputs(args);
    evaluator_precomputed_values_up->evaluate();
    evaluator_precomputed_values = evaluator_precomputed_values_up.get();
  }
  NVF_ERROR(evaluator_precomputed_values != nullptr);
  expr_eval.precomputedValues() = evaluator_precomputed_values;

  auto arg_index_type = args.getSmallestIndexTypeOfArguments();

  KernelArgumentHolder ret;
  ret.setDeviceIndex(args.getDeviceIndex());

  for (Val* output : fusion->outputs()) {
    NVF_ERROR(
        output->isA<TensorView>(),
        "Cannot allocate outputs that are not tensors.");
    auto output_tv = output->as<TensorView>();
    const auto& [sizes, strides] = inferShapeOfOutput(output_tv, expr_eval);
    const auto dtype = (output_tv->dtype() == DataType::Index)
        ? data_type_to_aten(arg_index_type)
        : data_type_to_aten(output_tv->dtype());
    ret.pushTensorProxy(sizes, strides, dtype);
  }
  return ret;
}

namespace {

// Make sure the index type of Kernel is valid
void validateIndexType(
    kir::Kernel* kernel,
    const CompileParams& compile_params) {
  NVF_ERROR(
      !compile_params.index_type.has_value() ||
          kernel->indexType() == compile_params.index_type.value(),
      "Kernel index type and compilation index type don't match. Kernel type: ",
      kernel->indexType(),
      ". Compilation index type: ",
      compile_params.index_type.value());
}

void validateCooperativeLaunch(
    CUfunction kernel,
    const LaunchParams& launch_params,
    int64_t device_index) {
  int num_blocks_per_SM = -1;
  auto block_size =
      launch_params.bdimx() * launch_params.bdimy() * launch_params.bdimz();
  NVFUSER_CUDA_SAFE_CALL(cuOccupancyMaxActiveBlocksPerMultiprocessor(
      &num_blocks_per_SM,
      kernel,
      (int)block_size,
      (size_t)launch_params.smem()));

  auto grid_size =
      launch_params.gdimx() * launch_params.gdimy() * launch_params.gdimz();
  auto max_active_blocks = num_blocks_per_SM *
      at::cuda::getDeviceProperties((c10::DeviceIndex)device_index)
          ->multiProcessorCount;
  NVF_ERROR(
      (int64_t)(max_active_blocks) >= grid_size,
      "Wanted to launch a cooperative kernel, however the number of blocks is greater than ",
      "what can be resident on the GPU at once. Need: ",
      grid_size,
      " (",
      launch_params.gdimx(),
      " * ",
      launch_params.gdimy(),
      " * ",
      launch_params.gdimz(),
      ") but limited to ",
      num_blocks_per_SM,
      " * ",
      at::cuda::getDeviceProperties(device_index)->multiProcessorCount);
}

// Dump fusion inputs and outputs as well as some useful fusion
// information. Note that inputs and outputs are those that are passed
// to FusionExecutor::runFusion, so outputs may not be given.
void dumpFusionArgs(
    int64_t fusion_id,
    const KernelArgumentHolder& args,
    const LaunchParams& launch_constraints,
    const CompileParams& compile_params,
    const std::vector<at::Tensor>& outputs) {
  debug() << "Arguments for fusion" << fusion_id << ":" << std::endl
          << "Inputs:" << std::endl;
  for (auto i : c10::irange(args.size())) {
    debug() << "  " << args[i] << std::endl;
  }
  debug() << "Outputs:" << std::endl;
  for (const auto& output : outputs) {
    debug() << "  " << output.scalar_type() << " " << output.sizes()
            << " (strides = " << output.strides() << ")" << std::endl;
  }
  debug() << launch_constraints.toString();
  debug() << "maxrregcount= " << compile_params.maxrregcount << std::endl;
}

// Dump arguments that are passed to a CUDA kernel call, which include
// the inputs and outputs of the fusion as well as temporary
// global-memory buffers. Unlike dumpFusionArgs, which dumps inputs
// and outputs passed to FusionExecutor::runFusion, this function
// dumps those that are passed to a CUDA kernel.
void dumpKernelArgs(
    int64_t fusion_id,
    const KernelArgumentHolder& args,
    size_t num_inputs,
    const std::vector<at::Tensor>& allocated_outputs,
    const std::vector<at::Tensor>& intermediates,
    const std::vector<FusionExecutor::GlobalBufferInfo>& intermediates_info) {
  using namespace PolymorphicValue_functions;
  debug() << "Arguments for kernel" << fusion_id << ":" << std::endl
          << "Inputs:" << std::endl;
  for (auto i : c10::irange(num_inputs)) {
    debug() << "  " << toString(*args[i]) << std::endl;
  }
  debug() << "Outputs:" << std::endl;
  // note: add aliased outputs here.
  for (const auto& output : allocated_outputs) {
    debug() << "  " << output.scalar_type() << " " << output.sizes()
            << " (strides = " << output.strides()
            << ", address = " << output.data_ptr() << ")" << std::endl;
  }
  debug() << "Intermediate global buffers:" << std::endl;
  for (const auto i : c10::irange(intermediates.size())) {
    const auto& buffer = intermediates.at(i);
    const auto& zero_init = intermediates_info.at(i).zero_init;
    const auto& resets_to_zero = intermediates_info.at(i).resets_to_zero;
    debug() << "  " << buffer.scalar_type() << " " << buffer.sizes()
            << " is_zero_initialized: " << zero_init
            << " resets_to_zero: " << resets_to_zero << std::endl;
  }
}

FusionExecutor::GlobalBufferInfo getGlobalBufferAllocationInfo(
    const at::Tensor& at_tensor) {
  FusionExecutor::GlobalBufferInfo info{
      .sizes = at_tensor.sizes().vec(),
      .strides = at_tensor.strides().vec(),
      .type = at_tensor.scalar_type()};
  return info;
}

} // namespace

void FusionExecutor::initializeExecutorEntry(
    ExecutorEntry& executor_entry,
    const KernelArgumentHolder& args,
    const LaunchParams& launch_constraints,
    const CompileParams& compile_params,
    const std::vector<at::Tensor>& outputs,
    DataType index_type) {
  FUSER_PERF_SCOPE("FusionExecutor::initializeExecutorEntry");

  ExpressionEvaluator expr_eval;
  evaluatorPrecomputedValues()->bindInputs(args);
  expr_eval.precomputedValues() = evaluatorPrecomputedValues().get();

  auto launch_params = computeLaunchParams(
      launch_constraints, expr_eval, warp_size_, index_type);

  for (const auto& entry : kernel()->summary().validations) {
    NVF_CHECK(expr_eval.evaluate(entry.first).as<bool>(), entry.second);
  }

  executor_utils::validateVectorizedTensors(
      kernel(), args, outputs, compileTimeDataCache(), expr_eval);

  executor_utils::validateCircularBuffering(kernel(), expr_eval);

  std::vector<GlobalBufferInfo> output_info;

  if (outputs.empty()) {
    output_info =
        getOutputBufferInfo(args, expr_eval, index_type, lowered_->kernel());
  } else {
    // Need to save the information necessary for allocations as
    // future uses of this ExecutorEntry may not be provided with
    // allocated outputs
    for (const auto& output : outputs) {
      output_info.emplace_back(getGlobalBufferAllocationInfo(output));
    }
  }

  auto intermediates = getIntermediateBufferInfo(expr_eval, index_type);

  // All information is gathered. Save it to ExecutorEntry
  executor_entry.launch_params = launch_params;
  executor_entry.outputs = output_info;
  executor_entry.intermediates = intermediates;
  executor_entry.init = true;
}

/// Copies the data, logical_size, and alloc_stride parameters to the
/// appropriate parts of entry.args[idx].
///
/// For GPU tensors, we pass a Tensor<type, rank, rank> struct (see
/// runtime/tensor.cu), where the rank describes the number of elements in the
/// shape and stride arrays. The actual shapes and strides are dynamic, but the
/// type and rank of the tensors are actually static (changing them would need
/// a new FusionDefinition). So we create the storage area for the
/// Tensor<t,r,r> during ::computeArgs, and then in this function we just
/// update that memory with the current values for the tensor's base address,
/// shape, and strides.
///
/// @param entry the entry we have previously setup for this fusion
/// @param idx the index into entry.args and related parallel arrays in the
///            entry.
/// @param idx_type_size generally sizeof(int32_t) or sizeof(int64_t); used for
///                      computing how large the arrays to copy are.
static void fillTensorArgMetadata(
    FusionExecutor::ExecutorEntry& entry,
    const PolymorphicValue& tensor_metadata,
    size_t idx,
    size_t idx_type_size) {
  void* data = tensor_metadata->*&TensorMetaData::data;
  // g++ has trouble inferring the types of more complicated fields through our
  // *& operators. Creating an `auto` alias as a temporary resolves this
  // problem.
#define TMD_ARRAY_REF(pv, field)                  \
  ({                                              \
    const auto& fld_tmp_ = pv->*&field;           \
    const c10::IntArrayRef& fld_aref_ = fld_tmp_; \
    fld_aref_;                                    \
  })
  const c10::IntArrayRef& shape =
      TMD_ARRAY_REF(tensor_metadata, TensorMetaData::logical_size);
  const c10::IntArrayRef& strides =
      TMD_ARRAY_REF(tensor_metadata, TensorMetaData::alloc_stride);
#undef TMD_ARRAY_REF

  // These are the three offsets we need to copy into.
  std::array<std::byte*, 3> offsets = {
      entry.args[idx].data(), // data ptr
      entry.args[idx].data() + sizeof(void*), // shape array
      // strides array:
      entry.args[idx].data() + sizeof(void*) + shape.size() * idx_type_size,
  };

  memcpy(offsets[0], &data, sizeof(void*));
  switch (idx_type_size) {
    case sizeof(int64_t): {
      // we use i64's for our sizes, so can use a simple copy here
      memcpy(offsets[1], shape.data(), shape.size() * sizeof(int64_t));
      memcpy(offsets[2], strides.data(), strides.size() * sizeof(int64_t));
    } break;
    case sizeof(int32_t): {
      // we need to cast per-element, so need a loop.
      // This case happens when the kernel uses 32bit indices. Since we
      // (specifically TensorMetaData) store indices in 64bit, we can't
      // directly copy our buffer into the args buffer. We thus have to
      // manually downcast each element to fit in the smaller buffer.
      for (size_t i = 0; i < shape.size(); ++i) {
        const int32_t shp = static_cast<int32_t>(shape[i]);
        memcpy(offsets[1] + i * sizeof(int32_t), &shp, sizeof(int32_t));
      }
      // In rare cases we have fewer strides than shapes
      for (size_t i = 0; i < strides.size(); ++i) {
        const int32_t strd = static_cast<int32_t>(strides[i]);
        memcpy(offsets[2] + i * sizeof(int32_t), &strd, sizeof(int32_t));
      }
    } break;
    default:
      NVF_CHECK(0, "Unhandled index type size");
      break;
  }
}

// set the arguments that we'll pass to cuLaunchKernel. This should happen
// when we change the rank of a tensor or the number of arguments to a kernel.
// It does not need to happen when only shapes change---use recomputeArgs for
// that.
void FusionExecutor::computeArgs(
    ExecutorEntry& entry,
    ExpressionEvaluator& expr_eval,
    const kir::Kernel* kernel) const {
  FUSER_PERF_SCOPE("FusionExecutor::computeArgs");

  const std::vector<Val*>& params = kernel->parameters();
  entry.args.resize(params.size());
  entry.arg_ptrs.resize(params.size());
  const PrimDataType idx_type = kernel->indexType();
  for (size_t p = 0; p < params.size(); ++p) {
    entry.args[p] = getKernelArgument(expr_eval, params[p], idx_type);
    entry.arg_ptrs[p] = entry.args[p].data();
  }
}

// Reset the arguments that we'll pass to cuLaunchKernel. This needs to be
// invoked on every shape change.
void FusionExecutor::recomputeArgs(
    ExecutorEntry& entry,
    ExpressionEvaluator& expr_eval,
    const kir::Kernel* kernel) const {
  FUSER_PERF_SCOPE("FusionExecutor::recomputeArgs");
  // assert(entry.init && "entry was never initialized");

  const std::vector<Val*>& params = kernel->parameters();
  const PrimDataType idx_type = kernel->indexType();
  // assert(entry.args.size() == params.size());
  // assert(entry.arg_ptrs.size() == params.size());
  // assert(params.size() >= args.size());
  for (size_t p = 0; p < params.size(); ++p) {
    PolymorphicValue pv = expr_eval.evaluate(params[p]);
    if (pv.is<at::Tensor>() && pv.as<at::Tensor>().is_cuda()) {
      // GPU tensors are not passed directly: instead we pass a Tensor<type,
      // rank, rank> struct. The pointer and dimensions are dynamic, but the
      // types and ranks are actually static (changing the rank or the types
      // would need to be done via a new FusionDefinition). As such, we created
      // the Tensor<t, r, r> struct during ::computeArgs, and here we just fill
      // in the base address, shape, and stride arrays to cover whatever new
      // tensors we got this round.
      TensorView* mtv = dynamic_cast<TensorView*>(params[p]);
      const Val* mdexpr = IrBuilder::metadataExpr(mtv);
      const PolymorphicValue& tmd = expr_eval.evaluate(mdexpr);
      const size_t idx_type_size =
          PrimDataType::Int == idx_type ? sizeof(int64_t) : sizeof(int32_t);
      fillTensorArgMetadata(entry, tmd, p, idx_type_size);
    } else {
      entry.args[p] = getKernelArgument(expr_eval, params[p], idx_type);
    }
    entry.arg_ptrs[p] = entry.args[p].data();
  }
}

void FusionExecutor::recompileKernel(
    const LaunchParams& new_launch_params,
    const CompileParams& new_compile_params) {
  if (new_launch_params.nThreads() <= block_size_high_water_mark_ &&
      new_compile_params.maxrregcount == maxrregcount_high_water_mark_) {
    return;
  }

  const auto structured_code = getStructuredCode();
  block_size_high_water_mark_ = new_launch_params.nThreads();
  maxrregcount_high_water_mark_ = new_compile_params.maxrregcount;

  compiled_kernel_ = executor_utils::getCompiledKernel(
      kernel_code_,
      structured_code,
      kernelName(),
      kernel_id_,
      new_compile_params,
      block_size_high_water_mark_);

  resetCompiledKernelProperties();

  if (kernel()->summary().has_cooperative_grid_reduction) {
    // We need to increase shared memory before kernel launch, but also before
    // calling into `validateCooperativeLaunch`!
    // So we need to do it there before calling into the validation, to avoid
    // false positives
    ensureAvailableDynamicSmemSize(new_launch_params.smem());
    validateCooperativeLaunch(
        compiled_kernel_->function, new_launch_params, options_.device.index());
  }
}

int64_t FusionExecutor::getAvailableDynamicSmemSize() {
  NVF_ERROR(
      hasCompiledKernel(),
      "Cannot get dynamic smem size unless kernel is compiled");
  if (!available_dynamic_smem_size_.has_value()) {
    int size = 0;
    NVFUSER_CUDA_SAFE_CALL(cuFuncGetAttribute(
        &size,
        CU_FUNC_ATTRIBUTE_MAX_DYNAMIC_SHARED_SIZE_BYTES,
        compiled_kernel_->function));
    available_dynamic_smem_size_ = size;
  }
  return available_dynamic_smem_size_.value();
}

int64_t FusionExecutor::getStaticSmemSize() {
  NVF_ERROR(
      hasCompiledKernel(),
      "Cannot get static smem size unless kernel is compiled");
  if (!static_smem_size_.has_value()) {
    int size = 0;
    // Is this really a costly operation worth caching?
    NVFUSER_CUDA_SAFE_CALL(cuFuncGetAttribute(
        &size,
        CU_FUNC_ATTRIBUTE_SHARED_SIZE_BYTES,
        compiled_kernel_->function));
    static_smem_size_ = size;
  }
  return static_smem_size_.value();
}

void FusionExecutor::validateDynamicSmemSize(int64_t dynamic_smem_size) {
  // If specified, check that dynamic smem size matches what the scheduler
  // expects
  int64_t expected_dynamic_smem_size = fusion()->expectedDynamicSmemBytes();
  if (expected_dynamic_smem_size >= 0) {
    NVF_ERROR(
        dynamic_smem_size == expected_dynamic_smem_size,
        "Actual dynamic smem allocation ",
        dynamic_smem_size,
        " does not match expected size ",
        expected_dynamic_smem_size);
  }
  NVF_ERROR(
      getStaticSmemSize() + dynamic_smem_size < device_smem_limit_,
      "The total shared memory allocation is larger than available memory.",
      " Dynamic size: ",
      dynamic_smem_size,
      ". Static size: ",
      getStaticSmemSize(),
      ". Required total size: ",
      getStaticSmemSize() + dynamic_smem_size,
      ". Device limit size: ",
      device_smem_limit_);
}

int64_t FusionExecutor::ensureAvailableDynamicSmemSize(
    int64_t dynamic_smem_size) {
  NVF_ERROR(
      hasCompiledKernel(),
      "Cannot set dynamic smem size unless kernel is compiled");
  if (dynamic_smem_size > getAvailableDynamicSmemSize()) {
    validateDynamicSmemSize(dynamic_smem_size);
    NVFUSER_CUDA_SAFE_CALL(cuFuncSetAttribute(
        compiled_kernel_->function,
        CU_FUNC_ATTRIBUTE_MAX_DYNAMIC_SHARED_SIZE_BYTES,
        dynamic_smem_size));
    available_dynamic_smem_size_ = dynamic_smem_size;
  }
  return getAvailableDynamicSmemSize();
}

void FusionExecutor::resetCompiledKernelProperties() {
  available_dynamic_smem_size_.reset();
  static_smem_size_.reset();
}

std::vector<at::Tensor> FusionExecutor::evaluateFusionOutputs(
    KernelArgumentHolder& args,
    std::vector<at::Tensor> outputs,
    ExpressionEvaluator& expr_eval) {
  // TODO: Add relevant profiling code.
  if (outputs.empty()) {
    for (const auto& out_val : fusion()->outputs()) {
      auto out_tensor =
          expr_eval.evaluate(out_val->as<TensorView>()).as<at::Tensor>();
      expr_eval.bind(out_val, out_tensor);
      outputs.emplace_back(out_tensor);
    }
  }
  args.push(outputs);
  return outputs;
}

namespace {
at::Tensor findBufferForFusionOutput(
    const std::vector<at::Tensor>& out_tensors,
    const Val* fusion_out,
    const Fusion* fusion) {
  auto i =
      std::find(fusion->outputs().begin(), fusion->outputs().end(), fusion_out);
  NVF_ERROR(i != fusion->outputs().end());
  auto index = std::distance(fusion->outputs().begin(), i);
  return out_tensors[index];
}
} // namespace

std::vector<at::Tensor> FusionExecutor::runFusion(
    KernelArgumentHolder& args,
    const LaunchParams& launch_constraints,
    CompileParams compile_params,
    std::vector<at::Tensor> outputs) {
  FUSER_PERF_SCOPE("FusionExecutor::runFusion");

  if (isProfilerEnabled()) {
    NVF_CHECK(
        group_id_ >= 0,
        "An invalid segment id is passed to FusionProfiler!:",
        group_id_);
    SegmentProfiler& sprof = FusionProfiler::segment(group_id_);
    sprof.inputBytesAccessed(inputBytesProcessed(args));
    sprof.scheduler(toString(heuristic_));
    sprof.startKernel(args.getDeviceIndex());
  }

  NVF_ERROR(isCompiled());
  NVF_ERROR(
      outputs.empty() || (outputs.size() == fusion()->outputs().size()),
      __func__,
      " provided number of outputs does not match fusion output");

  // Bind fusion inputs
  ExpressionEvaluator expr_eval;
  const auto& inputs = fusion()->inputs();
  for (const auto i : c10::irange(inputs.size())) {
    expr_eval.bind(inputs[i], *args[i]);
  }

  if (isExpressionEvaluated(fusion())) {
    outputs = evaluateFusionOutputs(args, outputs, expr_eval);
    if (isProfilerEnabled()) {
      auto& sprof = FusionProfiler::segment(group_id_);
      sprof.stopKernel();
      sprof.outputBytesAccessed(outputBytesProcessed(outputs));
    }
    return outputs;
  }

  if (host_ir_container_ != nullptr) {
    if (outputs.empty()) {
      std::vector<GlobalBufferInfo> output_info = getOutputBufferInfo(
          args, expr_eval, PrimDataType::Int, host_ir_container_.get());
      outputs = allocateOutputs(
          host_ir_container_.get(), output_info, options_.device, expr_eval);
    }
    for (Expr* e : host_ir_container_->topLevelExprs()) {
      NVF_ERROR(e->isA<Communication>());
      auto* communication = e->as<Communication>();
      c10d::Backend* backend =
          communicator_->getBackendForTeam(communication->team(), std::nullopt);
      auto in_tensor = expr_eval.evaluate(communication->in()).as<at::Tensor>();
      at::Tensor out_tensor = findBufferForFusionOutput(
          outputs, communication->out(), host_ir_container_.get());
      c10::intrusive_ptr<c10d::Work> work = postSingleCommunication(
          communication,
          communicator_->deviceId(),
          backend,
          in_tensor,
          out_tensor);
      if (work != nullptr) {
        work->wait();
      }
    }
    return outputs;
  }

  NVF_ERROR(validKernelId(), "Invalid kernel id for FusionExecutor.");
  NVF_ERROR(
      !args.getCacheId().has_value() || outputs.empty(),
      "short cut input cache is not compatible with pre-allocated output");

  validateIndexType(kernel(), compile_params);

  const auto num_inputs = args.size();

  if (isDebugDumpEnabled(DebugDumpOption::FusionArgs)) {
    dumpFusionArgs(
        fusion_id_, args, launch_constraints, compile_params, outputs);
  }

  c10::DeviceGuard dg(options_.device);
  auto stream = at::cuda::getCurrentCUDAStream();
  at::cuda::jit::initializeCudaContext();
  NVF_ERROR(lowered_);

  // Placeholder for the case where parameter cache is not used
  ExecutorEntry temporary_executor_entry;

  ExecutorEntry* executor_entry =
      args.getCacheId().has_value() && !disable_parameter_cache_
      ? &executor_entry_lookup_[*args.getCacheId()]
      : &temporary_executor_entry;

  // Initialize the executor entry if not initlized
  if (!executor_entry->init) {
    initializeExecutorEntry(
        *executor_entry,
        args,
        launch_constraints,
        compile_params,
        outputs,
        kernel()->indexType());
  }

  recompileKernel(executor_entry->launch_params, compile_params);

  // TODO: Why does this need to be stored in the class?
  launch_params_ = executor_entry->launch_params;

  // context manager to disable auto grad for `empty_cuda` calls later
  at::AutoDispatchBelowADInplaceOrView non_variable_type_mode;

  // only allocate outputs when not given
  if (outputs.empty()) {
    outputs = allocateOutputs(
        fusion(), executor_entry->outputs, options_.device, expr_eval);
  }
  args.push(outputs);

  for (const auto i : c10::irange(outputs.size())) {
    auto output = kernel()->outputs()[i];
    if (std::any_of(
            kernel()->inputs().begin(),
            kernel()->inputs().end(),
            [&](const auto& in) { return in == output; })) {
      // Skip trivially forwarded outputs because they are just placeholders
      continue;
    }
    expr_eval.bind(output, *args[inputs.size() + i]);
  }

  std::vector<at::Tensor> intermediates;
  at::Tensor profile_buffer;
  {
    for (const auto i : c10::irange(executor_entry->intermediates.size())) {
      const auto& buf_info = executor_entry->intermediates.at(i);
      bool has_expansion = false;
      std::vector<int64_t> unexpanded_sizes;
      unexpanded_sizes.reserve(buf_info.sizes.size());
      NVF_ERROR(buf_info.sizes.size() == buf_info.strides.size())
      for (const auto j : c10::irange(buf_info.sizes.size())) {
        if (buf_info.strides[j] == 0) {
          has_expansion = true;
          unexpanded_sizes.push_back(1L);
        } else {
          unexpanded_sizes.push_back(buf_info.sizes[j]);
        }
      }
      at::Tensor intermediate_buffer;
      if (buf_info.zero_init) {
        if (isOptionEnabled(EnableOption::ReuseZeroedMemory) ||
            buf_info.resets_to_zero) {
          // Allow access to reusable zeroed memory if buffer is guaranteed
          // to reset to zero upon completion of the kernel, or if we have
          // enabled the option (unsafe)
          intermediate_buffer = contigZeroedTensor(
              unexpanded_sizes, buf_info.type, options_.device);
        } else {
          intermediate_buffer = at::zeros(
              unexpanded_sizes,
              at::TensorOptions().dtype(buf_info.type).device(options_.device));
        }
      } else {
        intermediate_buffer = at::native::empty_cuda(
            unexpanded_sizes,
            buf_info.type,
            c10::nullopt,
            options_.device,
            c10::nullopt);
        if (shouldFillAllocationWithNan()) {
          fillTensorWithNan(intermediate_buffer);
        }
      }
      if (has_expansion) {
        intermediate_buffer =
            at::native::expand(intermediate_buffer, buf_info.sizes);
      }
      args.push(intermediate_buffer);
      intermediates.push_back(intermediate_buffer);
      expr_eval.bind(
          kernel()->summary().global_allocations.at(i)->buffer(),
          *args[inputs.size() + outputs.size() + i]);
      if (buf_info.is_profile_buffer) {
        profile_buffer = intermediate_buffer;
      }
    }
  }

  if (executor_entry->args.empty()) {
    computeArgs(*executor_entry, expr_eval, kernel());
  }

  if (isDebugDumpEnabled(DebugDumpOption::LaunchParam)) {
    launch_params_.print();
  }

  if (isDebugDumpEnabled(DebugDumpOption::KernelArgs)) {
    dumpKernelArgs(
        fusion_id_,
        args,
        num_inputs,
        outputs,
        intermediates,
        executor_entry->intermediates);
  }

  if (isDebugDumpEnabled(DebugDumpOption::IndexType)) {
    debug() << "Index type: " << kernel()->indexType() << std::endl;
  }

  executor_utils::CudaKernelTimer timer(stream);

  if (execute_kernel_ && !kernel()->topLevelExprs().empty()) {
    ensureAvailableDynamicSmemSize(executor_entry->launch_params.smem());

    recomputeArgs(*executor_entry, expr_eval, kernel());

    if (isDebugDumpEnabled(DebugDumpOption::Occupancy) ||
        isDebugDumpEnabled(DebugDumpOption::PerfDebugVerbose)) {
      int blocks_per_sm = -1;
      NVFUSER_CUDA_SAFE_CALL(cuOccupancyMaxActiveBlocksPerMultiprocessor(
          &blocks_per_sm,
          compiled_kernel_->function,
          launch_params_.nThreads(),
          launch_params_.smem()));

      const int64_t device_id =
          static_cast<unsigned char>(options_.device.index());
      const auto prop =
          at::cuda::getDeviceProperties((c10::DeviceIndex)device_id);
      const int64_t warps_per_sm =
          ceilDiv(blocks_per_sm * launch_params_.nThreads(), prop->warpSize);

      const int hw_max_warps =
          prop->maxThreadsPerMultiProcessor / prop->warpSize;
      const float occupancy = (float)warps_per_sm / (float)hw_max_warps * 100.f;
      setKernelOccupancy(occupancy);
      std::ostringstream oss;
      oss << std::fixed << std::setprecision(2) << occupancy << "%";

      debug() << "num_sms=" << prop->multiProcessorCount
              << ", blocks_per_sm=" << blocks_per_sm
              << ", warps_per_sm=" << warps_per_sm
              << ", occupancy=" << oss.str() << std::endl;
    }

    if (!kernel()->summary().has_cooperative_grid_reduction) {
      FUSER_PERF_SCOPE("ExecutorRunFusion::cuLaunchKernel");
      NVFUSER_CUDA_SAFE_CALL(cuLaunchKernel(
          compiled_kernel_->function,
          launch_params_.gdimx(),
          launch_params_.gdimy(),
          launch_params_.gdimz(),
          launch_params_.bdimx(),
          launch_params_.bdimy(),
          launch_params_.bdimz(),
          launch_params_.smem(),
          stream,
          executor_entry->arg_ptrs.data(),
          nullptr));
    } else {
      FUSER_PERF_SCOPE("ExecutorRunFusion::cuLaunchCooperativeKernel");
      NVFUSER_CUDA_SAFE_CALL(cuLaunchCooperativeKernel(
          compiled_kernel_->function,
          launch_params_.gdimx(),
          launch_params_.gdimy(),
          launch_params_.gdimz(),
          launch_params_.bdimx(),
          launch_params_.bdimy(),
          launch_params_.bdimz(),
          launch_params_.smem(),
          stream,
          executor_entry->arg_ptrs.data()));
    }
  }

  releaseZeroedMemory();

  if (isOptionEnabled(EnableOption::KernelProfile)) {
    debug() << kernel()->profile().toString(profile_buffer);
  }

  if (isProfilerEnabled()) {
    auto& sprof = FusionProfiler::segment(group_id_);
    sprof.stopKernel();
    sprof.outputBytesAccessed(outputBytesProcessed(outputs));
  }

  return outputs;
}

int64_t FusionExecutor::inputBytesProcessed(const KernelArgumentHolder& args) {
  int64_t num_bytes = 0;
  // Figure how many bytes are inputs, outputs, and temporary buffers
  for (auto i : c10::irange(args.size())) {
    if (args[i]->is<at::Tensor>()) {
      auto t = args[i]->as<at::Tensor>();
      num_bytes += static_cast<int64_t>(t.storage().nbytes());
    }
  }
  return num_bytes;
}

int64_t FusionExecutor::outputBytesProcessed(
    const std::vector<at::Tensor>& outputs) {
  int64_t num_bytes = 0;
  for (auto i : c10::irange(outputs.size())) {
    const auto& output = outputs.at(i);
    // NOTE: this assumes that all output elements correspond to a single
    // store
    num_bytes += static_cast<int64_t>(output.storage().nbytes());
  }
  return num_bytes;
}

void FusionExecutor::compileRtc(
    const std::string& code,
    const std::string& name,
    bool structured,
    PrimDataType index_type) {
  FUSER_PERF_SCOPE("FusionExecutor::compileRtc");
  NVF_ERROR(
      index_type == PrimDataType::Int || index_type == PrimDataType::Int32 ||
          "Invalid index type: ",
      index_type);

  createKernelId();

  std::string scode;
  if (!structured) {
    scode = getStructuredCode(code, index_type);
  } else {
    scode = code;
  }
  compiled_kernel_ =
      executor_utils::getCompiledKernel(std::nullopt, scode, name, kernel_id_);
}

float FusionExecutor::runRtc(
    const LaunchParams& launch_params,
    const std::vector<at::Tensor>& args,
    PrimDataType index_type) {
  FUSER_PERF_SCOPE("FusionExecutor::runRtc");

  c10::DeviceGuard dg(options_.device);
  auto stream = at::cuda::getCurrentCUDAStream();

  cudaEvent_t start_event = {};
  cudaEvent_t finish_event = {};

  NVFUSER_CUDA_RT_SAFE_CALL(cudaEventCreate(&start_event));
  NVFUSER_CUDA_RT_SAFE_CALL(cudaEventCreate(&finish_event));

  NVFUSER_CUDA_RT_SAFE_CALL(cudaEventRecord(start_event, stream));

  std::vector<std::vector<std::byte>> data;
  std::vector<void*> pointers;

  for (const auto& input : args) {
    auto dtype =
        std::get<PrimDataType>(aten_to_data_type(input.scalar_type()).type);
    DataType metadata_type = globalTensorMetaData(dtype, input.dim());

    std::shared_ptr<Struct> struct_ = std::make_shared<TensorMetaData>();
    TensorMetaData* metadata = (TensorMetaData*)struct_.get();
    metadata->dtype = dtype;
    metadata->data = input.data_ptr();
    metadata->logical_size = input.sizes();
    metadata->logical_stride = input.strides();
    metadata->alloc_size = input.sizes();
    metadata->alloc_stride = input.strides();

    data.emplace_back(polymorphicValueToBytes(
        PolymorphicValue(std::move(struct_)), metadata_type, index_type));
    pointers.emplace_back(data.back().data());
  }

  NVFUSER_CUDA_SAFE_CALL(cuLaunchKernel(
      compiled_kernel_->function,
      launch_params.gdimx(),
      launch_params.gdimy(),
      launch_params.gdimz(),
      launch_params.bdimx(),
      launch_params.bdimy(),
      launch_params.bdimz(),
      launch_params.smem(),
      stream,
      pointers.data(),
      nullptr));

  NVFUSER_CUDA_RT_SAFE_CALL(cudaEventRecord(finish_event, stream));
  NVFUSER_CUDA_RT_SAFE_CALL(cudaEventSynchronize(start_event));
  NVFUSER_CUDA_RT_SAFE_CALL(cudaEventSynchronize(finish_event));

  float kernel_time_ms = 0;
  NVFUSER_CUDA_RT_SAFE_CALL(
      cudaEventElapsedTime(&kernel_time_ms, start_event, finish_event));
  NVFUSER_CUDA_RT_SAFE_CALL(cudaEventDestroy(start_event));
  NVFUSER_CUDA_RT_SAFE_CALL(cudaEventDestroy(finish_event));

  return kernel_time_ms;
}

flatbuffers::Offset<serde::FusionExecutor> FusionExecutor::serialize(
    flatbuffers::FlatBufferBuilder& builder) const {
  // See table definition for FusionExecutor in serde/fusion_cache.fbs
  using fb_executor_entry = flatbuffers::Offset<serde::ExecutorEntry>;

  // Separate unordered_map for executor_entry_lookup into key and value
  // vectors. The key value is the cache_id value in the KernelArgumentHolder.
  std::vector<size_t> executor_entry_lookup_keys_fb;
  std::vector<fb_executor_entry> executor_entry_lookup_values_fb;
  for (const auto& [key, value] : executor_entry_lookup_) {
    executor_entry_lookup_keys_fb.push_back(key);
    executor_entry_lookup_values_fb.push_back(serialize(builder, value));
  }

  // When compilation is skipped, avoid serializing cubin because it doesn't
  // exist. The remaining fields are also not necessary in this case.
  if (!hasCompiledKernel()) {
    return serde::CreateFusionExecutorDirect(builder);
  }

  return serde::CreateFusionExecutorDirect(
      builder,
      device_smem_limit_,
      block_size_high_water_mark_,
      maxrregcount_high_water_mark_,
      warp_size_,
      toUnderlying(heuristic_),
      fusion_id_,
      concrete_id_,
      runtime_id_,
      group_id_,
      kernel_code_.c_str(),
      &executor_entry_lookup_keys_fb,
      &executor_entry_lookup_values_fb,
      toUnderlying(kernel()->indexType()),
      serialize(builder, compiled_kernel_.get()));
}

flatbuffers::Offset<serde::CudaKernel> FusionExecutor::serialize(
    flatbuffers::FlatBufferBuilder& builder,
    const executor_utils::CompiledKernel* compiled_kernel) const {
  NVF_ERROR(
      compiled_kernel_ != nullptr &&
          (!compiled_kernel->cubin.empty() || !compiled_kernel->ptx.empty()),
      "Expected compiled cuda kernel before serializing FusionExecutor.");

  auto fb_kernel_name = builder.CreateString(compiled_kernel->kernel_name);
  auto fb_compile_args = builder.CreateString(compiled_kernel->compile_args);

  flatbuffers::Offset<flatbuffers::Vector<uint8_t>> fb_cubin = 0;
  flatbuffers::Offset<flatbuffers::String> fb_cubin_filename = 0;
  if (!compiled_kernel->cubin.empty()) {
    uint8_t* cubin_ptr = nullptr;
    fb_cubin = builder.CreateUninitializedVector(
        compiled_kernel->cubin.size(), &cubin_ptr);
    std::copy(
        compiled_kernel->cubin.begin(),
        compiled_kernel->cubin.end(),
        cubin_ptr);
    fb_cubin_filename = builder.CreateString(compiled_kernel->cubin_filename);
  }

  flatbuffers::Offset<flatbuffers::Vector<uint8_t>> fb_ptx = 0;
  flatbuffers::Offset<flatbuffers::String> fb_ptx_filename = 0;
  if (!compiled_kernel->ptx.empty()) {
    uint8_t* ptx_ptr = nullptr;
    fb_ptx = builder.CreateUninitializedVector(
        compiled_kernel->ptx.size(), &ptx_ptr);
    std::copy(
        compiled_kernel->ptx.begin(), compiled_kernel->ptx.end(), ptx_ptr);
    fb_ptx_filename = builder.CreateString(compiled_kernel->ptx_filename);
  }

  serde::CudaKernelBuilder ckb(builder);
  ckb.add_cubin(fb_cubin);
  ckb.add_cubin_filename(fb_cubin_filename);
  ckb.add_ptx(fb_ptx);
  ckb.add_ptx_filename(fb_ptx_filename);
  ckb.add_kernel_name(fb_kernel_name);
  ckb.add_compile_args(fb_compile_args);
  ckb.add_block_size(compiled_kernel->block_size);
  return ckb.Finish();
}

flatbuffers::Offset<serde::ExecutorEntry> FusionExecutor::serialize(
    flatbuffers::FlatBufferBuilder& builder,
    const ExecutorEntry& data) const {
  // See table definition for ExecutorEntry in serde/fusion_cache.fbs

  // Serialize GlobalBufferInfo for outputs.
  // We map the output TensorView pointer to its corresponding position in
  // fusion outputs assuming that the output ordering is consistent.
  using fb_global_buffer_info = flatbuffers::Offset<serde::GlobalBufferInfo>;
  std::vector<fb_global_buffer_info> outputs_fb;
  outputs_fb.reserve(data.outputs.size());
  for (const auto& buffer : data.outputs) {
    auto tv_iter = std::find(
        kernel()->outputs().cbegin(), kernel()->outputs().cend(), buffer.tv);
    auto tv_position = (tv_iter == kernel()->outputs().cend())
        ? -1
        : std::distance(kernel()->outputs().cbegin(), tv_iter);
    outputs_fb.push_back(
        serialize(builder, buffer, tv_position, true /* is_fusion_output */));
  }

  // Serialize GlobalBufferInfo for intermediates.
  // We map the intermediate TensorView pointer to its corresponding position in
  // KernelSummary global allocations. We assume that the ordering is consistent
  // between GpuLower objects with the same scheduled fusion.
  std::vector<fb_global_buffer_info> intermediates_fb;
  intermediates_fb.reserve(data.intermediates.size());
  for (const auto& buffer : data.intermediates) {
    auto match_tv_predicate = [buffer_tv = buffer.tv](const kir::Allocate* a) {
      return a->buffer() == buffer_tv;
    };
    auto tv_iter = std::find_if(
        kernel()->summary().global_allocations.cbegin(),
        kernel()->summary().global_allocations.cend(),
        match_tv_predicate);
    auto tv_position =
        (tv_iter == kernel()->summary().global_allocations.cend())
        ? -1
        : std::distance(
              kernel()->summary().global_allocations.cbegin(), tv_iter);
    intermediates_fb.push_back(
        serialize(builder, buffer, tv_position, false /* is_fusion_output */));
  }

  return serde::CreateExecutorEntryDirect(
      builder,
      data.init,
      data.launch_params.serialize(builder),
      &outputs_fb,
      &intermediates_fb);
}

flatbuffers::Offset<serde::GlobalBufferInfo> FusionExecutor::serialize(
    flatbuffers::FlatBufferBuilder& builder,
    const GlobalBufferInfo& data,
    int64_t tv_position,
    bool is_fusion_output) const {
  // See table definition for GlobalBufferInfo in serde/fusion_cache.fbs
  return serde::CreateGlobalBufferInfoDirect(
      builder,
      tv_position,
      &data.sizes,
      &data.strides,
      nvfuser::toUnderlying(data.type),
      data.zero_init,
      data.resets_to_zero,
      data.is_profile_buffer,
      is_fusion_output);
}

void FusionExecutor::deserialize(
    const serde::FusionExecutor* buffer,
    Fusion* fusion,
    int8_t device_index,
    CompileParams compile_params,
    ScheduleHeuristic heuristic,
    int64_t fusion_id,
    int64_t concrete_id,
    int64_t runtime_id,
    int64_t group_id) {
  // See table definition for FusionExecutor in serde/fusion_cache.fbs

  NVF_ERROR(buffer != nullptr, "serde::FusionExecutor is nullptr.");

  // TODO Should we set fusion_id, concrete_id, runtime_id, and group_id when we
  // skip compilation?
  if (isExpressionEvaluated(fusion)) {
    fusion_ = std::make_unique<Fusion>(*fusion);
    NVF_ERROR(!hasCompiledKernel(), "Failed to deserialize FusionExecutor");
    return;
  }

  NVF_ERROR(
      fusion_id == buffer->fusion_id(),
      "Expected given fusion_id to match serde fusion_id.");
  NVF_ERROR(
      concrete_id == buffer->concrete_id(),
      "Expected given concrete_id to match serde concrete_id.");
  NVF_ERROR(
      runtime_id == buffer->runtime_id(),
      "Expected given runtime_id to match serde runtime_id.");
  NVF_ERROR(
      group_id == buffer->group_id(),
      "Expected given group_id to match serde group_id.");
  NVF_ERROR(
      toUnderlying(heuristic) == buffer->heuristic(),
      ": ",
      toUnderlying(heuristic),
      " vs ",
      buffer->heuristic());

  // Initialize CompileOptions
  options_.device = c10::Device(c10::DeviceType::CUDA, device_index);
  c10::DeviceGuard dg(options_.device);

  // Initialize internal fields
  device_smem_limit_ = buffer->device_smem_limit();
  block_size_high_water_mark_ = buffer->block_size_high_water_mark();
  maxrregcount_high_water_mark_ = buffer->maxrregcount_high_water_mark();
  warp_size_ = buffer->warp_size();
  kernel_code_ = buffer->kernel_code()->str();

  // KernelDB query checks kernel_code string and compile_params before
  // copying cubin.
  compile_params.index_type = serde::mapToNvfuserDtype(buffer->index_type());
  compile_params.maxrregcount = maxrregcount_high_water_mark_;

  // Get lowered fusion
  lowered_ = std::make_unique<GpuLower>(fusion, compile_params);
  lowered_->run();

  // Replace integers that are tensor sizes by named scalars like "T0.size[0]"
  createKernelId(
      heuristic,
      buffer->fusion_id(),
      buffer->concrete_id(),
      buffer->runtime_id(),
      buffer->group_id());
  setUsedTVs();

  // GlobalBufferInfo requires lowered kernel before deserialization
  for (auto idx : c10::irange(buffer->executor_entry_lookup_keys()->size())) {
    executor_entry_lookup_.emplace(
        buffer->executor_entry_lookup_keys()->Get(idx),
        deserialize(buffer->executor_entry_lookup_values()->Get(idx)));
  }

  compiled_kernel_ = executor_utils::getCompiledKernel(
      buffer->compiled_kernel(), compile_params);

  NVF_ERROR(hasCompiledKernel(), "Failed to deserialize FusionExecutor");
}

FusionExecutor::ExecutorEntry FusionExecutor::deserialize(
    const serde::ExecutorEntry* buffer) {
  // See table definition for ExecutorEntry in serde/fusion_cache.fbs

  NVF_ERROR(buffer != nullptr, "serde::ExecutorEntry is nullptr.");

  ExecutorEntry entry;

  entry.init = buffer->init();

  entry.launch_params.deserialize(buffer->launch_params());

  for (auto output_buffer : *buffer->outputs()) {
    entry.outputs.push_back(deserialize(output_buffer));
  }

  for (auto intermediate_buffer : *buffer->intermediates()) {
    entry.intermediates.push_back(deserialize(intermediate_buffer));
  }

  return entry;
}

FusionExecutor::GlobalBufferInfo FusionExecutor::deserialize(
    const serde::GlobalBufferInfo* buffer) {
  // See table definition for GlobalBufferInfo in serde/fusion_cache.fbs

  NVF_ERROR(buffer != nullptr, "serde::GlobalBufferInfo is nullptr.");

  NVF_ERROR(
      buffer->tv() != -1, "Serialization failed to encode buffer tv position.");

  NVF_ERROR(lowered_ != nullptr, "Lowered kernel is not initialized.");

  GlobalBufferInfo info;
  if (buffer->is_fusion_output()) {
    auto out_val = kernel()->outputs().at(buffer->tv());
    NVF_ERROR(out_val != nullptr);
    info.tv = dynamic_cast<TensorView*>(out_val);
  } else {
    auto out_val = kernel()->summary().global_allocations.at(buffer->tv());
    NVF_ERROR(out_val != nullptr);
    info.tv = dynamic_cast<TensorView*>(out_val->buffer());
  }

  for (auto dim_size : *buffer->sizes()) {
    info.sizes.emplace_back(dim_size);
  }

  for (auto dim_stride : *buffer->strides()) {
    info.strides.emplace_back(dim_stride);
  }

  info.type = serde::mapToAtenDtype(buffer->dtype());
  info.zero_init = buffer->zero_init();
  info.resets_to_zero = buffer->resets_to_zero();
  info.is_profile_buffer = buffer->is_profile_buffer();
  return info;
}

} // namespace nvfuser
