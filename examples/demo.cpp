#include "vqe/workloads.hpp"
#include <chrono>
#include <iostream>

int main(int argc, char** argv) {
  try {
    const std::string query = argc > 1 ? argv[1] : "q6";
    const auto count = argc > 2 ? std::stoull(argv[2]) : 100000;
    const auto data = vqe::workloads::generate(count);
    vqe::Plan plan;
    if (query == "q1") plan = vqe::workloads::q1(data);
    else if (query == "q3") plan = vqe::workloads::q3(data);
    else if (query == "q6") plan = vqe::workloads::q6(data);
    else throw std::invalid_argument("usage: vqe_demo [q1|q3|q6] [lineitem_rows]");
    const auto physical = vqe::lower(vqe::optimize(plan));
    std::cout << vqe::explain(*physical) << "backend=" << vqe::kernel_backend(vqe::KernelMode::Auto) << '\n';
    const auto start = std::chrono::steady_clock::now(); auto op = vqe::execute(*physical); auto batches = vqe::collect(*op);
    const auto milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    for (const auto& f : op->schema()) std::cout << f.name << '\t'; std::cout << '\n';
    for (const auto& b : batches) for (std::size_t i = 0; i < b.size(); ++i) {
      for (const auto& c : b.columns) std::cout << vqe::to_string(c.value(b.selection[i])) << '\t'; std::cout << '\n';
    }
    std::cout << "execution_ms=" << milliseconds << " retained_operator_buffer_bytes=" << op->allocated_bytes() << '\n';
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
