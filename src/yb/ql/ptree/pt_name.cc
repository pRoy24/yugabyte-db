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
// Treenode definitions for all name nodes.
//--------------------------------------------------------------------------------------------------

#include "yb/ql/ptree/pt_name.h"
#include "yb/ql/ptree/sem_context.h"

namespace yb {
namespace ql {

//--------------------------------------------------------------------------------------------------

PTName::PTName(MemoryContext *memctx,
               YBLocation::SharedPtr loc,
               const MCSharedPtr<MCString>& name)
    : TreeNode(memctx, loc),
      name_(name) {
}

PTName::~PTName() {
}

CHECKED_STATUS PTName::SetupPrimaryKey(SemContext *sem_context) {
  PTColumnDefinition *column = sem_context->GetColumnDefinition(*name_);
  if (column == nullptr) {
    return sem_context->Error(this, "Column does not exist", ErrorCode::UNDEFINED_COLUMN);
  }
  if (column->is_primary_key()) {
    return sem_context->Error(this, ErrorCode::DUPLICATE_COLUMN);
  }

  // Add the analyzed column to table. For CREATE INDEX, need to check for proper datatype and set
  // column location because column definition is loaded from the indexed table definition actually.
  PTCreateTable *table = sem_context->current_create_table_stmt();
  if (table->opcode() == TreeNodeOpcode::kPTCreateIndex) {
    if (column->datatype() == nullptr) {
      return sem_context->Error(this, "Unsupported index datatype",
                                ErrorCode::SQL_STATEMENT_INVALID);
    }
    column->set_loc(*this);
  }
  RETURN_NOT_OK(table->AppendPrimaryColumn(sem_context, column));

  return Status::OK();
}

CHECKED_STATUS PTName::SetupHashAndPrimaryKey(SemContext *sem_context) {
  PTColumnDefinition *column = sem_context->GetColumnDefinition(*name_);
  if (column == nullptr) {
    return sem_context->Error(this, "Column does not exist", ErrorCode::UNDEFINED_COLUMN);
  }
  if (column->is_primary_key()) {
    return sem_context->Error(this, ErrorCode::DUPLICATE_COLUMN);
  }

  // Add the analyzed column to table. For CREATE INDEX, need to check for proper datatype and set
  // column location because column definition is loaded from the indexed table definition actually.
  PTCreateTable *table = sem_context->current_create_table_stmt();
  if (table->opcode() == TreeNodeOpcode::kPTCreateIndex) {
    if (column->datatype() == nullptr) {
      return sem_context->Error(this, "Unsupported index datatype",
                                ErrorCode::SQL_STATEMENT_INVALID);
    }
    column->set_loc(*this);
  }
  RETURN_NOT_OK(table->AppendHashColumn(sem_context, column));

  return Status::OK();
}

CHECKED_STATUS PTName::SetupCoveringIndexColumn(SemContext *sem_context) {
  PTColumnDefinition *column = sem_context->GetColumnDefinition(*name_);
  if (column == nullptr) {
    return sem_context->Error(this, "Column does not exist", ErrorCode::UNDEFINED_COLUMN);
  }
  if (column->is_primary_key()) {
    return sem_context->Error(this, "Column covered already", ErrorCode::INVALID_TABLE_DEFINITION);
  }
  if (column->is_static()) {
    return sem_context->Error(this, "Static column not supported as a covered index column",
                              ErrorCode::SQL_STATEMENT_INVALID);
  }

  // Add the analyzed covered index column to table. Need to check for proper datatype and set
  // column location because column definition is loaded from the indexed table definition actually.
  PTCreateTable *table = sem_context->current_create_table_stmt();
  DCHECK(table->opcode() == TreeNodeOpcode::kPTCreateIndex);
  if (column->datatype() == nullptr) {
    return sem_context->Error(this, "Unsupported index datatype", ErrorCode::SQL_STATEMENT_INVALID);
  }
  column->set_loc(*this);
  return table->AppendColumn(sem_context, column, true /* check_duplicate */);
}

//--------------------------------------------------------------------------------------------------

PTNameAll::PTNameAll(MemoryContext *memctx, YBLocation::SharedPtr loc)
    : PTName(memctx, loc, MCMakeShared<MCString>(memctx, "*")) {
}

PTNameAll::~PTNameAll() {
}

//--------------------------------------------------------------------------------------------------

PTQualifiedName::PTQualifiedName(MemoryContext *memctx,
                                 YBLocation::SharedPtr loc,
                                 const PTName::SharedPtr& ptname)
    : PTName(memctx, loc),
      ptnames_(memctx) {
  Append(ptname);
}

PTQualifiedName::PTQualifiedName(MemoryContext *memctx,
                                 YBLocation::SharedPtr loc,
                                 const MCSharedPtr<MCString>& name)
    : PTName(memctx, loc),
      ptnames_(memctx) {
  Append(PTName::MakeShared(memctx, loc, name));
}

PTQualifiedName::~PTQualifiedName() {
}

void PTQualifiedName::Append(const PTName::SharedPtr& ptname) {
  ptnames_.push_back(ptname);
}

void PTQualifiedName::Prepend(const PTName::SharedPtr& ptname) {
  ptnames_.push_front(ptname);
}

CHECKED_STATUS PTQualifiedName::Analyze(SemContext *sem_context) {
  // We don't support qualified name yet except for a keyspace.
  // Support only the names like: '<keyspace_name>.<table_name>'.
  if (ptnames_.size() >= 3) {
    return sem_context->Error(this, ErrorCode::FEATURE_NOT_SUPPORTED);
  }

  return Status::OK();
}

CHECKED_STATUS PTQualifiedName::AnalyzeName(SemContext *sem_context, const ObjectType object_type) {
  switch (object_type) {
    case OBJECT_SCHEMA:
      if (ptnames_.size() != 1) {
        return sem_context->Error(this, "Invalid keyspace name", ErrorCode::INVALID_ARGUMENTS);
      }
      if (ptnames_.front()->name() == common::kRedisKeyspaceName) {
        return sem_context->Error(this,
                                  strings::Substitute("$0 is a reserved keyspace name",
                                                      common::kRedisKeyspaceName).c_str(),
                                  ErrorCode::INVALID_ARGUMENTS);
      }
      return Status::OK();

    case OBJECT_TABLE: FALLTHROUGH_INTENDED;
    case OBJECT_TYPE:
      if (ptnames_.size() > 2) {
        return sem_context->Error(this, "Invalid table or type name",
                                  ErrorCode::SQL_STATEMENT_INVALID);
      }
      if (ptnames_.size() == 1) {
        const string current_keyspace = sem_context->CurrentKeyspace();
        if (current_keyspace.empty()) {
          return sem_context->Error(this, ErrorCode::NO_NAMESPACE_USED);
        }
        MemoryContext* memctx = sem_context->PSemMem();
        Prepend(PTName::MakeShared(memctx, loc_,
                                   MCMakeShared<MCString>(memctx, current_keyspace.c_str())));
      }
      if (ptnames_.front()->name() == common::kRedisKeyspaceName) {
        return sem_context->Error(this,
                                  strings::Substitute("$0 is a reserved keyspace name",
                                                      common::kRedisKeyspaceName).c_str(),
                                  ErrorCode::INVALID_ARGUMENTS);
      }
      return Status::OK();

    case OBJECT_AGGREGATE: FALLTHROUGH_INTENDED;
    case OBJECT_AMOP: FALLTHROUGH_INTENDED;
    case OBJECT_AMPROC: FALLTHROUGH_INTENDED;
    case OBJECT_ATTRIBUTE: FALLTHROUGH_INTENDED;
    case OBJECT_CAST: FALLTHROUGH_INTENDED;
    case OBJECT_COLUMN: FALLTHROUGH_INTENDED;
    case OBJECT_COLLATION: FALLTHROUGH_INTENDED;
    case OBJECT_CONVERSION: FALLTHROUGH_INTENDED;
    case OBJECT_DATABASE: FALLTHROUGH_INTENDED;
    case OBJECT_DEFAULT: FALLTHROUGH_INTENDED;
    case OBJECT_DEFACL: FALLTHROUGH_INTENDED;
    case OBJECT_DOMAIN: FALLTHROUGH_INTENDED;
    case OBJECT_DOMCONSTRAINT: FALLTHROUGH_INTENDED;
    case OBJECT_EVENT_TRIGGER: FALLTHROUGH_INTENDED;
    case OBJECT_EXTENSION: FALLTHROUGH_INTENDED;
    case OBJECT_FDW: FALLTHROUGH_INTENDED;
    case OBJECT_FOREIGN_SERVER: FALLTHROUGH_INTENDED;
    case OBJECT_FOREIGN_TABLE: FALLTHROUGH_INTENDED;
    case OBJECT_FUNCTION: FALLTHROUGH_INTENDED;
    case OBJECT_INDEX: FALLTHROUGH_INTENDED;
    case OBJECT_LANGUAGE: FALLTHROUGH_INTENDED;
    case OBJECT_LARGEOBJECT: FALLTHROUGH_INTENDED;
    case OBJECT_MATVIEW: FALLTHROUGH_INTENDED;
    case OBJECT_OPCLASS: FALLTHROUGH_INTENDED;
    case OBJECT_OPERATOR: FALLTHROUGH_INTENDED;
    case OBJECT_OPFAMILY: FALLTHROUGH_INTENDED;
    case OBJECT_POLICY: FALLTHROUGH_INTENDED;
    case OBJECT_ROLE: FALLTHROUGH_INTENDED;
    case OBJECT_RULE: FALLTHROUGH_INTENDED;
    case OBJECT_SEQUENCE: FALLTHROUGH_INTENDED;
    case OBJECT_TABCONSTRAINT: FALLTHROUGH_INTENDED;
    case OBJECT_TABLESPACE: FALLTHROUGH_INTENDED;
    case OBJECT_TRANSFORM: FALLTHROUGH_INTENDED;
    case OBJECT_TRIGGER: FALLTHROUGH_INTENDED;
    case OBJECT_TSCONFIGURATION: FALLTHROUGH_INTENDED;
    case OBJECT_TSDICTIONARY: FALLTHROUGH_INTENDED;
    case OBJECT_TSPARSER: FALLTHROUGH_INTENDED;
    case OBJECT_TSTEMPLATE: FALLTHROUGH_INTENDED;
    case OBJECT_USER_MAPPING: FALLTHROUGH_INTENDED;
    case OBJECT_VIEW:
      return sem_context->Error(this, ErrorCode::FEATURE_NOT_SUPPORTED);
  }
  return Status::OK();
}

}  // namespace ql
}  // namespace yb
