#include "vqe/kernels.hpp"
#if defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>
#elif defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif
namespace vqe {
namespace detail {
void less_scalar(const std::int64_t*, std::int64_t, std::uint8_t*, std::size_t);
void add_scalar(const double*, const double*, double*, std::size_t);
}
namespace {
#if defined(__x86_64__) || defined(__i386__)
bool avx2_available() { static const bool available = __builtin_cpu_supports("avx2"); return available; }
__attribute__((target("avx2")))
void less_avx2(const std::int64_t* in, std::int64_t c, std::uint8_t* out, std::size_t n) {
  const auto constant = _mm256_set1_epi64x(c); std::size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    const auto value = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(in + i));
    const auto mask = _mm256_movemask_pd(_mm256_castsi256_pd(_mm256_cmpgt_epi64(constant, value)));
    for (int lane = 0; lane < 4; ++lane) out[i + static_cast<std::size_t>(lane)] = static_cast<std::uint8_t>((mask >> lane) & 1);
  }
  detail::less_scalar(in + i, c, out + i, n - i);
}
__attribute__((target("avx2")))
void add_avx2(const double* a, const double* b, double* out, std::size_t n) {
  std::size_t i = 0;
  for (; i + 4 <= n; i += 4) _mm256_storeu_pd(out + i, _mm256_add_pd(_mm256_loadu_pd(a + i), _mm256_loadu_pd(b + i)));
  detail::add_scalar(a + i, b + i, out + i, n - i);
}
#endif
}
std::string_view kernel_backend(KernelMode mode) {
  if (mode == KernelMode::Scalar) return "scalar";
#if defined(__aarch64__) || defined(_M_ARM64)
  return "neon";
#elif defined(__x86_64__) || defined(__i386__)
  if (avx2_available()) return "avx2";
#endif
  return "scalar";
}
void less_i64_constant(const std::int64_t* in, std::int64_t c, std::uint8_t* out, std::size_t n, KernelMode mode) {
  if (mode == KernelMode::Scalar) { detail::less_scalar(in, c, out, n); return; }
#if defined(__aarch64__) || defined(_M_ARM64)
  const auto constant = vdupq_n_s64(c); std::size_t i = 0;
  for (; i + 2 <= n; i += 2) {
    const auto mask = vcltq_s64(vld1q_s64(in + i), constant);
    out[i] = vgetq_lane_u64(mask, 0) != 0; out[i + 1] = vgetq_lane_u64(mask, 1) != 0;
  }
  detail::less_scalar(in + i, c, out + i, n - i);
#elif defined(__x86_64__) || defined(__i386__)
  if (avx2_available()) less_avx2(in, c, out, n); else detail::less_scalar(in, c, out, n);
#else
  detail::less_scalar(in, c, out, n);
#endif
}
void add_f64(const double* a, const double* b, double* out, std::size_t n, KernelMode mode) {
  if (mode == KernelMode::Scalar) { detail::add_scalar(a, b, out, n); return; }
#if defined(__aarch64__) || defined(_M_ARM64)
  std::size_t i = 0;
  for (; i + 2 <= n; i += 2) vst1q_f64(out + i, vaddq_f64(vld1q_f64(a + i), vld1q_f64(b + i)));
  detail::add_scalar(a + i, b + i, out + i, n - i);
#elif defined(__x86_64__) || defined(__i386__)
  if (avx2_available()) add_avx2(a, b, out, n); else detail::add_scalar(a, b, out, n);
#else
  detail::add_scalar(a, b, out, n);
#endif
}
}
