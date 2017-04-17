// Copyright (c) YugaByte, Inc.

#include "yb/docdb/doc_yql_scanspec.h"

namespace yb {
namespace docdb {

DocYQLScanSpec::DocYQLScanSpec(const Schema& schema, const DocKey& doc_key)
    : YQLScanSpec(nullptr),
      range_(nullptr),
      schema_(schema),
      hash_code_(0),
      hashed_components_(nullptr),
      doc_key_(&doc_key),
      start_doc_key_(DocKey()),
      lower_doc_key_(DocKey()),
      upper_doc_key_(DocKey()) {
  }


DocYQLScanSpec::DocYQLScanSpec(
    const Schema& schema, const uint32_t hash_code,
    const std::vector<PrimitiveValue>& hashed_components, const YQLConditionPB* condition,
    const DocKey& start_doc_key)
    : YQLScanSpec(condition),
      range_(condition != nullptr ? new common::YQLScanRange(schema, *condition) : nullptr),
      schema_(schema),
      hash_code_(hash_code),
      hashed_components_(&hashed_components),
      doc_key_(nullptr),
      start_doc_key_(start_doc_key),
      lower_doc_key_(bound_key(true)),
      upper_doc_key_(bound_key(false)) {
  // Initialize the upper and lower doc keys.
  CHECK(hashed_components_ != nullptr) << "hashed primary key columns missing";
}

DocKey DocYQLScanSpec::bound_key(const bool lower_bound) const {
  // If no hashed_component, start from the beginning.
  if (hashed_components_->empty()) {
    return DocKey();
  }
  std::vector<PrimitiveValue> range_components;
  if (range_.get() != nullptr) {
    const std::vector<YQLValuePB> range_values = range_->range_values(lower_bound);
    range_components.reserve(range_values.size());
    size_t column_idx = schema_.num_hash_key_columns();
    for (const auto& value : range_values) {
      const auto& column = schema_.column(column_idx);
      range_components.emplace_back(PrimitiveValue::FromYQLValuePB(
          column.type(), value, column.sorting_type()));
      column_idx++;
    }
  }
  return DocKey(hash_code_, *hashed_components_, range_components);
}

Status DocYQLScanSpec::GetBoundKey(const bool lower_bound, DocKey* key) const {
  // If a full doc key is specify, that is the exactly doc to scan. Otherwise, compute the
  // lower/upper bound doc keys to scan from the range.
  if (doc_key_ != nullptr) {
    *key = *doc_key_;
    return Status::OK();
  }
  // If start doc_key is set, that is the lower bound for the scan range.
  if (lower_bound && !start_doc_key_.empty()) {
    if (range_.get() != nullptr) {
      if (!lower_doc_key_.empty() && start_doc_key_ < lower_doc_key_ ||
          !upper_doc_key_.empty() && start_doc_key_ > upper_doc_key_) {
        return STATUS_SUBSTITUTE(Corruption,
                                 "Invalid start_doc_key: $0. Range: $1, $2",
                                 start_doc_key_.ToString(),
                                 lower_doc_key_.ToString(),
                                 upper_doc_key_.ToString());
      }
    }
    // TODO: Should we return in the case for no range component?
    *key = start_doc_key_;
    return Status::OK();
  }

  *key = lower_bound ? lower_doc_key_ : upper_doc_key_;
  return Status::OK();
}

}  // namespace docdb
}  // namespace yb