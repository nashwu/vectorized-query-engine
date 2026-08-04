#include "vqe/hash.hpp"
#include <bit>
#include <cmath>

namespace vqe {
std::uint64_t mix_hash(std::uint64_t x) {
  x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ULL;
  x ^= x >> 27; x *= 0x94d049bb133111ebULL; return x ^ (x >> 31);
}
std::uint64_t hash_cell(const Column& c, std::size_t r) {
  if (!c.validity().valid(r)) return 0x9e3779b97f4a7c15ULL;
  switch (c.type()) {
    case Type::Boolean: return mix_hash(c.data<std::uint8_t>()[r]);
    case Type::Int64: return mix_hash(static_cast<std::uint64_t>(c.data<std::int64_t>()[r]));
    case Type::Double: {
      auto x = c.data<double>()[r]; if (x == 0) x = 0;
      return mix_hash(std::isnan(x) ? 0x7ff8000000000000ULL : std::bit_cast<std::uint64_t>(x));
    }
    case Type::String: {
      std::uint64_t h = 14695981039346656037ULL;
      for (char x : c.string_at(r)) { h ^= static_cast<unsigned char>(x); h *= 1099511628211ULL; }
      return mix_hash(h);
    }
  }
  return 0;
}
namespace {
template<class T> int cmp(T a, T b) { return a < b ? -1 : (a > b ? 1 : 0); }
}
int compare_cell(const Column& a, std::size_t ar, const Column& b, std::size_t br) {
  if (a.type() != b.type()) throw std::invalid_argument("comparison type mismatch");
  const bool av = a.validity().valid(ar), bv = b.validity().valid(br);
  if (!av || !bv) return av ? -1 : (bv ? 1 : 0);
  switch (a.type()) {
    case Type::Boolean: return cmp(a.data<std::uint8_t>()[ar], b.data<std::uint8_t>()[br]);
    case Type::Int64: return cmp(a.data<std::int64_t>()[ar], b.data<std::int64_t>()[br]);
    case Type::String: return cmp(a.string_at(ar), b.string_at(br));
    case Type::Double: {
      const auto x = a.data<double>()[ar], y = b.data<double>()[br];
      if (std::isnan(x) || std::isnan(y)) return std::isnan(x) ? (std::isnan(y) ? 0 : 1) : -1;
      return cmp(x, y);
    }
  }
  return 0;
}
bool equal_cell(const Column& a, std::size_t ar, const Column& b, std::size_t br) { return compare_cell(a, ar, b, br) == 0; }
std::uint64_t hash_row(const std::vector<Column>& cols, std::span<const std::size_t> keys, std::size_t row) {
  std::uint64_t h = 0x243f6a8885a308d3ULL;
  for (auto key : keys) h = mix_hash(h ^ hash_cell(cols[key], row));
  return h;
}
bool joinable_row(const std::vector<Column>& cols, std::span<const std::size_t> keys, std::size_t row) {
  for (auto key : keys) {
    const auto& c = cols[key]; if (!c.validity().valid(row)) return false;
    if (c.type() == Type::Double && std::isnan(c.data<double>()[row])) return false;
  }
  return true;
}
HashIndex::HashIndex(std::size_t n) : buckets_(std::bit_ceil(std::max(std::size_t{16}, n))) {}
void HashIndex::grow() {
  auto old = std::move(buckets_); buckets_.resize(old.size() * 2);
  for (const auto& b : old) if (b.row != no_row) {
    auto i = static_cast<std::size_t>(b.hash) & (buckets_.size() - 1);
    while (buckets_[i].row != no_row) i = (i + 1) & (buckets_.size() - 1);
    buckets_[i] = b;
  }
}
RowStore::RowStore(const Schema& s) { for (const auto& f : s) columns.emplace_back(f.type); }
void RowStore::append(const Batch& b, std::size_t r) {
  if (b.columns.size() != columns.size()) throw std::invalid_argument("store width mismatch");
  for (std::size_t c = 0; c < columns.size(); ++c) columns[c].append_from(b.columns[c], r);
  ++rows_;
}
void RowStore::append_key(const Batch& b, std::span<const std::size_t> keys, std::size_t r) {
  if (keys.size() != columns.size()) throw std::invalid_argument("key width mismatch");
  for (std::size_t c = 0; c < keys.size(); ++c) columns[c].append_from(b.columns[keys[c]], r);
  ++rows_;
}
bool RowStore::equal_key(std::size_t stored, const Batch& b, std::span<const std::size_t> input_keys,
                         std::span<const std::size_t> stored_keys, std::size_t input) const {
  for (std::size_t c = 0; c < input_keys.size(); ++c)
    if (!equal_cell(columns[stored_keys[c]], stored, b.columns[input_keys[c]], input)) return false;
  return true;
}
std::size_t RowStore::allocated_bytes() const { std::size_t n = 0; for (const auto& c : columns) n += c.allocated_bytes(); return n; }
}
