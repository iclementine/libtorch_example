#pragma once

#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>
#include "cuda.h"

#include "fmt/core.h"
#include "triton_jit/jit_utils.h"
#include "triton_jit/triton_kernel.h"

namespace triton_jit {

enum struct ArgType : int8_t {
  NON_CONSTEXPR = 0,
  SPECIALIZED = 1,
  CONSTEXPR = 2,
};

struct StaticSignature {
  int num_args;
  std::vector<ArgType> arg_type;
};

class TritonJITFunction {
 private:
  std::string file_path_;
  std::string function_name_;
  StaticSignature static_sig_;
  mutable std::unordered_map<std::string, TritonKernel> overloads_;

 public:
  template <typename... Args>
  void operator()(CUstream stream,
                  unsigned int grid_x,
                  unsigned int grid_y,
                  unsigned int grid_z,
                  unsigned int num_warps,
                  unsigned int num_stages,
                  Args... args) const {
    const int num_args = this->static_sig_.num_args;

    // since we need to take address of all the arguemnts to the kernel to launch a kernel
    // but data pointers are not the arguement of the function operator(), they are local variables
    // that are created in `arg_handle`, to take the addresses of them, we need to keep them alive
    // out of the function
    std::vector<void *> data_pointers;
    data_pointers.reserve(num_args);

    std::vector<void *> kernel_args;
    kernel_args.reserve(num_args);

    std::vector<std::string> signature;
    signature.reserve(num_args);

    int idx = 0;

    auto arg_handle = [&](const auto &item) {
      if constexpr (has_data_ptr<decltype(item)>::value) {
        void *p_item = item.data_ptr();
        data_pointers.push_back(p_item);
        kernel_args.push_back(&(data_pointers.back()));

        const char *dtype = to_triton_typename(item.scalar_type());
        const char *specialization = "";
        if (this->static_sig_.arg_type[idx] == ArgType::SPECIALIZED) {
          specialization = spec(reinterpret_cast<std::uintptr_t>(data_pointers.back()));
        }
        std::string sig_for_idx = fmt::format("*{}{}", dtype, specialization);
        signature.push_back(sig_for_idx);
      } else if constexpr (std::is_same_v<decltype(item), std::nullopt_t>) {
        signature.push_back("*i8");
      } else if (this->static_sig_.arg_type[idx] == ArgType::CONSTEXPR) {  // constexpr
        signature.push_back(fmt::format("{}", item));
      } else if (this->static_sig_.arg_type[idx] == ArgType::SPECIALIZED) {  // specialzied
        const char *dtype = triton_type<decltype(item)>::name;
        if constexpr (std::is_integral_v<std::remove_cv_t<std::remove_reference_t<decltype(item)>>>) {
          const char *specialization = spec(item);
          if (specialization != ":1") {
            const void *p_item = &item;
            // cuLaunchKernel requires `void*`, so if the argument is const,
            // we need to const_cast to remove the const qualifier to call it
            kernel_args.push_back(const_cast<void *>(p_item));
          }
          std::string sig_for_idx = fmt::format("{}{}", dtype, specialization);
          signature.push_back(sig_for_idx);
        } else {
          const void *p_item = &item;
          kernel_args.push_back(const_cast<void *>(p_item));
          std::string sig_for_idx = fmt::format("{}", dtype);
          signature.push_back(sig_for_idx);
        }
      } else {  // ArgType::NON_SPECIALIZED
        const void *p_item = &item;
        kernel_args.push_back(const_cast<void *>(p_item));
        const char *dtype = triton_type<decltype(item)>::name;
        signature.push_back(dtype);
      }
      idx++;
    };
    (arg_handle(args), ...);
    // global scratch: introduced in triton 3.3
    void *global_scratch = nullptr;
    data_pointers.push_back(global_scratch);
    kernel_args.push_back(&(data_pointers.back()));

    std::string full_signature;
    for (int i = 0; i < signature.size(); i++) {
      if (i == 0) {
        full_signature += signature[i];
      } else {
        full_signature += ",";
        full_signature += signature[i];
      }
    }

    // TODO: device or compute capability to compile
    CUcontext ctx;
    checkCudaErrors(cuStreamGetCtx(stream, &ctx));
    checkCudaErrors(cuCtxSetCurrent(ctx));  // redundant?
    CUdevice d;
    // device management is done with torch, assume one CUcontext per device                         // int
    checkCudaErrors(cuCtxGetDevice(&d));

    const TritonKernel &kernel = this->get_kernel(full_signature, num_warps, num_stages, d);
    kernel.launch(grid_x, grid_y, grid_z, num_warps, stream, kernel_args.data());
    return;
  }

 private:
  const TritonKernel &get_kernel(std::string_view signature,
                                 int num_warps,
                                 int num_stages,
                                 CUdevice device_index) const;

 public:
  static TritonJITFunction &getInstance(std::string_view path, std::string_view name);

 private:
  static std::unordered_map<std::string, TritonJITFunction> functions_;
  TritonJITFunction(std::string_view path, std::string_view name);
};

}  // namespace triton_jit
