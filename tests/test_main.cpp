#include "vqe/vector.hpp"
#include "vqe/hash.hpp"
#include "vqe/kernels.hpp"
#include "vqe/expression.hpp"
#include "vqe/operators.hpp"
#include <bit>
#include <cmath>
#include <map>
#include <algorithm>
#include <functional>
#include <iostream>
#include <limits>
#include <random>
#include <sstream>

using namespace vqe;
namespace {
std::vector<std::pair<std::string, std::function<void()>>>& tests() {
  static std::vector<std::pair<std::string, std::function<void()>>> t; return t;
}
struct Register { Register(std::string n, std::function<void()> f) { tests().emplace_back(std::move(n), std::move(f)); } };
#define TEST(name) void name(); Register reg_##name(#name, name); void name()
#define CHECK(...) do { if (!(__VA_ARGS__)) throw std::runtime_error(std::string(__FILE__) + ":" + std::to_string(__LINE__) + " " #__VA_ARGS__); } while (false)
template<class F> void throws(F f) { bool caught = false; try { f(); } catch (const std::exception&) { caught = true; } CHECK(caught); }
Value I(std::int64_t x) { return x; }
std::vector<std::vector<Value>> rows(Operator& op) {
  std::vector<std::vector<Value>> result;
  for (const auto& b : collect(op)) for (std::size_t i = 0; i < b.size(); ++i) {
    std::vector<Value> row; for (const auto& c : b.columns) row.push_back(c.value(b.selection[i])); result.push_back(std::move(row));
  }
  return result;
}
std::shared_ptr<Table> table(const Schema& s, const std::vector<std::vector<Value>>& data, std::size_t group = 3) {
  auto t = std::make_shared<Table>(s); Batch b(s, group);
  for (const auto& r : data) {
    for (std::size_t i = 0; i < s.size(); ++i) b.columns[i].append_value(r.at(i));
    ++b.physical_size;
    if (b.physical_size == group) { b.finish(b.physical_size); t->append(b); b.reset(); }
  }
  b.finish(b.physical_size); t->append(b); return t;
}
TEST(validity_boundaries_and_reuse) {
  Validity v;
  for (auto n : {0U, 1U, 63U, 64U, 65U, 2048U}) {
    v.reset(n); CHECK(v.all_valid());
    for (std::size_t i = 0; i < n; ++i) v.set(i, i % 3 != 0);
    for (std::size_t i = 0; i < n; ++i) CHECK(v.valid(i) == (i % 3 != 0));
    v.reset(0); for (std::size_t i = 0; i < n; ++i) v.append(i % 2 == 0);
    CHECK(v.null_count() == n / 2);
  }
}
TEST(selection_and_batch_invariants) {
  Selection s; s.identity(17); CHECK(s[16] == 16); s.truncate(3); CHECK(s.size() == 3);
  s.clear(); s.push(31); s.push(2); CHECK(s[0] == 31 && s[1] == 2);
  throws([&] { s.push(65535); }); throws([&] { s.identity(65536); });
  throws([] { Batch b({}, 0); }); throws([] { Batch b({{1, Type::Int64, "a"}, {1, Type::Int64, "b"}}); });
  Batch b({{1, Type::Int64, "x"}}, 4); b.columns[0].append_value(I(7)); b.finish(1);
  b.selection.clear(); b.selection.push(2); throws([&] { b.validate(); });
}
TEST(strings_and_buffer_reuse) {
  Column c(Type::String); c.append_string("alpha"); c.append_null(); c.append_string(std::string("a\0b", 3)); c.append_string("");
  CHECK(c.string_at(0) == "alpha"); CHECK(is_null(c.value(1))); CHECK(c.string_at(2).size() == 3);
  Column d(Type::String); for (std::size_t i = 0; i < c.size(); ++i) d.append_from(c, i);
  CHECK(d.value(2) == c.value(2)); const auto bytes = c.allocated_bytes(); c.reset(); CHECK(c.allocated_bytes() == bytes);
}
TEST(hash_collisions_and_resizing) {
  HashIndex h;
  for (std::size_t i = 0; i < 500; ++i) CHECK(h.find_or_insert(7, [i](auto r) { return i == r; }, [i] { return i; }).second);
  CHECK(h.capacity() >= 1024); CHECK(h.size() == 500);
  for (std::size_t i = 0; i < 500; ++i) CHECK(h.find(7, [i](auto r) { return r == i; }) == i);
  CHECK(h.find(7, [](auto) { return false; }) == no_row);
}
}
int main() {
  int failures = 0;
  for (const auto& [name, test] : tests()) try { test(); std::cout << "PASS " << name << '\n'; }
    catch (const std::exception& e) { ++failures; std::cerr << "FAIL " << name << ": " << e.what() << '\n'; }
  std::cout << tests().size() << " tests, " << failures << " failures\n";
  return failures ? 1 : 0;
}
