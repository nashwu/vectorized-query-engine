#pragma once
#include "vqe/vector.hpp"
#include <limits>
#include <utility>

namespace vqe {
constexpr std::size_t no_row = std::numeric_limits<std::size_t>::max();
std::uint64_t mix_hash(std::uint64_t);
std::uint64_t hash_cell(const Column&, std::size_t);
bool equal_cell(const Column&, std::size_t, const Column&, std::size_t);
// Total ordering for sorting/statistics: NULL last, NaN after finite/infinite
// doubles. Equality expressions retain IEEE semantics (NaN != NaN).
int compare_cell(const Column&, std::size_t, const Column&, std::size_t);
std::uint64_t hash_row(const std::vector<Column>&, std::span<const std::size_t>, std::size_t);
bool joinable_row(const std::vector<Column>&, std::span<const std::size_t>, std::size_t);

// Buckets hold hash+row ID (16 bytes). Payload lives in separate contiguous
// arrays. At <=70% occupancy linear probing always encounters an empty bucket.
class HashIndex {
 public:
  struct Bucket { std::uint64_t hash = 0; std::size_t row = no_row; };
  explicit HashIndex(std::size_t initial_capacity = 16);
  template<class Equal> std::size_t find(std::uint64_t hash, Equal equal) const {
    auto i = static_cast<std::size_t>(hash) & (buckets_.size() - 1);
    while (buckets_[i].row != no_row) {
      if (buckets_[i].hash == hash && equal(buckets_[i].row)) return buckets_[i].row;
      i = (i + 1) & (buckets_.size() - 1);
    }
    return no_row;
  }
  template<class Equal, class Create> std::pair<std::size_t, bool> find_or_insert(std::uint64_t hash, Equal equal, Create create) {
    if ((size_ + 1) * 10 > buckets_.size() * 7) grow();
    auto i = static_cast<std::size_t>(hash) & (buckets_.size() - 1);
    while (buckets_[i].row != no_row) {
      if (buckets_[i].hash == hash && equal(buckets_[i].row)) return {buckets_[i].row, false};
      i = (i + 1) & (buckets_.size() - 1);
    }
    const auto row = create(); buckets_[i] = {hash, row}; ++size_; return {row, true};
  }
  std::size_t size() const { return size_; }
  std::size_t capacity() const { return buckets_.size(); }
  std::size_t allocated_bytes() const { return buckets_.capacity() * sizeof(Bucket); }
 private:
  void grow();
  std::vector<Bucket> buckets_;
  std::size_t size_ = 0;
};

// Unbounded columnar payload for blocking operators. Unlike Batch it is never
// exposed to execution selections and can hold more than 65535 rows.
class RowStore {
 public:
  explicit RowStore(const Schema&);
  void append(const Batch&, std::size_t row);
  void append_key(const Batch&, std::span<const std::size_t> indices, std::size_t row);
  bool equal_key(std::size_t stored_row, const Batch&, std::span<const std::size_t> input_indices,
                 std::span<const std::size_t> stored_indices, std::size_t input_row) const;
  std::size_t size() const { return rows_; }
  std::size_t allocated_bytes() const;
  std::vector<Column> columns;
 private:
  std::size_t rows_ = 0;
};
}
