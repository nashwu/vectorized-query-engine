#include "vqe/kernels.hpp"
namespace vqe {
namespace detail {
void less_scalar(const std::int64_t*, std::int64_t, std::uint8_t*, std::size_t);
void add_scalar(const double*, const double*, double*, std::size_t);
}
std::string_view kernel_backend(KernelMode) { return "scalar"; }
void less_i64_constant(const std::int64_t* in, std::int64_t c, std::uint8_t* out, std::size_t n, KernelMode) {
  detail::less_scalar(in, c, out, n);
}
void add_f64(const double* a, const double* b, double* out, std::size_t n, KernelMode) {
  detail::add_scalar(a, b, out, n);
}
}
