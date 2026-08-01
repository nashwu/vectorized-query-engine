#include "vqe/vector.hpp"
#include <algorithm>
#include <limits>
#include <sstream>
#include <unordered_set>

namespace vqe {
std::size_t column_index(const Schema& s, ColumnId id) {
  for (std::size_t i = 0; i < s.size(); ++i) if (s[i].id == id) return i;
  throw std::invalid_argument("unknown column ID " + std::to_string(id));
}
void validate_schema(const Schema& s) {
  std::unordered_set<ColumnId> ids;
  for (const auto& f : s) if (!ids.insert(f.id).second) throw std::invalid_argument("duplicate column ID");
}
bool is_null(const Value& v) { return std::holds_alternative<std::monostate>(v); }
Type value_type(const Value& v) {
  switch (v.index()) {
    case 1: return Type::Boolean; case 2: return Type::Int64;
    case 3: return Type::Double; case 4: return Type::String;
    default: throw std::invalid_argument("NULL requires an explicit type");
  }
}
std::string to_string(const Value& v) {
  return std::visit([](const auto& x) -> std::string {
    using T = std::decay_t<decltype(x)>;
    if constexpr (std::is_same_v<T, std::monostate>) return "NULL";
    else if constexpr (std::is_same_v<T, std::string>) return x;
    else if constexpr (std::is_same_v<T, bool>) return x ? "true" : "false";
    else { std::ostringstream out; out.precision(17); out << x; return out.str(); }
  }, v);
}
void Validity::reset(std::size_t n, bool valid) {
  size_ = n; null_count_ = valid ? 0 : n;
  bits_.resize((n + 63) / 64);
  std::fill(bits_.begin(), bits_.end(), valid ? ~std::uint64_t{0} : 0);
}
void Validity::set(std::size_t row, bool v) {
  assert(row < size_);
  const bool old = valid(row);
  if (old != v) { if (v) --null_count_; else ++null_count_; }
  const auto bit = std::uint64_t{1} << (row % 64);
  if (v) bits_[row / 64] |= bit; else bits_[row / 64] &= ~bit;
}
void Validity::append(bool v) {
  if (size_ % 64 == 0) bits_.push_back(0);
  const auto row = size_++;
  ++null_count_; set(row, v);
}
void Selection::identity(std::size_t n) {
  if (n > max_batch_size) throw std::length_error("batch exceeds uint16 selection range");
  dense_ = true; size_ = n; indices_.clear();
}
void Selection::push(std::size_t i) {
  if (dense_) throw std::logic_error("clear selection before sparse append");
  if (i >= max_batch_size || size_ == max_batch_size) throw std::length_error("selection overflow");
  indices_.push_back(static_cast<std::uint16_t>(i)); ++size_;
}
void Selection::truncate(std::size_t n) {
  if (n > size_) throw std::out_of_range("selection truncate");
  size_ = n; if (!dense_) indices_.resize(n);
}
Column::Column(Type t, std::size_t n) : type_(t) {
  switch (t) {
    case Type::Boolean: data_ = std::vector<std::uint8_t>{}; break;
    case Type::Int64: data_ = std::vector<std::int64_t>{}; break;
    case Type::Double: data_ = std::vector<double>{}; break;
    case Type::String: data_ = StringBuffer{}; break;
  }
  reserve(n);
}
void Column::reserve(std::size_t n) {
  std::visit([n](auto& d) {
    using T = std::decay_t<decltype(d)>;
    if constexpr (std::is_same_v<T, StringBuffer>) d.offsets.reserve(n + 1);
    else d.reserve(n);
  }, data_);
}
void Column::reset(std::size_t n) {
  std::visit([n](auto& d) {
    using T = std::decay_t<decltype(d)>;
    if constexpr (std::is_same_v<T, StringBuffer>) { d.offsets.assign(n + 1, 0); d.bytes.clear(); }
    else { d.resize(n); std::fill(d.begin(), d.end(), typename T::value_type{}); }
  }, data_);
  validity_.reset(n);
}
std::string_view Column::string_at(std::size_t row) const {
  assert(row < size());
  const auto& s = std::get<StringBuffer>(data_);
  const auto n = s.offsets[row + 1] - s.offsets[row];
  return n ? std::string_view(s.bytes.data() + s.offsets[row], n) : std::string_view{};
}
void Column::append_string(std::string_view v, bool valid) {
  auto& s = std::get<StringBuffer>(data_);
  if (v.size() > std::numeric_limits<std::uint32_t>::max() - s.bytes.size()) throw std::length_error("string arena exceeds 4 GiB");
  s.bytes.insert(s.bytes.end(), v.begin(), v.end());
  s.offsets.push_back(static_cast<std::uint32_t>(s.bytes.size())); validity_.append(valid);
}
void Column::append_value(const Value& v) {
  if (is_null(v)) { append_null(); return; }
  if (value_type(v) != type_) throw std::invalid_argument("column type mismatch");
  switch (type_) {
    case Type::Boolean: append<std::uint8_t>(std::get<bool>(v)); break;
    case Type::Int64: append(std::get<std::int64_t>(v)); break;
    case Type::Double: append(std::get<double>(v)); break;
    case Type::String: append_string(std::get<std::string>(v)); break;
  }
}
void Column::append_null() {
  switch (type_) {
    case Type::Boolean: append<std::uint8_t>(0, false); break;
    case Type::Int64: append<std::int64_t>(0, false); break;
    case Type::Double: append<double>(0, false); break;
    case Type::String: append_string({}, false); break;
  }
}
void Column::append_from(const Column& c, std::size_t row) {
  if (c.type() != type_) throw std::invalid_argument("copy type mismatch");
  const bool valid = c.validity().valid(row);
  switch (type_) {
    case Type::Boolean: append(c.data<std::uint8_t>()[row], valid); break;
    case Type::Int64: append(c.data<std::int64_t>()[row], valid); break;
    case Type::Double: append(c.data<double>()[row], valid); break;
    case Type::String: append_string(c.string_at(row), valid); break;
  }
}
Value Column::value(std::size_t r) const {
  if (!validity_.valid(r)) return {};
  switch (type_) {
    case Type::Boolean: return data<std::uint8_t>()[r] != 0;
    case Type::Int64: return data<std::int64_t>()[r];
    case Type::Double: return data<double>()[r];
    case Type::String: return std::string(string_at(r));
  }
  throw std::logic_error("unknown type");
}
std::size_t Column::allocated_bytes() const {
  return validity_.allocated_bytes() + std::visit([](const auto& d) -> std::size_t {
    using T = std::decay_t<decltype(d)>;
    if constexpr (std::is_same_v<T, StringBuffer>) return d.offsets.capacity() * 4 + d.bytes.capacity();
    else return d.capacity() * sizeof(typename T::value_type);
  }, data_);
}
Batch::Batch(const Schema& s, std::size_t c) : schema(s), capacity(c) {
  if (c == 0 || c > max_batch_size) throw std::invalid_argument("batch capacity must be 1..65535");
  validate_schema(s);
  for (const auto& f : s) columns.emplace_back(f.type, c);
}
void Batch::reset() { for (auto& c : columns) c.reset(); physical_size = 0; selection.identity(0); }
void Batch::finish(std::size_t n) {
  if (n > capacity) throw std::length_error("batch capacity exceeded");
  physical_size = n; selection.identity(n);
  validate();
}
void Batch::append_row(const Batch& b, std::size_t row) {
  if (columns.size() != b.columns.size()) throw std::invalid_argument("batch width mismatch");
  if (physical_size == capacity) throw std::length_error("batch full");
  for (std::size_t c = 0; c < columns.size(); ++c) columns[c].append_from(b.columns[c], row);
  ++physical_size;
}
void Batch::validate() const {
  if (physical_size > capacity || columns.size() != schema.size()) throw std::logic_error("invalid batch shape");
  for (std::size_t i = 0; i < columns.size(); ++i)
    if (columns[i].size() != physical_size || columns[i].type() != schema[i].type) throw std::logic_error("invalid column shape");
  for (std::size_t i = 0; i < size(); ++i) if (selection[i] >= physical_size) throw std::logic_error("selection outside batch");
}
std::size_t Batch::allocated_bytes() const {
  auto n = selection.allocated_bytes(); for (const auto& c : columns) n += c.allocated_bytes(); return n;
}
} // namespace vqe
