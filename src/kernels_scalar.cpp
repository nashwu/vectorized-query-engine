#include "vqe/kernels.hpp"
namespace vqe::detail {
// This translation unit is compiled with auto-vectorization disabled, providing
// a measured scalar baseline independent of compiler vectorization decisions.
void less_scalar(const std::int64_t* in, std::int64_t c, std::uint8_t* out, std::size_t n) {
  for (std::size_t i = 0; i < n; ++i) out[i] = in[i] < c;
}
void add_scalar(const double* a, const double* b, double* out, std::size_t n) {
  for (std::size_t i = 0; i < n; ++i) out[i] = a[i] + b[i];
}
}
