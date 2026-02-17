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

	// Compute expression key from a LogicalFilter expression (for non-pushable predicates like LIKE)
	static string ComputeExpressionKey(const string &table_name, const Expression &expr);

	// Move original filters to LogicalFilter above, inject cache filter on scan (for pushable predicates)
	static void InjectCacheFilter(unique_ptr<LogicalOperator> &plan, shared_ptr<ConditionCacheEntry> entry);

	// Inject cache filter on a LogicalGet without moving filters (for non-pushable predicates)
	// The LogicalFilter already exists above, so we just add the cache filter to the scan
	static void InjectCacheFilterSimple(unique_ptr<LogicalOperator> &get_plan, shared_ptr<ConditionCacheEntry> entry);
};

} // namespace duckdb
