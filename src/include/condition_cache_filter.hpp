#pragma once

#include "condition_cache_state.hpp"

#include "duckdb/function/scalar_function.hpp"
#include "duckdb/planner/expression.hpp"

namespace duckdb {

// Bind data containing the cached bitvectors
struct ConditionCacheFilterBindData : public FunctionData {
	shared_ptr<ConditionCacheEntry> cache_entry;

	explicit ConditionCacheFilterBindData(shared_ptr<ConditionCacheEntry> entry);

	unique_ptr<FunctionData> Copy() const override;
	bool Equals(const FunctionData &other_p) const override;
};

// Bind callback (should never be called directly)
unique_ptr<FunctionData> ConditionCacheFilterBind(ClientContext &context, ScalarFunction &bound_function,
                                                  vector<unique_ptr<Expression>> &arguments);

// Per-thread local state (placeholder)
struct ConditionCacheFilterState : public FunctionLocalState {};

unique_ptr<FunctionLocalState> ConditionCacheFilterInit(ExpressionState &state,
                                                        const BoundFunctionExpression &expr,
                                                        FunctionData *bind_data);

// The filter function: for each vector, check the bitvector and return constant boolean
void ConditionCacheFilterFn(DataChunk &args, ExpressionState &state, Vector &result);

} // namespace duckdb
