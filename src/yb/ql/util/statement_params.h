//--------------------------------------------------------------------------------------------------
// Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//
//
// Parameters for executing a SQL statement.
//--------------------------------------------------------------------------------------------------

#ifndef YB_QL_UTIL_STATEMENT_PARAMS_H_
#define YB_QL_UTIL_STATEMENT_PARAMS_H_

#include <boost/thread/shared_mutex.hpp>

#include "yb/common/ql_protocol.pb.h"
#include "yb/common/ql_value.h"

namespace yb {
namespace ql {

// This class represents the parameters for executing a SQL statement.
class StatementParameters {
 public:
  // Public types.
  typedef std::unique_ptr<StatementParameters> UniPtr;
  typedef std::unique_ptr<const StatementParameters> UniPtrConst;

  // Constructors
  StatementParameters();
  StatementParameters(const StatementParameters& other);
  virtual ~StatementParameters();

  // Accessor functions for page_size.
  uint64_t page_size() const { return page_size_; }
  void set_page_size(const uint64_t page_size) { page_size_ = page_size; }

  // Set paging state.
  CHECKED_STATUS set_paging_state(const std::string& paging_state) {
    // For performance, create QLPagingStatePB by demand only when setting paging state because
    // only SELECT statements continuing from a previous page carry a paging state.
    if (paging_state_ == nullptr) {
      paging_state_.reset(new QLPagingStatePB());
    }
    return paging_state_->ParseFromString(paging_state) ?
        Status::OK() : STATUS(Corruption, "invalid paging state");
  }

  // Accessor functions for paging state fields.
  const std::string& table_id() const { return paging_state().table_id(); }

  const std::string& next_partition_key() const { return paging_state().next_partition_key(); }

  const std::string& next_row_key() const { return paging_state().next_row_key(); }

  int64_t total_num_rows_read() const { return paging_state().total_num_rows_read(); }

  int64_t next_partition_index() const { return paging_state().next_partition_index(); }

  // Retrieve a bind variable for the execution of the statement. To be overridden by subclasses
  // to return actual bind variables.
  virtual CHECKED_STATUS GetBindVariable(const std::string& name,
                                         int64_t pos,
                                         const std::shared_ptr<QLType>& type,
                                         QLValue* value) const {
    return STATUS(RuntimeError, "no bind variable available");
  }

  const YBConsistencyLevel yb_consistency_level() const {
    return yb_consistency_level_;
  }

 protected:
  void set_yb_consistency_level(const YBConsistencyLevel yb_consistency_level) {
    yb_consistency_level_ = yb_consistency_level;
  }

 private:
  const QLPagingStatePB& paging_state() const {
    return paging_state_ != nullptr ? *paging_state_ : QLPagingStatePB::default_instance();
  }

  // Limit of the number of rows to return set as page size.
  uint64_t page_size_;

  // Paging State.
  std::unique_ptr<QLPagingStatePB> paging_state_;

  // Consistency level for YB.
  YBConsistencyLevel yb_consistency_level_;
};

} // namespace ql
} // namespace yb

#endif  // YB_QL_UTIL_STATEMENT_PARAMS_H_
