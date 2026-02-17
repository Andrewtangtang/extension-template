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

string ConditionCacheOptimizer::ComputeExpressionKey(const string &table_name, const Expression &expr) {
	// Use the bound expression's ToString() as a canonical key
	// Format: "table_name|expr:expression_string"
	return table_name + "|expr:" + expr.ToString();
}

void ConditionCacheOptimizer::WalkPlan(ClientContext &context, unique_ptr<LogicalOperator> &plan) {
	// Recurse into children first
	for (auto &child : plan->children) {
		WalkPlan(context, child);
	}

	// Path 1: LogicalGet with table_filters (handles pushable predicates)
	if (plan->type == LogicalOperatorType::LOGICAL_GET) {
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
		return;
	}

	// Path 2: LogicalFilter → LogicalGet (handles non-pushable predicates like LIKE)
	if (plan->type == LogicalOperatorType::LOGICAL_FILTER && !plan->children.empty() &&
	    plan->children[0]->type == LogicalOperatorType::LOGICAL_GET) {

		auto &filter = plan->Cast<LogicalFilter>();
		auto &get = plan->children[0]->Cast<LogicalGet>();
		auto table = get.GetTable();
		if (!table) {
			return;
		}

		for (auto &expr : filter.expressions) {
			string expr_key = ComputeExpressionKey(table->name, *expr);

			if (tl_build_capture.active && table->name == tl_build_capture.target_table) {
				// During build: capture the expression key for storage
				tl_build_capture.captured_filter_key = expr_key;
				return;
			}

			// During normal query: look up cache by expression key
			auto store = ConditionCacheStore::GetOrCreate(context);
			auto entry = store->Lookup(expr_key);
			if (entry) {
				// Cache hit: inject cache filter on the LogicalGet
				// LogicalFilter stays above for correctness (evaluates the actual predicate)
				InjectCacheFilterSimple(plan->children[0], std::move(entry));
				return;
			}
		}
	}
}

void ConditionCacheOptimizer::InjectCacheFilter(unique_ptr<LogicalOperator> &plan,
                                                 shared_ptr<ConditionCacheEntry> entry) {
	auto &get = plan->Cast<LogicalGet>();

	// --- Step 1: Identify which filters to move vs keep ---
	// ExpressionFilter (LIKE, JSON, etc.) → move to LogicalFilter above (expensive, zone map can't help)
	// ConstantFilter, IS_NULL, IN_FILTER, etc. → keep in table_filters (zone map friendly)
	vector<idx_t> cols_to_move;
	for (auto &filter_entry : get.table_filters.filters) {
		if (filter_entry.second->filter_type == TableFilterType::EXPRESSION_FILTER) {
			cols_to_move.push_back(filter_entry.first);
		}
	}

	// If no ExpressionFilters to move, we can use InjectCacheFilterSimple instead
	// (just add cache filter alongside existing filters)
	if (cols_to_move.empty()) {
		InjectCacheFilterSimple(plan, std::move(entry));
		return;
	}

	// --- Step 2: Ensure moved-filter columns are projected out of LogicalGet ---
	// Only columns whose filters are being moved to LogicalFilter need to be in projection.
	if (get.projection_ids.empty()) {
		for (idx_t i = 0; i < get.GetColumnIds().size(); i++) {
			get.projection_ids.push_back(i);
		}
	}

	for (auto col_idx : cols_to_move) {
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
			get.projection_ids.push_back(col_pos.GetIndex());
		}
	}

	// --- Step 3: Convert ONLY ExpressionFilter entries into expressions for LogicalFilter ---
	vector<unique_ptr<Expression>> filter_expressions;
	for (auto col_idx : cols_to_move) {
		auto it = get.table_filters.filters.find(col_idx);
		if (it == get.table_filters.filters.end()) {
			continue;
		}

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
		idx_t binding_col_idx = get.projection_ids[output_idx.GetIndex()];
		ColumnBinding binding(get.table_index, binding_col_idx);
		auto column_ref = make_uniq<BoundColumnRefExpression>(std::move(column_type), binding);
		auto filter_expr = it->second->ToExpression(*column_ref);
		filter_expressions.push_back(std::move(filter_expr));
	}

	// --- Step 4: Remove ONLY ExpressionFilter entries from table_filters ---
	// ConstantFilter, IS_NULL, IN_FILTER etc. stay in table_filters for zone map
	for (auto col_idx : cols_to_move) {
		get.table_filters.filters.erase(col_idx);
	}

	// --- Step 5: Add the cache filter on ROW_ID ---
	ScalarFunction func("__condition_cache_filter", {LogicalType::BIGINT}, LogicalType::BOOLEAN,
	                    ConditionCacheFilterFn, ConditionCacheFilterBind, nullptr, nullptr,
	                    ConditionCacheFilterInit);

	auto bind_data = make_uniq<ConditionCacheFilterBindData>(entry);

	vector<unique_ptr<Expression>> children;
	children.push_back(make_uniq<BoundReferenceExpression>(LogicalType::BIGINT, 0));

	auto func_expr = make_uniq<BoundFunctionExpression>(LogicalType::BOOLEAN, func, std::move(children),
	                                                    std::move(bind_data));

	// Use CacheExpressionFilter (subclass) to enable row-group level pruning via CheckStatistics
	get.table_filters.PushFilter(ColumnIndex(COLUMN_IDENTIFIER_ROW_ID),
	                             make_uniq<CacheExpressionFilter>(std::move(func_expr), entry));

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

	// --- Step 6: Wrap LogicalGet with LogicalFilter containing moved predicates ---
	if (!filter_expressions.empty()) {
		auto logical_filter = make_uniq<LogicalFilter>();
		logical_filter->expressions = std::move(filter_expressions);
		// Move the LogicalGet under the LogicalFilter
		logical_filter->children.push_back(std::move(plan));
		// Replace plan with the new LogicalFilter
		plan = std::move(logical_filter);
	}
}

void ConditionCacheOptimizer::InjectCacheFilterSimple(unique_ptr<LogicalOperator> &get_plan,
                                                       shared_ptr<ConditionCacheEntry> entry) {
	auto &get = get_plan->Cast<LogicalGet>();

	// Add cache filter on ROW_ID — much simpler than InjectCacheFilter because
	// we don't need to move any filters. The LogicalFilter is already above.
	ScalarFunction func("__condition_cache_filter", {LogicalType::BIGINT}, LogicalType::BOOLEAN,
	                    ConditionCacheFilterFn, ConditionCacheFilterBind, nullptr, nullptr,
	                    ConditionCacheFilterInit);

	auto bind_data = make_uniq<ConditionCacheFilterBindData>(entry);

	vector<unique_ptr<Expression>> children;
	children.push_back(make_uniq<BoundReferenceExpression>(LogicalType::BIGINT, 0));

	auto func_expr = make_uniq<BoundFunctionExpression>(LogicalType::BOOLEAN, func, std::move(children),
	                                                    std::move(bind_data));

	// Use CacheExpressionFilter (subclass) to enable row-group level pruning via CheckStatistics
	get.table_filters.PushFilter(ColumnIndex(COLUMN_IDENTIFIER_ROW_ID),
	                             make_uniq<CacheExpressionFilter>(std::move(func_expr), entry));

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
