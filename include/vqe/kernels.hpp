#pragma once
#include <cstddef>
#include <cstdint>
#include <string_view>
namespace vqe {
enum class KernelMode { Scalar, Auto };
std::string_view kernel_backend(KernelMode);
void less_i64_constant(const std::int64_t* input, std::int64_t constant,
                       std::uint8_t* output, std::size_t size, KernelMode);
void add_f64(const double* left, const double* right, double* output,
             std::size_t size, KernelMode);
}
