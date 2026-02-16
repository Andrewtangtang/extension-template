#pragma once

#include "duckdb/optimizer/optimizer_extension.hpp"
#include "duckdb/planner/operator/logical_get.hpp"

namespace duckdb {

struct ConditionCacheEntry;

class ConditionCacheOptimizer : public OptimizerExtension {
public:
	ConditionCacheOptimizer();

	// Post-optimize callback: walks the plan, looks up cache, injects filters
	static void OptimizeFunction(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan);

private:
	// Recursively walk the logical plan tree
	static void WalkPlan(ClientContext &context, unique_ptr<LogicalOperator> &plan);

	// Compute a canonical filter key from a LogicalGet's table_filters
	static string ComputeFilterKey(const string &table_name, const LogicalGet &get);

	// Inject the cache-based ExpressionFilter on ROW_ID
	static void InjectCacheFilter(LogicalGet &get, shared_ptr<ConditionCacheEntry> entry);
};

} // namespace duckdb
