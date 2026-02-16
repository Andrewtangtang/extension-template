#include "condition_cache_optimizer.hpp"
#include "condition_cache_filter.hpp"
#include "condition_cache_state.hpp"

#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/common/enums/expression_type.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"
#include "duckdb/planner/table_filter.hpp"

namespace duckdb {

ConditionCacheOptimizer::ConditionCacheOptimizer() {
	optimize_function = OptimizeFunction;
}

void ConditionCacheOptimizer::OptimizeFunction(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan) {
	WalkPlan(input.context, plan);
}

string ConditionCacheOptimizer::ComputeFilterKey(const string &table_name, const LogicalGet &get) {
	// Build a canonical string from the table_filters
	// Format: "table_name|col_name:filter_string;col_name:filter_string;..."
	string result = table_name + "|";
	bool first = true;
	for (auto &entry : get.table_filters.filters) {
		if (!first) {
			result += ";";
		}
		first = false;
		auto col_idx = entry.first;
		string col_name;
		if (col_idx < get.names.size()) {
			col_name = get.names[col_idx];
		} else {
			col_name = StringUtil::Format("col%d", col_idx);
		}
		result += col_name + ":" + entry.second->ToString(col_name);
	}
	return result;
}

void ConditionCacheOptimizer::WalkPlan(ClientContext &context, unique_ptr<LogicalOperator> &plan) {
	// Recurse into children first
	for (auto &child : plan->children) {
		WalkPlan(context, child);
	}

	if (plan->type != LogicalOperatorType::LOGICAL_GET) {
		return;
	}

	auto &get = plan->Cast<LogicalGet>();
	auto table = get.GetTable();
	if (!table) {
		return;
	}

	// Skip if there are no table filters
	if (get.table_filters.filters.empty()) {
		return;
	}

	// Compute the canonical filter key
	string filter_key = ComputeFilterKey(table->name, get);

	// Check if we're in build-capture mode (condition_cache_build is running)
	if (tl_build_capture.active && table->name == tl_build_capture.target_table) {
		tl_build_capture.captured_filter_key = filter_key;
		// Don't inject the cache filter during build
		return;
	}

	// Look up the cache
	auto store = ConditionCacheStore::GetOrCreate(context);
	auto entry = store->Lookup(filter_key);
	if (!entry) {
		return;
	}

	// Cache hit: inject the ExpressionFilter
	InjectCacheFilter(get, std::move(entry));
}

void ConditionCacheOptimizer::InjectCacheFilter(LogicalGet &get, shared_ptr<ConditionCacheEntry> entry) {
	// Build the scalar function (following the RowIdFilter demo pattern)
	ScalarFunction func("__condition_cache_filter", {LogicalType::BIGINT}, LogicalType::BOOLEAN,
	                    ConditionCacheFilterFn, ConditionCacheFilterBind, nullptr, nullptr,
	                    ConditionCacheFilterInit);

	// Create bind data with the cached bitvectors
	auto bind_data = make_uniq<ConditionCacheFilterBindData>(std::move(entry));

	// Construct the bound expression
	// Column index 0: the filter chunk contains only the filtered column (ROW_ID)
	vector<unique_ptr<Expression>> children;
	children.push_back(make_uniq<BoundReferenceExpression>(LogicalType::BIGINT, 0));

	auto expr = make_uniq<BoundFunctionExpression>(LogicalType::BOOLEAN, func, std::move(children),
	                                               std::move(bind_data));

	// Push the filter on the ROW_ID column
	get.table_filters.PushFilter(ColumnIndex(COLUMN_IDENTIFIER_ROW_ID),
	                             make_uniq<ExpressionFilter>(std::move(expr)));

	// Ensure ROW_ID is in the scan's column list
	bool has_rowid = false;
	for (auto &col : get.GetColumnIds()) {
		if (col.IsRowIdColumn()) {
			has_rowid = true;
			break;
		}
	}
	if (!has_rowid) {
		if (get.projection_ids.empty()) {
			for (idx_t i = 0; i < get.GetColumnIds().size(); i++) {
				get.projection_ids.push_back(i);
			}
		}
		get.AddColumnId(COLUMN_IDENTIFIER_ROW_ID);
	}
}

} // namespace duckdb
