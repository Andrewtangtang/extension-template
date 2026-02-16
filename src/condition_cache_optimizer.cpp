#include "condition_cache_optimizer.hpp"
#include "condition_cache_filter.hpp"
#include "condition_cache_state.hpp"

#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/common/enums/expression_type.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
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

	// Cache hit: move original filters to LogicalFilter above, inject cache filter on scan
	InjectCacheFilter(plan, std::move(entry));
}

void ConditionCacheOptimizer::InjectCacheFilter(unique_ptr<LogicalOperator> &plan,
                                                 shared_ptr<ConditionCacheEntry> entry) {
	auto &get = plan->Cast<LogicalGet>();

	// --- Step 1: Ensure filter-only columns are projected out of LogicalGet ---
	// Some columns may only be used for filtering (not in the query output).
	// We need them in the LogicalGet output so the LogicalFilter above can reference them.
	if (get.projection_ids.empty()) {
		for (idx_t i = 0; i < get.GetColumnIds().size(); i++) {
			get.projection_ids.push_back(i);
		}
	}

	for (auto &filter_entry : get.table_filters.filters) {
		idx_t col_idx = filter_entry.first;

		// Check if this column is in column_ids
		auto &col_ids = get.GetColumnIds();
		optional_idx col_pos;
		for (idx_t i = 0; i < col_ids.size(); i++) {
			if (col_ids[i].GetPrimaryIndex() == col_idx) {
				col_pos = i;
				break;
			}
		}
		if (!col_pos.IsValid()) {
			// Column not in scan at all — should not happen for pushed-down filters
			continue;
		}

		// Check if it's already in projection_ids
		bool in_projection = false;
		for (auto &pid : get.projection_ids) {
			if (pid == col_pos.GetIndex()) {
				in_projection = true;
				break;
			}
		}
		if (!in_projection) {
			// Add it to projection_ids so it appears in the LogicalGet output
			get.projection_ids.push_back(col_pos.GetIndex());
		}
	}

	// --- Step 2: Convert original table_filters into expressions for a LogicalFilter ---
	vector<unique_ptr<Expression>> filter_expressions;
	for (auto &filter_entry : get.table_filters.filters) {
		idx_t col_idx = filter_entry.first;

		// Find the output index (position in projection_ids)
		auto &col_ids = get.GetColumnIds();
		optional_idx output_idx;
		for (idx_t i = 0; i < col_ids.size(); i++) {
			if (col_ids[i].GetPrimaryIndex() == col_idx) {
				for (idx_t j = 0; j < get.projection_ids.size(); j++) {
					if (get.projection_ids[j] == i) {
						output_idx = j;
						break;
					}
				}
				break;
			}
		}

		if (!output_idx.IsValid()) {
			continue;
		}

		auto column_type = get.GetColumnType(ColumnIndex(col_idx));
		// GetColumnBindings() uses projection_ids values as binding column indices,
		// so the binding column index is projection_ids[output_idx], not output_idx itself
		idx_t binding_col_idx = get.projection_ids[output_idx.GetIndex()];
		ColumnBinding binding(get.table_index, binding_col_idx);
		auto column_ref = make_uniq<BoundColumnRefExpression>(std::move(column_type), binding);
		auto filter_expr = filter_entry.second->ToExpression(*column_ref);
		filter_expressions.push_back(std::move(filter_expr));
	}

	// --- Step 2: Clear original table_filters ---
	get.table_filters.filters.clear();

	// --- Step 3: Add ONLY the cache filter on ROW_ID ---
	ScalarFunction func("__condition_cache_filter", {LogicalType::BIGINT}, LogicalType::BOOLEAN,
	                    ConditionCacheFilterFn, ConditionCacheFilterBind, nullptr, nullptr,
	                    ConditionCacheFilterInit);

	auto bind_data = make_uniq<ConditionCacheFilterBindData>(std::move(entry));

	vector<unique_ptr<Expression>> children;
	children.push_back(make_uniq<BoundReferenceExpression>(LogicalType::BIGINT, 0));

	auto expr = make_uniq<BoundFunctionExpression>(LogicalType::BOOLEAN, func, std::move(children),
	                                               std::move(bind_data));

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

	// --- Step 4: Wrap LogicalGet with LogicalFilter containing original predicates ---
	if (!filter_expressions.empty()) {
		auto logical_filter = make_uniq<LogicalFilter>();
		logical_filter->expressions = std::move(filter_expressions);
		// Move the LogicalGet under the LogicalFilter
		logical_filter->children.push_back(std::move(plan));
		// Replace plan with the new LogicalFilter
		plan = std::move(logical_filter);
	}
}

} // namespace duckdb
