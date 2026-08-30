#pragma once
#include "vqe/plan.hpp"
namespace vqe::workloads {
struct Data { std::shared_ptr<Table> lineitem, orders, customer; };
// Deterministic synthetic data, not TPC-H dbgen data. Dates are integer day IDs.
Data generate(std::size_t lineitems, std::size_t group_size = default_batch_size);
Plan q1(const Data&);
Plan q3(const Data&);
Plan q6(const Data&);
}
