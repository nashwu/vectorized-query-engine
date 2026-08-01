#pragma once
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace vqe {
constexpr std::size_t default_batch_size = 2048;
constexpr std::size_t max_batch_size = 65535;
using ColumnId = std::uint32_t;
enum class Type { Boolean, Int64, Double, String };
using Value = std::variant<std::monostate, bool, std::int64_t, double, std::string>;
struct Field { ColumnId id; Type type; std::string name; };
using Schema = std::vector<Field>;
std::size_t column_index(const Schema&, ColumnId);
void validate_schema(const Schema&);
bool is_null(const Value&);
std::string to_string(const Value&);
Type value_type(const Value&);

class Validity {
 public:
  void reset(std::size_t size, bool valid = true);
  bool valid(std::size_t row) const { assert(row < size_); return (bits_[row / 64] >> (row % 64)) & 1U; }
  void set(std::size_t row, bool valid);
  void append(bool valid);
  std::size_t size() const { return size_; }
  bool all_valid() const { return null_count_ == 0; }
  std::size_t null_count() const { return null_count_; }
  std::size_t allocated_bytes() const { return bits_.capacity() * sizeof(std::uint64_t); }
 private:
  std::vector<std::uint64_t> bits_;
  std::size_t size_ = 0, null_count_ = 0;
};

// Identity selections occupy no index storage; sparse indices are physical rows.
class Selection {
 public:
  void identity(std::size_t size);
  void clear() { dense_ = false; size_ = 0; indices_.clear(); }
  void push(std::size_t index);
  void truncate(std::size_t size);
  std::size_t operator[](std::size_t i) const { assert(i < size_); return dense_ ? i : indices_[i]; }
  std::size_t size() const { return size_; }
  bool dense() const { return dense_; }
  std::size_t allocated_bytes() const { return indices_.capacity() * sizeof(std::uint16_t); }
 private:
  bool dense_ = true;
  std::size_t size_ = 0;
  std::vector<std::uint16_t> indices_;
};

struct StringBuffer { std::vector<std::uint32_t> offsets{0}; std::vector<char> bytes; };
class Column {
 public:
  explicit Column(Type type = Type::Int64, std::size_t reserve = 0);
  Type type() const { return type_; }
  std::size_t size() const { return validity_.size(); }
  const Validity& validity() const { return validity_; }
  Validity& validity() { return validity_; }
  void reset(std::size_t size = 0);
  void reserve(std::size_t size);
  template<class T> std::vector<T>& data() { return std::get<std::vector<T>>(data_); }
  template<class T> const std::vector<T>& data() const { return std::get<std::vector<T>>(data_); }
  std::string_view string_at(std::size_t row) const;
  void append_string(std::string_view value, bool valid = true);
  template<class T> void append(T value, bool valid = true) {
    data<T>().push_back(value); validity_.append(valid);
  }
  void append_value(const Value&);
  void append_null();
  void append_from(const Column&, std::size_t row);
  Value value(std::size_t row) const;
  std::size_t allocated_bytes() const;
 private:
  Type type_;
  Validity validity_;
  std::variant<std::vector<std::uint8_t>, std::vector<std::int64_t>, std::vector<double>, StringBuffer> data_;
};

struct Batch {
  explicit Batch(const Schema& schema = {}, std::size_t capacity = default_batch_size);
  Schema schema;
  std::vector<Column> columns;
  Selection selection;
  std::size_t capacity;
  std::size_t physical_size = 0;
  std::size_t size() const { return selection.size(); }
  void reset();
  void finish(std::size_t rows);
  void append_row(const Batch&, std::size_t physical_row);
  void validate() const;
  std::size_t allocated_bytes() const;
};
} // namespace vqe
