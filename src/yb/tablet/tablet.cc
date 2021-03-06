// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.
//
// The following only applies to changes made to this file as part of YugaByte development.
//
// Portions Copyright (c) YugaByte, Inc.
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

// Portions Copyright (c) YugaByte, Inc.

#include "yb/tablet/tablet.h"

#include <algorithm>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <ostream>
#include <unordered_set>
#include <utility>
#include <vector>
#include <boost/optional.hpp>

#include "yb/rocksdb/db.h"
#include "yb/rocksdb/options.h"
#include "yb/rocksdb/statistics.h"
#include "yb/rocksdb/utilities/checkpoint.h"
#include "yb/rocksdb/write_batch.h"

#include "yb/cfile/cfile_writer.h"
#include "yb/common/common.pb.h"
#include "yb/common/hybrid_time.h"
#include "yb/common/iterator.h"
#include "yb/common/row_changelist.h"
#include "yb/common/row_operations.h"
#include "yb/common/scan_spec.h"
#include "yb/common/schema.h"
#include "yb/common/ql_rowblock.h"
#include "yb/consensus/consensus.pb.h"
#include "yb/consensus/log_anchor_registry.h"
#include "yb/consensus/opid_util.h"

#include "yb/docdb/conflict_resolution.h"
#include "yb/docdb/doc_rowwise_iterator.h"
#include "yb/docdb/docdb.h"
#include "yb/docdb/docdb.pb.h"
#include "yb/docdb/docdb_compaction_filter.h"
#include "yb/docdb/docdb_rocksdb_util.h"
#include "yb/docdb/intent.h"
#include "yb/docdb/primitive_value.h"
#include "yb/docdb/lock_batch.h"

#include "yb/gutil/atomicops.h"
#include "yb/gutil/map-util.h"
#include "yb/gutil/stl_util.h"
#include "yb/gutil/strings/numbers.h"
#include "yb/gutil/strings/substitute.h"
#include "yb/rocksutil/yb_rocksdb.h"
#include "yb/rocksutil/yb_rocksdb_logger.h"
#include "yb/server/hybrid_clock.h"

#include "yb/tablet/compaction.h"
#include "yb/tablet/compaction_policy.h"
#include "yb/tablet/delta_compaction.h"
#include "yb/tablet/diskrowset.h"
#include "yb/tablet/key_value_iterator.h"
#include "yb/tablet/maintenance_manager.h"
#include "yb/tablet/row_op.h"
#include "yb/tablet/rowset_info.h"
#include "yb/tablet/rowset_tree.h"
#include "yb/tablet/svg_dump.h"
#include "yb/tablet/tablet_metrics.h"
#include "yb/tablet/tablet_mm_ops.h"
#include "yb/tablet/tablet_retention_policy.h"
#include "yb/tablet/transaction_coordinator.h"
#include "yb/tablet/transaction_participant.h"
#include "yb/tablet/operations/alter_schema_operation.h"
#include "yb/tablet/operations/write_operation.h"
#include "yb/tablet/tablet_options.h"
#include "yb/util/bloom_filter.h"
#include "yb/util/debug/trace_event.h"
#include "yb/util/enums.h"
#include "yb/util/env.h"
#include "yb/util/flag_tags.h"
#include "yb/util/jsonwriter.h"
#include "yb/util/locks.h"
#include "yb/util/mem_tracker.h"
#include "yb/util/metrics.h"
#include "yb/util/slice.h"
#include "yb/util/stopwatch.h"
#include "yb/util/string_packer.h"
#include "yb/util/trace.h"
#include "yb/util/url-coding.h"

DEFINE_bool(tablet_do_dup_key_checks, true,
            "Whether to check primary keys for duplicate on insertion. "
            "Use at your own risk!");
TAG_FLAG(tablet_do_dup_key_checks, unsafe);

DEFINE_int32(tablet_compaction_budget_mb, 128,
             "Budget for a single compaction");
TAG_FLAG(tablet_compaction_budget_mb, experimental);

DEFINE_int32(tablet_bloom_block_size, 4096,
             "Block size of the bloom filters used for tablet keys.");
TAG_FLAG(tablet_bloom_block_size, advanced);

DEFINE_double(tablet_bloom_target_fp_rate, 0.01f,
              "Target false-positive rate (between 0 and 1) to size tablet key bloom filters. "
              "A lower false positive rate may reduce the number of disk seeks required "
              "in heavy insert workloads, at the expense of more space and RAM "
              "required for bloom filters.");
TAG_FLAG(tablet_bloom_target_fp_rate, advanced);

METRIC_DEFINE_entity(tablet);
METRIC_DEFINE_gauge_size(tablet, memrowset_size, "MemRowSet Memory Usage",
                         yb::MetricUnit::kBytes,
                         "Size of this tablet's memrowset");
METRIC_DEFINE_gauge_size(tablet, on_disk_size, "Tablet Size On Disk",
                         yb::MetricUnit::kBytes,
                         "Size of this tablet on disk.");

using namespace std::placeholders;

using std::shared_ptr;
using std::make_shared;
using std::string;
using std::unordered_set;
using std::vector;
using std::unique_ptr;

using rocksdb::WriteBatch;
using rocksdb::SequenceNumber;
using yb::util::ScopedPendingOperation;
using yb::tserver::WriteRequestPB;
using yb::tserver::WriteResponsePB;
using yb::docdb::KeyValueWriteBatchPB;
using yb::tserver::ReadRequestPB;
using yb::docdb::ValueType;
using yb::docdb::KeyBytes;
using yb::docdb::DocOperation;
using yb::docdb::RedisWriteOperation;
using yb::docdb::QLWriteOperation;
using yb::docdb::DocDBCompactionFilterFactory;
using yb::docdb::KuduWriteOperation;
using yb::docdb::IntentKind;
using yb::docdb::IntentTypePair;
using yb::docdb::KeyToIntentTypeMap;

// Make sure RocksDB does not disappear while we're using it. This is used at the top level of
// functions that perform RocksDB operations (directly or indirectly). Once a function is using
// this mechanism, any functions that it calls can safely use RocksDB as usual.
#define GUARD_AGAINST_ROCKSDB_SHUTDOWN \
  if (IsShutdownRequested()) { \
    return STATUS(IllegalState, "tablet is shutting down"); \
  } \
  ScopedPendingOperation shutdown_guard(&pending_op_counter_);

namespace yb {
namespace tablet {

using yb::MaintenanceManager;
using consensus::OpId;
using consensus::MaximumOpId;
using log::LogAnchorRegistry;
using strings::Substitute;
using base::subtle::Barrier_AtomicIncrement;

using docdb::DocDbAwareFilterPolicy;
using docdb::DocKey;
using docdb::DocPath;
using docdb::DocRowwiseIterator;
using docdb::DocWriteBatch;
using docdb::SubDocKey;
using docdb::PrimitiveValue;

static CompactionPolicy *CreateCompactionPolicy() {
  return new BudgetedCompactionPolicy(FLAGS_tablet_compaction_budget_mb);
}

////////////////////////////////////////////////////////////
// TabletComponents
////////////////////////////////////////////////////////////

TabletComponents::TabletComponents(shared_ptr<MemRowSet> mrs,
                                   shared_ptr<RowSetTree> rs_tree)
    : memrowset(std::move(mrs)), rowsets(std::move(rs_tree)) {}

////////////////////////////////////////////////////////////
// Tablet
////////////////////////////////////////////////////////////
void EmitRocksDbMetricsAsJson(
    std::shared_ptr<rocksdb::Statistics> rocksdb_statistics,
    JsonWriter* writer,
    const MetricJsonOptions& opts) {
  // Make sure the class member 'rocksdb_statistics_' exists, as this is the stats object
  // maintained by RocksDB for this tablet.
  if (rocksdb_statistics == nullptr) {
    return;
  }
  // Emit all the ticker (gauge) metrics.
  for (std::pair<rocksdb::Tickers, std::string> entry : rocksdb::TickersNameMap) {
    // Start the metric object.
    writer->StartObject();
    // Write the name.
    writer->String("name");
    writer->String(entry.second);
    // Write the value.
    uint64_t value = rocksdb_statistics->getTickerCount(entry.first);
    writer->String("value");
    writer->Uint64(value);
    // Finish the metric object.
    writer->EndObject();
  }
  // Emit all the histogram metrics.
  rocksdb::HistogramData histogram_data;
  for (std::pair<rocksdb::Histograms, std::string> entry : rocksdb::HistogramsNameMap) {
    // Start the metric object.
    writer->StartObject();
    // Write the name.
    writer->String("name");
    writer->String(entry.second);
    // Write the value.
    rocksdb_statistics->histogramData(entry.first, &histogram_data);
    writer->String("total_count");
    writer->Double(histogram_data.count);
    writer->String("min");
    writer->Double(histogram_data.min);
    writer->String("mean");
    writer->Double(histogram_data.average);
    writer->String("median");
    writer->Double(histogram_data.median);
    writer->String("std_dev");
    writer->Double(histogram_data.standard_deviation);
    writer->String("percentile_95");
    writer->Double(histogram_data.percentile95);
    writer->String("percentile_99");
    writer->Double(histogram_data.percentile99);
    writer->String("max");
    writer->Double(histogram_data.max);
    writer->String("total_sum");
    writer->Double(histogram_data.sum);
    // Finish the metric object.
    writer->EndObject();
  }
}

CHECKED_STATUS EmitRocksDbMetricsAsPrometheus(
    std::shared_ptr<rocksdb::Statistics> rocksdb_statistics,
    PrometheusWriter* writer,
    const MetricEntity::AttributeMap& attrs) {
  // Make sure the class member 'rocksdb_statistics_' exists, as this is the stats object
  // maintained by RocksDB for this tablet.
  if (rocksdb_statistics == nullptr) {
    return Status::OK();
  }
  // Emit all the ticker (gauge) metrics.
  for (std::pair<rocksdb::Tickers, std::string> entry : rocksdb::TickersNameMap) {
    RETURN_NOT_OK(writer->WriteSingleEntry(
        attrs, entry.second, rocksdb_statistics->getTickerCount(entry.first)));
  }
  // Emit all the histogram metrics.
  rocksdb::HistogramData histogram_data;
  for (std::pair<rocksdb::Histograms, std::string> entry : rocksdb::HistogramsNameMap) {
    rocksdb_statistics->histogramData(entry.first, &histogram_data);

    auto copy_of_attr = attrs;
    const std::string hist_name = entry.second;
    RETURN_NOT_OK(writer->WriteSingleEntry(
        copy_of_attr, hist_name + "_sum", histogram_data.sum));
    RETURN_NOT_OK(writer->WriteSingleEntry(
        copy_of_attr, hist_name + "_count", histogram_data.count));
  }
  return Status::OK();
}

const char* Tablet::kDMSMemTrackerId = "DeltaMemStores";

Tablet::Tablet(
    const scoped_refptr<TabletMetadata>& metadata,
    const scoped_refptr<server::Clock>& clock,
    const shared_ptr<MemTracker>& parent_mem_tracker,
    MetricRegistry* metric_registry,
    const scoped_refptr<LogAnchorRegistry>& log_anchor_registry,
    const TabletOptions& tablet_options,
    TransactionParticipantContext* transaction_participant_context,
    TransactionCoordinatorContext* transaction_coordinator_context)
    : key_schema_(metadata->schema().CreateKeyProjection()),
      metadata_(metadata),
      table_type_(metadata->table_type()),
      log_anchor_registry_(log_anchor_registry),
      mem_tracker_(
          MemTracker::CreateTracker(-1, Substitute("tablet-$0", tablet_id()), parent_mem_tracker)),
      dms_mem_tracker_(MemTracker::CreateTracker(-1, kDMSMemTrackerId, mem_tracker_)),
      clock_(clock),
      mvcc_(clock, metadata->table_type() != TableType::KUDU_COLUMNAR_TABLE_TYPE),
      tablet_options_(tablet_options) {
  CHECK(schema()->has_column_ids());
  compaction_policy_.reset(CreateCompactionPolicy());

  if (metric_registry) {
    MetricEntity::AttributeMap attrs;
    // TODO(KUDU-745): table_id is apparently not set in the metadata.
    attrs["table_id"] = metadata_->table_id();
    attrs["table_name"] = metadata_->table_name();
    attrs["partition"] = metadata_->partition_schema().PartitionDebugString(metadata_->partition(),
                                                                            *schema());
    metric_entity_ = METRIC_ENTITY_tablet.Instantiate(metric_registry, tablet_id(), attrs);
    // If we are creating a KV table create the metrics callback.
    if (table_type_ != TableType::KUDU_COLUMNAR_TABLE_TYPE) {
      rocksdb_statistics_ = rocksdb::CreateDBStatistics();
      auto rocksdb_statistics = rocksdb_statistics_;
      metric_entity_->AddExternalJsonMetricsCb(
          [rocksdb_statistics](JsonWriter* jw, const MetricJsonOptions& opts) {
        EmitRocksDbMetricsAsJson(rocksdb_statistics, jw, opts);
      });

      metric_entity_->AddExternalPrometheusMetricsCb(
          [rocksdb_statistics, attrs](PrometheusWriter* pw) {
        auto s = EmitRocksDbMetricsAsPrometheus(rocksdb_statistics, pw, attrs);
        if (!s.ok()) {
          YB_LOG_EVERY_N(WARNING, 100) << "Failed to get Prometheus metrics: " << s.ToString();
        }
      });
    }
    metrics_.reset(new TabletMetrics(metric_entity_));
    METRIC_memrowset_size.InstantiateFunctionGauge(
            metric_entity_, Bind(&Tablet::MemRowSetSize, Unretained(this)))
        ->AutoDetach(&metric_detacher_);
    METRIC_on_disk_size.InstantiateFunctionGauge(
            metric_entity_, Bind(&Tablet::EstimateOnDiskSize, Unretained(this)))
        ->AutoDetach(&metric_detacher_);
  }

  if (transaction_participant_context) {
    transaction_participant_ = std::make_unique<TransactionParticipant>(
        transaction_participant_context);
  }

  if (transaction_coordinator_context) { // TODO(dtxn) Create coordinator only for status tablets
    CHECK_NOTNULL(transaction_participant_context);
    transaction_coordinator_ = std::make_unique<TransactionCoordinator>(
        transaction_coordinator_context, transaction_participant_.get());
  }

  flush_stats_ = make_shared<TabletFlushStats>();
  tablet_options_.listeners.emplace_back(flush_stats_);
}

Tablet::~Tablet() {
  Shutdown();
  dms_mem_tracker_->UnregisterFromParent();
  mem_tracker_->UnregisterFromParent();
}

Status Tablet::Open() {
  TRACE_EVENT0("tablet", "Tablet::Open");
  std::lock_guard<rw_spinlock> lock(component_lock_);
  CHECK_EQ(state_, kInitialized) << "already open";
  CHECK(schema()->has_column_ids());

  switch (table_type_) {
    case TableType::YQL_TABLE_TYPE: FALLTHROUGH_INTENDED;
    case TableType::REDIS_TABLE_TYPE:
      RETURN_NOT_OK(OpenKeyValueTablet());
      break;
    case TableType::KUDU_COLUMNAR_TABLE_TYPE:
      RETURN_NOT_OK(OpenKuduColumnarTablet());
      break;
    default:
      LOG(FATAL) << "Cannot open tablet " << tablet_id() << " with unknown table type "
                 << table_type_;
  }

  state_ = kBootstrapping;
  return Status::OK();
}

Status Tablet::OpenKeyValueTablet() {
  rocksdb::Options rocksdb_options;
  docdb::InitRocksDBOptions(&rocksdb_options, tablet_id(), rocksdb_statistics_, tablet_options_);

  // Install the history cleanup handler. Note that TabletRetentionPolicy is going to hold a raw ptr
  // to this tablet. So, we ensure that rocksdb_ is reset before this tablet gets destroyed.
  rocksdb_options.compaction_filter_factory = make_shared<DocDBCompactionFilterFactory>(
      make_shared<TabletRetentionPolicy>(this));

  const string db_dir = metadata()->rocksdb_dir();
  LOG(INFO) << "Creating RocksDB database in dir " << db_dir;

  // Create the directory table-uuid first.
  RETURN_NOT_OK_PREPEND(metadata()->fs_manager()->CreateDirIfMissing(DirName(db_dir)),
                        Substitute("Failed to create RocksDB table directory $0",
                                   DirName(db_dir)));

  RETURN_NOT_OK_PREPEND(metadata()->fs_manager()->CreateDirIfMissing(db_dir),
                        Substitute("Failed to create RocksDB tablet directory $0",
                                   db_dir));

  LOG(INFO) << "Opening RocksDB at: " << db_dir;
  rocksdb::DB* db = nullptr;
  rocksdb::Status rocksdb_open_status = rocksdb::DB::Open(rocksdb_options, db_dir, &db);
  if (!rocksdb_open_status.ok()) {
    LOG(ERROR) << "Failed to open a RocksDB database in directory " << db_dir << ": "
               << rocksdb_open_status.ToString();
    if (db != nullptr) {
      delete db;
    }
    return STATUS(IllegalState, rocksdb_open_status.ToString());
  }
  rocksdb_.reset(db);
  ql_storage_.reset(new docdb::QLRocksDBStorage(rocksdb_.get()));
  if (transaction_participant_) {
    transaction_participant_->SetDB(db);
  }
  LOG(INFO) << "Successfully opened a RocksDB database at " << db_dir;
  return Status::OK();
}

Status Tablet::OpenKuduColumnarTablet() {
  next_mrs_id_ = metadata_->last_durable_mrs_id() + 1;

  RowSetVector rowsets_opened;

  // open the tablet row-sets
  for (const shared_ptr<RowSetMetadata>& rowset_meta : metadata_->rowsets()) {
    shared_ptr<DiskRowSet> rowset;
    Status s = DiskRowSet::Open(rowset_meta, log_anchor_registry_.get(), &rowset, mem_tracker_);
    if (!s.ok()) {
      LOG(ERROR) << "Failed to open rowset " << rowset_meta->ToString() << ": "
                 << s.ToString();
      return s;
    }

    rowsets_opened.push_back(rowset);
  }

  shared_ptr<RowSetTree> new_rowset_tree(new RowSetTree());
  CHECK_OK(new_rowset_tree->Reset(rowsets_opened));
  // now that the current state is loaded, create the new MemRowSet with the next id
  shared_ptr<MemRowSet> new_mrs(new MemRowSet(next_mrs_id_++, *schema(),
                                              log_anchor_registry_.get(),
                                              mem_tracker_));
  components_ = new TabletComponents(new_mrs, new_rowset_tree);
  return Status::OK();
}

void Tablet::MarkFinishedBootstrapping() {
  CHECK_EQ(state_, kBootstrapping);
  state_ = kOpen;
}

void Tablet::SetShutdownRequestedFlag() {
  shutdown_requested_.store(true, std::memory_order::memory_order_release);
}

void Tablet::Shutdown() {
  SetShutdownRequestedFlag();
  UnregisterMaintenanceOps();

  LOG_SLOW_EXECUTION(WARNING, 1000,
                     Substitute("Tablet $0: Waiting for pending ops to complete", tablet_id())) {
    CHECK_OK(pending_op_counter_.WaitForAllOpsToFinish(MonoDelta::FromSeconds(60)));
  }

  if (transaction_coordinator_) {
    transaction_coordinator_->Shutdown();
  }

  std::lock_guard<rw_spinlock> lock(component_lock_);
  components_ = nullptr;
  // Shutdown the RocksDB instance for this table, if present.
  rocksdb_.reset();
  state_ = kShutdown;

  // In the case of deleting a tablet, we still keep the metadata around after
  // ShutDown(), and need to flush the metadata to indicate that the tablet is deleted.
  // During that flush, we don't want metadata to call back into the Tablet, so we
  // have to unregister the pre-flush callback.
  metadata_->SetPreFlushCallback(Bind(DoNothingStatusClosure));
}

Status Tablet::GetMappedReadProjection(const Schema& projection,
                                       Schema *mapped_projection) const {
  const Schema* cur_schema = schema();
  return cur_schema->GetMappedReadProjection(projection, mapped_projection);
}

BloomFilterSizing Tablet::bloom_sizing() const {
  return BloomFilterSizing::BySizeAndFPRate(FLAGS_tablet_bloom_block_size,
                                            FLAGS_tablet_bloom_target_fp_rate);
}

Status Tablet::NewRowIterator(const Schema &projection,
                              const boost::optional<TransactionId>& transaction_id,
                              gscoped_ptr<RowwiseIterator> *iter) const {
  // Yield current rows.
  MvccSnapshot snap(mvcc_);
  return NewRowIterator(projection, snap, Tablet::UNORDERED, transaction_id, iter);
}

Status Tablet::NewRowIterator(const Schema &projection,
                              const MvccSnapshot &snap,
                              const OrderMode order,
                              const boost::optional<TransactionId>& transaction_id,
                              gscoped_ptr<RowwiseIterator> *iter) const {
  CHECK_EQ(state_, kOpen);
  if (metrics_) {
    metrics_->scans_started->Increment();
  }
  VLOG(2) << "Created new Iterator under snap: " << snap.ToString();
  iter->reset(new Iterator(this, projection, snap, order, transaction_id));
  return Status::OK();
}

Status Tablet::DecodeWriteOperations(const Schema* client_schema,
                                     WriteOperationState* operation_state) {
  TRACE_EVENT0("tablet", "Tablet::DecodeWriteOperations");

  DCHECK_EQ(operation_state->row_ops().size(), 0);

  if (table_type_ != TableType::KUDU_COLUMNAR_TABLE_TYPE) {
    CHECK(operation_state->request()->has_write_batch())
        << "Write request for kv-table has no write batch";
    CHECK(!operation_state->request()->has_row_operations())
        << "Write request for kv-table has row operations";
    // We construct a RocksDB write batch immediately before applying it.
  } else {
    CHECK(!operation_state->request()->has_write_batch())
        << "Write request for kudu-table has write batch";
    CHECK(operation_state->request()->has_row_operations())
        << "Write request for kudu-table has no row operations";
    // Acquire the schema lock in shared mode, so that the schema doesn't
    // change while this transaction is in-flight.
    operation_state->AcquireSchemaLock(&schema_lock_);

    // The Schema needs to be held constant while any transactions are between
    // PREPARE and APPLY stages
    TRACE("PREPARE: Decoding operations");
    vector<DecodedRowOperation> ops;

    // Decode the ops
    RowOperationsPBDecoder dec(&operation_state->request()->row_operations(),
                               client_schema,
                               schema(),
                               operation_state->arena());
    RETURN_NOT_OK(dec.DecodeOperations(&ops));

    // Create RowOp objects for each
    vector<RowOp*> row_ops;
    ops.reserve(ops.size());
    for (const DecodedRowOperation& op : ops) {
      row_ops.push_back(new RowOp(op));
    }

    // Important to set the schema before the ops -- we need the
    // schema in order to stringify the ops.
    operation_state->set_schema_at_decode_time(schema());
    operation_state->swap_row_ops(&row_ops);
  }

  return Status::OK();
}

Status Tablet::AcquireKuduRowLocks(WriteOperationState *operation_state) {
  if (table_type_ == TableType::KUDU_COLUMNAR_TABLE_TYPE) {
    TRACE_EVENT1("tablet", "Tablet::AcquireKuduRowLocks",
                 "num_locks", operation_state->row_ops().size());
    TRACE("PREPARE: Acquiring locks for $0 operations", operation_state->row_ops().size());
    for (RowOp* op : operation_state->row_ops()) {
      RETURN_NOT_OK(AcquireLockForOp(operation_state, op));
    }
    TRACE("PREPARE: locks acquired");
  }
  return Status::OK();
}

Status Tablet::CheckRowInTablet(const ConstContiguousRow& row) const {
  bool contains_row;
  RETURN_NOT_OK(metadata_->partition_schema().PartitionContainsRow(metadata_->partition(),
                                                                   row,
                                                                   &contains_row));

  if (PREDICT_FALSE(!contains_row)) {
    return STATUS(NotFound,
                  Substitute("Row not in tablet partition. Partition: '$0', row: '$1'.",
                             metadata_->partition_schema().PartitionDebugString(
                                 metadata_->partition(), *schema()),
                             metadata_->partition_schema().RowDebugString(row)));
  }
  return Status::OK();
}

Status Tablet::AcquireLockForOp(WriteOperationState* operation_state, RowOp* op) {
  CHECK_EQ(TableType::KUDU_COLUMNAR_TABLE_TYPE, table_type_);

  ConstContiguousRow row_key(&key_schema_, op->decoded_op.row_data);
  op->key_probe.reset(new tablet::RowSetKeyProbe(row_key));
  RETURN_NOT_OK(CheckRowInTablet(row_key));

  ScopedRowLock row_lock(&lock_manager_,
                         operation_state,
                         op->key_probe->encoded_key_slice(),
                         LockManager::LOCK_EXCLUSIVE);
  op->row_lock = row_lock.Pass();
  return Status::OK();
}

void Tablet::StartOperation(WriteOperationState* operation_state) {
  std::unique_ptr<ScopedWriteOperation> mvcc_tx;

  // If the state already has a hybrid_time then we're replaying a transaction that occurred
  // before a crash or at another node...
  const HybridTime existing_hybrid_time = operation_state->hybrid_time_even_if_unset();

  if (existing_hybrid_time != HybridTime::kInvalidHybridTime) {
    mvcc_tx.reset(new ScopedWriteOperation(&mvcc_, existing_hybrid_time));
    // ... otherwise this is a new transaction and we must assign a new hybrid_time. We either
    // assign a hybrid_time in the future, if the consistency mode is COMMIT_WAIT, or we assign
    // one in the present if the consistency mode is any other one.
  } else if (operation_state->external_consistency_mode() == COMMIT_WAIT) {
    mvcc_tx.reset(new ScopedWriteOperation(&mvcc_, ScopedWriteOperation::NOW_LATEST));
  } else {
    mvcc_tx.reset(new ScopedWriteOperation(&mvcc_, ScopedWriteOperation::NOW));
  }
  operation_state->SetMvccTxAndHybridTime(std::move(mvcc_tx));
}

Status Tablet::InsertUnlocked(WriteOperationState *operation_state,
                              RowOp* insert) {
  // A check only needed for Kudu's columnar format that has to happen before the row lock.
  const TabletComponents* comps =
      table_type_ == TableType::KUDU_COLUMNAR_TABLE_TYPE ?
      DCHECK_NOTNULL(operation_state->tablet_components()) : nullptr;

  CHECK(state_ == kOpen || state_ == kBootstrapping);
  // make sure that the WriteOperationState has the component lock and that
  // there the RowOp has the row lock.

  if (table_type_ == TableType::KUDU_COLUMNAR_TABLE_TYPE) {
    DCHECK(insert->has_row_lock()) << "RowOp must hold the row lock.";
  }

  DCHECK_EQ(operation_state->schema_at_decode_time(), schema()) << "Raced against schema change";
  DCHECK(operation_state->op_id().IsInitialized()) << "OperationState OpId needed for anchoring";

  ProbeStats stats;

  // Submit the stats before returning from this function
  ProbeStatsSubmitter submitter(stats, metrics_.get());

  switch (table_type_) {
    case TableType::KUDU_COLUMNAR_TABLE_TYPE:
      return KuduColumnarInsertUnlocked(operation_state, insert, comps, &stats);
    default:
      LOG(FATAL) << "Cannot perform an unlocked insert for table type " << table_type_;
  }
  return STATUS(IllegalState, "This cannot happen");
}

Status Tablet::KuduColumnarInsertUnlocked(
    WriteOperationState *operation_state,
    RowOp* insert,
    const TabletComponents* comps,
    ProbeStats* stats) {

  // First, ensure that it is a unique key by checking all the open RowSets.
  if (FLAGS_tablet_do_dup_key_checks) {
    vector<RowSet *> to_check;
    comps->rowsets->FindRowSetsWithKeyInRange(insert->key_probe->encoded_key_slice(),
                                              &to_check);

    for (const RowSet *rowset : to_check) {
      bool present = false;
      RETURN_NOT_OK(rowset->CheckRowPresent(*insert->key_probe, &present, stats));
      if (PREDICT_FALSE(present)) {
        Status s = STATUS(AlreadyPresent, "key already present");
        if (metrics_) {
          metrics_->insertions_failed_dup_key->Increment();
        }
        insert->SetFailed(s);
        return s;
      }
    }
  }

  HybridTime ht = operation_state->hybrid_time();
  ConstContiguousRow row(schema(), insert->decoded_op.row_data);

  // TODO: the Insert() call below will re-encode the key, which is a
  // waste. Should pass through the KeyProbe structure perhaps.

  // Now try to insert into memrowset. The memrowset itself will return
  // AlreadyPresent if it has already been inserted there.
  Status s = comps->memrowset->Insert(ht, row, operation_state->op_id());
  if (PREDICT_TRUE(s.ok())) {
    insert->SetInsertSucceeded(comps->memrowset->mrs_id());
  } else {
    if (s.IsAlreadyPresent() && metrics_) {
      metrics_->insertions_failed_dup_key->Increment();
    }
    insert->SetFailed(s);
  }
  return s;
}

Status Tablet::MutateRowUnlocked(WriteOperationState *operation_state,
                                 RowOp* mutate) {
  DCHECK(operation_state != nullptr) << "you must have a WriteOperationState";
  DCHECK(operation_state->op_id().IsInitialized()) << "OperationState OpId needed for anchoring";
  DCHECK_EQ(operation_state->schema_at_decode_time(), schema());

  gscoped_ptr<OperationResultPB> result(new OperationResultPB());

  const TabletComponents* comps = DCHECK_NOTNULL(operation_state->tablet_components());

  // Validate the update.
  RowChangeListDecoder rcl_decoder(mutate->decoded_op.changelist);
  Status s = rcl_decoder.Init();
  if (rcl_decoder.is_reinsert()) {
    // REINSERT mutations are the byproduct of an INSERT on top of a ghost
    // row, not something the user is allowed to specify on their own.
    s = STATUS(InvalidArgument, "User may not specify REINSERT mutations");
  }
  if (!s.ok()) {
    mutate->SetFailed(s);
    return s;
  }

  HybridTime ht = operation_state->hybrid_time();

  ProbeStats stats;
  // Submit the stats before returning from this function
  ProbeStatsSubmitter submitter(stats, metrics_.get());

  // First try to update in memrowset.
  s = comps->memrowset->MutateRow(ht,
                                  *mutate->key_probe,
                                  mutate->decoded_op.changelist,
                                  operation_state->op_id(),
                                  &stats,
                                  result.get());
  if (s.ok()) {
    mutate->SetMutateSucceeded(result.Pass());
    return s;
  }
  if (!s.IsNotFound()) {
    mutate->SetFailed(s);
    return s;
  }

  // Next, check the disk rowsets.

  // TODO: could iterate the rowsets in a smart order
  // based on recent statistics - eg if a rowset is getting
  // updated frequently, pick that one first.
  vector<RowSet *> to_check;
  comps->rowsets->FindRowSetsWithKeyInRange(mutate->key_probe->encoded_key_slice(),
                                            &to_check);
  for (RowSet *rs : to_check) {
    s = rs->MutateRow(ht,
                      *mutate->key_probe,
                      mutate->decoded_op.changelist,
                      operation_state->op_id(),
                      &stats,
                      result.get());
    if (s.ok()) {
      mutate->SetMutateSucceeded(result.Pass());
      return s;
    }
    if (!s.IsNotFound()) {
      mutate->SetFailed(s);
      return s;
    }
  }

  s = STATUS(NotFound, "key not found");
  mutate->SetFailed(s);
  return s;
}

void Tablet::StartApplying(WriteOperationState* operation_state) {
  if (table_type_ == TableType::KUDU_COLUMNAR_TABLE_TYPE) {
    shared_lock<rw_spinlock> lock(component_lock_);
    operation_state->StartApplying();
    operation_state->set_tablet_components(components_);
  } else {
    operation_state->StartApplying();
  }
}

void Tablet::ApplyRowOperations(WriteOperationState* operation_state) {
  last_committed_write_index_.store(operation_state->op_id().index(), std::memory_order_release);
  StartApplying(operation_state);
  switch (table_type_) {
    case TableType::KUDU_COLUMNAR_TABLE_TYPE: {
      for (RowOp* row_op : operation_state->row_ops()) {
        ApplyKuduRowOperation(operation_state, row_op);
      }
      return;
    }
    case TableType::YQL_TABLE_TYPE:
    case TableType::REDIS_TABLE_TYPE: {
      const KeyValueWriteBatchPB& put_batch =
          operation_state->consensus_round() && operation_state->consensus_round()->replicate_msg()
              // Online case.
              ? operation_state->consensus_round()->replicate_msg()->write_request().write_batch()
              // Bootstrap case.
              : operation_state->request()->write_batch();

      ApplyKeyValueRowOperations(put_batch,
                                 operation_state->op_id(),
                                 operation_state->hybrid_time());
      return;
    }
  }
  LOG(FATAL) << "Invalid table type: " << table_type_;
}

Status Tablet::CreateCheckpoint(const std::string& dir,
                                google::protobuf::RepeatedPtrField<RocksDBFilePB>* rocksdb_files) {
  GUARD_AGAINST_ROCKSDB_SHUTDOWN;

  CHECK(table_type_ != TableType::KUDU_COLUMNAR_TABLE_TYPE);

  std::lock_guard<std::mutex> lock(create_checkpoint_lock_);

  rocksdb::Status status;
  std::unique_ptr<rocksdb::Checkpoint> checkpoint;
  {
    rocksdb::Checkpoint* checkpoint_raw_ptr = nullptr;
    status = rocksdb::Checkpoint::Create(rocksdb_.get(), &checkpoint_raw_ptr);
    if (!status.ok()) {
      return STATUS(IllegalState, Substitute("Unable to create checkpoint object: $0",
                                             status.ToString()));
    }
    checkpoint.reset(checkpoint_raw_ptr);
  }

  status = checkpoint->CreateCheckpoint(dir);

  if (!status.ok()) {
    LOG(WARNING) << "Create checkpoint status: " << status.ToString();
    return STATUS(IllegalState, Substitute("Unable to create checkpoint: $0", status.ToString()));
  }
  LOG(INFO) << "Checkpoint created in " << dir;

  if (rocksdb_files != nullptr) {
    vector<rocksdb::Env::FileAttributes> files_attrs;
    status = rocksdb_->GetEnv()->GetChildrenFileAttributes(dir, &files_attrs);
    if (!status.ok()) {
      return STATUS(IllegalState, Substitute("Unable to get RocksDB files in dir $0: $1", dir,
                                             status.ToString()));
    }

    for (const auto& file_attrs : files_attrs) {
      if (file_attrs.name == "." || file_attrs.name == "..") {
        continue;
      }
      auto rocksdb_file_pb = rocksdb_files->Add();
      rocksdb_file_pb->set_name(file_attrs.name);
      rocksdb_file_pb->set_size_bytes(file_attrs.size_bytes);
    }
  }

  last_rocksdb_checkpoint_dir_ = dir;

  return Status::OK();
}

void Tablet::PrepareTransactionWriteBatch(
    const KeyValueWriteBatchPB& put_batch,
    HybridTime hybrid_time,
    WriteBatch* rocksdb_write_batch) {
  if (put_batch.transaction().has_isolation()) {
    // Store transaction metadata (status tablet, isolation level etc.)
    transaction_participant()->Add(put_batch.transaction(), rocksdb_write_batch);
  }
  auto transaction_id = FullyDecodeTransactionId(put_batch.transaction().transaction_id());
  CHECK_OK(transaction_id);
  auto metadata = transaction_participant()->Metadata(*transaction_id);
  CHECK(metadata) << "Transaction metadata missing: " << *transaction_id;

  auto isolation_level = metadata->isolation;
  yb::docdb::PrepareTransactionWriteBatch(
      put_batch, hybrid_time, rocksdb_write_batch, *transaction_id, isolation_level);
}

void Tablet::ApplyKeyValueRowOperations(const KeyValueWriteBatchPB& put_batch,
                                        const consensus::OpId& op_id,
                                        const HybridTime hybrid_time,
                                        rocksdb::WriteBatch* rocksdb_write_batch) {
  // Write batch could be preallocated, here we handle opposite case.
  if (rocksdb_write_batch == nullptr) {
    WriteBatch write_batch;
    ApplyKeyValueRowOperations(put_batch, op_id, hybrid_time, &write_batch);
    return;
  }

  DCHECK_NE(table_type_, TableType::KUDU_COLUMNAR_TABLE_TYPE);
  if (put_batch.kv_pairs_size() == 0) {
    return;
  }

  rocksdb_write_batch->SetUserOpId(rocksdb::OpId(op_id.term(), op_id.index()));

  if (put_batch.has_transaction()) {
    PrepareTransactionWriteBatch(put_batch, hybrid_time, rocksdb_write_batch);
  } else {
    PrepareNonTransactionWriteBatch(put_batch, hybrid_time, rocksdb_write_batch);
  }

  // We are using Raft replication index for the RocksDB sequence number for
  // all members of this write batch.
  rocksdb::WriteOptions write_options;
  InitRocksDBWriteOptions(&write_options);

  flush_stats_->AboutToWriteToDb(hybrid_time);
  auto rocksdb_write_status = rocksdb_->Write(write_options, rocksdb_write_batch);
  if (!rocksdb_write_status.ok()) {
    LOG(FATAL) << "Failed to write a batch with " << rocksdb_write_batch->Count() << " operations"
               << " into RocksDB: " << rocksdb_write_status.ToString();
  }
}

namespace {

// Separate Redis / QL / row operations write batches from write_request in preparation for the
// write transaction. Leave just the tablet id behind. Return Redis / QL / row operations, etc.
// in batch_request.
void SetupKeyValueBatch(WriteRequestPB* write_request, WriteRequestPB* batch_request) {
  batch_request->Swap(write_request);
  write_request->set_allocated_tablet_id(batch_request->release_tablet_id());
  if (batch_request->write_batch().has_transaction()) {
    write_request->mutable_write_batch()->mutable_transaction()->Swap(
        batch_request->mutable_write_batch()->mutable_transaction());
  }
}

} // namespace

Status Tablet::KeyValueBatchFromRedisWriteBatch(
    WriteRequestPB* redis_write_request,
    LockBatch* keys_locked,
    vector<RedisResponsePB>* responses) {
  GUARD_AGAINST_ROCKSDB_SHUTDOWN;
  docdb::DocOperations doc_ops;
  // Since we take exclusive locks, it's okay to use Now as the read TS for writes.
  const HybridTime read_hybrid_time = clock_->Now();
  WriteRequestPB batch_request;
  SetupKeyValueBatch(redis_write_request, &batch_request);
  auto* redis_write_batch = batch_request.mutable_redis_write_batch();

  doc_ops.reserve(redis_write_batch->size());
  for (size_t i = 0; i < redis_write_batch->size(); i++) {
    doc_ops.emplace_back(new RedisWriteOperation(
        redis_write_batch->Mutable(i), read_hybrid_time));
  }
  RETURN_NOT_OK(StartDocWriteOperation(
      doc_ops, keys_locked, redis_write_request->mutable_write_batch()));
  for (size_t i = 0; i < doc_ops.size(); i++) {
    responses->emplace_back(
        (down_cast<RedisWriteOperation*>(doc_ops[i].get()))->response());
  }

  return Status::OK();
}

Status Tablet::HandleRedisReadRequest(HybridTime timestamp,
                                      const RedisReadRequestPB& redis_read_request,
                                      RedisResponsePB* response) {
  GUARD_AGAINST_ROCKSDB_SHUTDOWN;
  ScopedTabletMetricsTracker metrics_tracker(metrics_->redis_read_latency);

  docdb::RedisReadOperation doc_op(redis_read_request);
  RETURN_NOT_OK(doc_op.Execute(rocksdb_.get(), timestamp));
  *response = std::move(doc_op.response());
  return Status::OK();
}

Status Tablet::HandleQLReadRequest(
    HybridTime timestamp, const QLReadRequestPB& ql_read_request,
    const TransactionMetadataPB& transaction_metadata, QLResponsePB* response,
    gscoped_ptr<faststring>* rows_data) {
  GUARD_AGAINST_ROCKSDB_SHUTDOWN;
  ScopedTabletMetricsTracker metrics_tracker(metrics_->ql_read_latency);

  if (metadata()->schema_version() != ql_read_request.schema_version()) {
    response->set_status(QLResponsePB::YQL_STATUS_SCHEMA_VERSION_MISMATCH);
    return Status::OK();
  }

  Result<TransactionOperationContextOpt> txn_op_ctx =
      CreateTransactionOperationContext(transaction_metadata);
  RETURN_NOT_OK(txn_op_ctx);
  return AbstractTablet::HandleQLReadRequest(
      timestamp, ql_read_request, *txn_op_ctx, response, rows_data);
}

CHECKED_STATUS Tablet::CreatePagingStateForRead(const QLReadRequestPB& ql_read_request,
                                                const size_t row_count,
                                                QLResponsePB* response) const {
  // If there is no hash column in the read request, this is a full-table query. And if there is no
  // paging state in the response, we are done reading from the current tablet. In this case, we
  // should return the exclusive end partition key of this tablet if not empty which is the start
  // key of the next tablet. Do so only if the request has no row count limit, or there is and we
  // haven't hit it, or we are asked to return paging state even when we have hit the limit.
  // Otherwise, leave the paging state empty which means we are completely done reading for the
  // whole SELECT statement.
  if (ql_read_request.hashed_column_values().empty() && !response->has_paging_state() &&
      (!ql_read_request.has_limit() || row_count < ql_read_request.limit() ||
          ql_read_request.return_paging_state())) {
    const string& next_partition_key = metadata_->partition().partition_key_end();
    if (!next_partition_key.empty()) {
      response->mutable_paging_state()->set_next_partition_key(next_partition_key);
    }
  }

  // If there is a paging state, update the total number of rows read so far.
  if (response->has_paging_state()) {
    response->mutable_paging_state()->set_total_num_rows_read(
        ql_read_request.paging_state().total_num_rows_read() + row_count);
  }
  return Status::OK();
}

Status Tablet::KeyValueBatchFromQLWriteBatch(
    WriteRequestPB* ql_write_request,
    LockBatch *keys_locked,
    WriteResponsePB* write_response,
    WriteOperationState* operation_state) {
  GUARD_AGAINST_ROCKSDB_SHUTDOWN;

  docdb::DocOperations doc_ops;
  WriteRequestPB batch_request;
  SetupKeyValueBatch(ql_write_request, &batch_request);
  auto* ql_write_batch = batch_request.mutable_ql_write_batch();

  doc_ops.reserve(ql_write_batch->size());

  Result<TransactionOperationContextOpt> txn_op_ctx =
      CreateTransactionOperationContext(ql_write_request->write_batch().transaction());
  RETURN_NOT_OK(txn_op_ctx);
  for (size_t i = 0; i < ql_write_batch->size(); i++) {
    QLWriteRequestPB* req = ql_write_batch->Mutable(i);
    QLResponsePB* resp = write_response->add_ql_response_batch();
    if (metadata_->schema_version() != req->schema_version()) {
      resp->set_status(QLResponsePB::YQL_STATUS_SCHEMA_VERSION_MISMATCH);
    } else {
      doc_ops.emplace_back(new QLWriteOperation(req, metadata_->schema(), resp, *txn_op_ctx));
    }
  }
  RETURN_NOT_OK(StartDocWriteOperation(
      doc_ops, keys_locked, ql_write_request->mutable_write_batch()));
  for (size_t i = 0; i < doc_ops.size(); i++) {
    QLWriteOperation* ql_write_op = down_cast<QLWriteOperation*>(doc_ops[i].get());
    // If the QL write op returns a rowblock, move the op to the transaction state to return the
    // rows data as a sidecar after the transaction completes.
    if (ql_write_op->rowblock() != nullptr) {
      doc_ops[i].release();
      operation_state->ql_write_ops()->emplace_back(unique_ptr<QLWriteOperation>(ql_write_op));
    }
  }

  return Status::OK();
}

Status Tablet::AcquireLocksAndPerformDocOperations(WriteOperationState *state) {
  if (table_type_ != KUDU_COLUMNAR_TABLE_TYPE) {
    LockBatch locks_held;
    WriteRequestPB* key_value_write_request = state->mutable_request();

    bool invalid_table_type = true;
    switch (table_type_) {
      case TableType::REDIS_TABLE_TYPE: {
        vector<RedisResponsePB> responses;
        RETURN_NOT_OK(KeyValueBatchFromRedisWriteBatch(key_value_write_request,
            &locks_held, &responses));
        for (auto &redis_resp : responses) {
          *(state->response()->add_redis_response_batch()) = std::move(redis_resp);
        }
        invalid_table_type = false;
        break;
      }
      case TableType::YQL_TABLE_TYPE: {
        CHECK_NE(key_value_write_request->ql_write_batch_size() > 0,
                 key_value_write_request->row_operations().rows().size() > 0)
            << "QL write and Kudu row operations not supported in the same request";
        if (key_value_write_request->ql_write_batch_size() > 0) {
          vector<QLResponsePB> responses;
          RETURN_NOT_OK(KeyValueBatchFromQLWriteBatch(
              key_value_write_request, &locks_held, state->response(), state));
        } else {
          // TODO: Remove this row op based codepath after all tests set yql_write_batch.
          RETURN_NOT_OK(KeyValueBatchFromKuduRowOps(key_value_write_request, &locks_held));
        }
        invalid_table_type = false;
        break;
      }
      case TableType::KUDU_COLUMNAR_TABLE_TYPE: {
        break;
      }
    }
    if (invalid_table_type) {
      FATAL_INVALID_ENUM_VALUE(TableType, table_type_);
    }
    // If there is a non-zero number of operations, we expect to be holding locks. The reverse is
    // not always true, because we could decide to avoid writing based on results of reading.
    DCHECK(!locks_held.empty() ||
           key_value_write_request->write_batch().kv_pairs_size() == 0)
        << "Expect to be holding locks for a non-zero number of write operations: "
        << key_value_write_request->write_batch().DebugString();
    state->ReplaceDocDBLocks(std::move(locks_held));

    DCHECK(!key_value_write_request->has_schema()) << "Schema not empty in key-value batch";
    DCHECK(!key_value_write_request->has_row_operations())
        << "Rows operations not empty in key-value batch";
    DCHECK_EQ(key_value_write_request->redis_write_batch_size(), 0)
        << "Redis write batch not empty in key-value batch";
    DCHECK_EQ(key_value_write_request->ql_write_batch_size(), 0)
        << "QL write batch not empty in key-value batch";
  }
  return Status::OK();
}

Status Tablet::KeyValueBatchFromKuduRowOps(WriteRequestPB* kudu_write_request,
                                           LockBatch *keys_locked) {
  GUARD_AGAINST_ROCKSDB_SHUTDOWN;

  TRACE("PREPARE: Decoding operations");

  WriteRequestPB row_operations_request;
  SetupKeyValueBatch(kudu_write_request, &row_operations_request);
  auto* write_batch = kudu_write_request->mutable_write_batch();

  TRACE("Acquiring schema lock in shared mode");
  shared_lock<rw_semaphore> schema_lock(schema_lock_);
  TRACE("Acquired schema lock");

  Schema client_schema;

  RETURN_NOT_OK(SchemaFromPB(row_operations_request.schema(), &client_schema));

  // Allocating temporary arena for decoding.
  Arena arena(32 * 1024, 4 * 1024 * 1024);

  vector<DecodedRowOperation> row_ops;
  RowOperationsPBDecoder row_operation_decoder(&row_operations_request.row_operations(),
                                               &client_schema,
                                               schema(),
                                               &arena);

  RETURN_NOT_OK(row_operation_decoder.DecodeOperations(&row_ops));

  RETURN_NOT_OK(CreateWriteBatchFromKuduRowOps(row_ops, write_batch, keys_locked));

  return Status::OK();
}

namespace {

DocPath DocPathForColumn(const KeyBytes& encoded_doc_key, ColumnId col_id) {
  return DocPath(encoded_doc_key, PrimitiveValue(col_id));
}

}  // namespace

Status Tablet::CreateWriteBatchFromKuduRowOps(const vector<DecodedRowOperation> &row_ops,
                                              KeyValueWriteBatchPB* write_batch,
                                              LockBatch* keys_locked) {
  GUARD_AGAINST_ROCKSDB_SHUTDOWN;
  docdb::DocOperations doc_ops;
  for (DecodedRowOperation row_op : row_ops) {
    // row_data contains the row key for all Kudu operation types (insert/update/delete).
    ConstContiguousRow contiguous_row(schema(), row_op.row_data);
    EncodedKeyBuilder key_builder(schema());
    for (int i = 0; i < schema()->num_key_columns(); i++) {
      DCHECK(!schema()->column(i).is_nullable())
          << "Column " << i << " (part of row key) cannot be nullable";
      key_builder.AddColumnKey(contiguous_row.cell_ptr(i));
    }
    unique_ptr<EncodedKey> encoded_key(key_builder.BuildEncodedKey());

    const DocKey doc_key = DocKey::FromKuduEncodedKey(*encoded_key, *schema());
    const auto encoded_doc_key = doc_key.Encode();

    switch (row_op.type) {
      case RowOperationsPB_Type_DELETE: {
        doc_ops.emplace_back(
            new KuduWriteOperation(DocPath(encoded_doc_key),
            PrimitiveValue(ValueType::kTombstone)));
        break;
      }
      case RowOperationsPB_Type_UPDATE: {
        RowChangeListDecoder decoder(row_op.changelist);
        RETURN_NOT_OK(decoder.Init());
        while (decoder.HasNext()) {
          CHECK(decoder.is_update());
          RowChangeListDecoder::DecodedUpdate update;
          RETURN_NOT_OK(decoder.DecodeNext(&update));
          doc_ops.emplace_back(new KuduWriteOperation(
              DocPathForColumn(encoded_doc_key, update.col_id),
              update.null ? PrimitiveValue(ValueType::kTombstone)
                          : PrimitiveValue::FromKuduValue(
                                schema()->column_by_id(update.col_id).type_info()->type(),
                                update.raw_value)));
        }
        break;
      }
      case RowOperationsPB_Type_INSERT: {
        for (int i = schema()->num_key_columns(); i < schema()->num_columns(); i++) {
          const ColumnSchema &col_schema = schema()->column(i);
          const DataType data_type = col_schema.type_info()->type();

          PrimitiveValue column_value;
          if (col_schema.is_nullable() && contiguous_row.is_null(i)) {
            // Skip this column as it is null and we are already overwriting the entire row at
            // the top. Another option would be to explicitly delete it like so:
            //
            //   column_value = PrimitiveValue(ValueType::kTombstone);
            //
            // This would make sense in case we just wanted to update a few columns in a
            // Cassandra-style INSERT ("upsert").
            continue;
          } else {
            column_value = PrimitiveValue::FromKuduValue(data_type, contiguous_row.CellSlice(i));
          }
          doc_ops.emplace_back(new KuduWriteOperation(DocPathForColumn(
              encoded_doc_key, schema()->column_id(i)), column_value));
        }
        break;
      }
      default: {
        LOG(FATAL) << "Unsupported row operation type " << row_op.type
                   << " for a RocksDB-backed table";
      }
    }
  }
  return StartDocWriteOperation(doc_ops, keys_locked, write_batch);
}

void Tablet::ApplyKuduRowOperation(WriteOperationState *operation_state,
                                   RowOp *row_op) {
  CHECK_EQ(TableType::KUDU_COLUMNAR_TABLE_TYPE, table_type_)
      << "Failed while trying to apply Kudu row operations on a non-Kudu table";
  switch (row_op->decoded_op.type) {
    case RowOperationsPB::INSERT:
      ignore_result(InsertUnlocked(operation_state, row_op));
      return;

    case RowOperationsPB::UPDATE:
    case RowOperationsPB::DELETE:
      ignore_result(MutateRowUnlocked(operation_state, row_op));
      return;

    default:
      LOG(FATAL) << RowOperationsPB::Type_Name(row_op->decoded_op.type);
  }
}

void Tablet::ModifyRowSetTree(const RowSetTree& old_tree,
                              const RowSetVector& rowsets_to_remove,
                              const RowSetVector& rowsets_to_add,
                              RowSetTree* new_tree) {
  RowSetVector post_swap;

  // O(n^2) diff algorithm to collect the set of rowsets excluding
  // the rowsets that were included in the compaction
  int num_removed = 0;

  for (const shared_ptr<RowSet> &rs : old_tree.all_rowsets()) {
    // Determine if it should be removed
    bool should_remove = false;
    for (const shared_ptr<RowSet> &to_remove : rowsets_to_remove) {
      if (to_remove == rs) {
        should_remove = true;
        num_removed++;
        break;
      }
    }
    if (!should_remove) {
      post_swap.push_back(rs);
    }
  }

  CHECK_EQ(num_removed, rowsets_to_remove.size());

  // Then push the new rowsets on the end of the new list
  std::copy(rowsets_to_add.begin(),
            rowsets_to_add.end(),
            std::back_inserter(post_swap));

  CHECK_OK(new_tree->Reset(post_swap));
}

void Tablet::AtomicSwapRowSets(const RowSetVector &old_rowsets,
                               const RowSetVector &new_rowsets) {
  if (table_type_ != TableType::KUDU_COLUMNAR_TABLE_TYPE) {
    return;
  }
  std::lock_guard<rw_spinlock> lock(component_lock_);
  AtomicSwapRowSetsUnlocked(old_rowsets, new_rowsets);
}

void Tablet::AtomicSwapRowSetsUnlocked(const RowSetVector &to_remove,
                                       const RowSetVector &to_add) {
  if (table_type_ != TableType::KUDU_COLUMNAR_TABLE_TYPE) {
    return;
  }
  DCHECK(component_lock_.is_locked());

  shared_ptr<RowSetTree> new_tree(new RowSetTree());
  ModifyRowSetTree(*components_->rowsets,
                   to_remove, to_add, new_tree.get());

  components_ = new TabletComponents(components_->memrowset, new_tree);
}

Status Tablet::DoMajorDeltaCompaction(const vector<ColumnId>& col_ids,
                                      shared_ptr<RowSet> input_rs) {
  if (table_type_ != TableType::KUDU_COLUMNAR_TABLE_TYPE) {
    return Status::OK();
  }
  CHECK_EQ(state_, kOpen);
  Status s = down_cast<DiskRowSet*>(input_rs.get())
      ->MajorCompactDeltaStoresWithColumnIds(col_ids);
  return s;
}

Status Tablet::Flush(FlushMode mode) {
  if (table_type_ != TableType::KUDU_COLUMNAR_TABLE_TYPE) {
    return FlushUnlocked(mode);
  }
  TRACE_EVENT1("tablet", "Tablet::Flush", "id", tablet_id());
  std::lock_guard<Semaphore> lock(rowsets_flush_sem_);
  return FlushUnlocked(mode);
}

Status Tablet::FlushUnlocked(FlushMode mode) {
  TRACE_EVENT0("tablet", "Tablet::FlushUnlocked");

  if (table_type_ != TableType::KUDU_COLUMNAR_TABLE_TYPE) {
    // TODO(bojanserafimov): Can raise null pointer exception if
    // the tablet just got shutdown. Acquire a read lock on component_lock_?
    rocksdb::FlushOptions options;
    options.wait = mode == FlushMode::kSync;
    rocksdb_->Flush(options);
    return Status::OK();
  }

  RowSetsInCompaction input;
  shared_ptr<MemRowSet> old_mrs;
  {
    // Create a new MRS with the latest schema.
    std::lock_guard<rw_spinlock> lock(component_lock_);
    RETURN_NOT_OK(ReplaceMemRowSetUnlocked(&input, &old_mrs));
  }

  // Wait for any in-flight transactions to finish against the old MRS
  // before we flush it.
  mvcc_.WaitForApplyingOperationsToCommit();

  // Note: "input" should only contain old_mrs.
  return FlushInternal(input, old_mrs);
}

Status Tablet::ImportData(const std::string& source_dir) {
  DCHECK_NE(table_type_, TableType::KUDU_COLUMNAR_TABLE_TYPE);
  return rocksdb_->Import(source_dir);
}

#define INTENT_VALUE_SCHECK(lhs, op, rhs, msg) \
  BOOST_PP_CAT(SCHECK_, op)(lhs, \
                            rhs, \
                            Corruption, \
                            Format("Bad intent value, $0 in $1, transaction: $2", \
                                   msg, \
                                   intent_iter->value().ToDebugHexString(), \
                                   transaction_id_slice.ToDebugHexString()))

// We apply intents using by iterating over whole transaction reverse index.
// Using value of reverse index record we find original intent record and apply it.
// After that we delete both intent record and reverse index record.
// TODO(dtxn) use separate thread for applying intents.
// TODO(dtxn) use multiple batches when applying really big transaction.
Status Tablet::ApplyIntents(const TransactionApplyData& data) {
  auto reverse_index_iter = docdb::CreateRocksDBIterator(
      rocksdb_.get(),
      docdb::BloomFilterMode::DONT_USE_BLOOM_FILTER,
      boost::none,
      rocksdb::kDefaultQueryId);

  auto intent_iter = docdb::CreateRocksDBIterator(rocksdb_.get(),
                                                  docdb::BloomFilterMode::DONT_USE_BLOOM_FILTER,
                                                  boost::none,
                                                  rocksdb::kDefaultQueryId);

  KeyBytes txn_reverse_index_prefix;
  Slice transaction_id_slice(data.transaction_id.data, TransactionId::static_size());
  AppendTransactionKeyPrefix(data.transaction_id, &txn_reverse_index_prefix);

  reverse_index_iter->Seek(txn_reverse_index_prefix.data());

  KeyValueWriteBatchPB put_batch;
  WriteBatch rocksdb_write_batch;

  while (reverse_index_iter->Valid()) {
    rocksdb::Slice key_slice(reverse_index_iter->key());

    if (!key_slice.starts_with(txn_reverse_index_prefix.data())) {
      break;
    }

    // If the key ends at the transaction id then it is transaction metadata (status tablet,
    // isolation level etc.).
    if (key_slice.size() > txn_reverse_index_prefix.size()) {
      // Value of reverse index is a key of original intent record, so seek it and check match.
      intent_iter->Seek(reverse_index_iter->value());
      if (intent_iter->Valid() && intent_iter->key() == reverse_index_iter->value()) {
        Slice intent_key(intent_iter->key());
        intent_key.consume_byte();
        auto intent_type = docdb::ExtractIntentType(
            intent_iter.get(), transaction_id_slice, &intent_key);
        RETURN_NOT_OK(intent_type);

        if (IsStrongIntent(*intent_type)) {
          Slice intent_value(intent_iter->value());
          INTENT_VALUE_SCHECK(intent_value[0], EQ, static_cast<uint8_t>(ValueType::kTransactionId),
                              "prefix expected");
          intent_value.consume_byte();
          INTENT_VALUE_SCHECK(intent_value.starts_with(transaction_id_slice), EQ, true,
                              "wrong transaction id");
          intent_value.remove_prefix(transaction_id_slice.size());

          auto* pair = put_batch.add_kv_pairs();
          // After strip of prefix and suffix intent_key contains just SubDocKey w/o a hybrid time.
          // Time will be added when writing batch to rocks db.
          pair->set_key(intent_key.cdata(), intent_key.size());
          pair->set_value(intent_value.cdata(), intent_value.size());
        }
        rocksdb_write_batch.Delete(intent_iter->key());
      } else {
        LOG(DFATAL) << "Unable to find intent: " << reverse_index_iter->value().ToDebugString()
                    << " for " << reverse_index_iter->key().ToDebugString();
      }
    }

    rocksdb_write_batch.Delete(reverse_index_iter->key());

    reverse_index_iter->Next();
  }

  // data.hybrid_time contains transaction commit time.
  // We don't set transaction field of put_batch, otherwise we would write another bunch of intents.
  // TODO(dtxn) commit_time?
  ApplyKeyValueRowOperations(put_batch, data.op_id, data.commit_time, &rocksdb_write_batch);
  return Status::OK();
}

Status Tablet::ReplaceMemRowSetUnlocked(RowSetsInCompaction *compaction,
                                        shared_ptr<MemRowSet> *old_ms) {
  if (table_type_ != TableType::KUDU_COLUMNAR_TABLE_TYPE) {
    return Status::OK();
  }
  *old_ms = components_->memrowset;
  // Mark the memrowset rowset as locked, so compactions won't consider it
  // for inclusion in any concurrent compactions.
  std::unique_lock<std::mutex> ms_lock(*(*old_ms)->compact_flush_lock(), std::try_to_lock);
  CHECK(ms_lock.owns_lock());

  // Add to compaction.
  compaction->AddRowSet(*old_ms, std::move(ms_lock));

  auto new_mrs = std::make_shared<MemRowSet>(next_mrs_id_++, *schema(), log_anchor_registry_.get(),
                                             mem_tracker_);
  auto new_rst = std::make_shared<RowSetTree>();
  ModifyRowSetTree(*components_->rowsets,
                   RowSetVector(),  // remove nothing
                   { *old_ms },  // add the old MRS
                   new_rst.get());

  // Swap it in
  components_ = new TabletComponents(new_mrs, new_rst);
  return Status::OK();
}

Status Tablet::FlushInternal(const RowSetsInCompaction& input,
                             const shared_ptr<MemRowSet>& old_ms) {
  if (table_type_ != TableType::KUDU_COLUMNAR_TABLE_TYPE) {
    return Status::OK();
  }

  CHECK(state_ == kOpen || state_ == kBootstrapping);

  // Step 1. Freeze the old memrowset by blocking readers and swapping
  // it in as a new rowset, replacing it with an empty one.
  //
  // At this point, we have already swapped in a new empty rowset, and
  // any new inserts are going into that one. 'old_ms' is effectively
  // frozen -- no new inserts should arrive after this point.
  //
  // NOTE: updates and deletes may still arrive into 'old_ms' at this point.
  //
  // TODO(perf): there's a memrowset.Freeze() call which we might be able to
  // use to improve iteration performance during the flush. The old design
  // used this, but not certain whether it's still doable with the new design.

  uint64_t start_insert_count = old_ms->debug_insert_count();
  int64_t mrs_being_flushed = old_ms->mrs_id();

  if (flush_hooks_) {
    RETURN_NOT_OK_PREPEND(flush_hooks_->PostSwapNewMemRowSet(),
                          "PostSwapNewMemRowSet hook failed");
  }

  LOG(INFO) << "Flush: entering stage 1 (old memrowset already frozen for inserts)";
  input.DumpToLog();
  LOG(INFO) << "Memstore in-memory size: " << old_ms->memory_footprint() << " bytes";

  RETURN_NOT_OK(DoCompactionOrFlush(input, mrs_being_flushed));

  // Sanity check that no insertions happened during our flush.
  CHECK_EQ(start_insert_count, old_ms->debug_insert_count())
    << "Sanity check failed: insertions continued in memrowset "
    << "after flush was triggered! Aborting to prevent dataloss.";

  return Status::OK();
}

Status Tablet::CreatePreparedAlterSchema(AlterSchemaOperationState *operation_state,
                                         const Schema* schema) {
  if (!key_schema_.KeyEquals(*schema)) {
    return STATUS(InvalidArgument, "Schema keys cannot be altered",
                  schema->CreateKeyProjection().ToString());
  }

  if (!schema->has_column_ids()) {
    // this probably means that the request is not from the Master
    return STATUS(InvalidArgument, "Missing Column IDs");
  }

  // Alter schema must run when no reads/writes are in progress.
  // However, compactions and flushes can continue to run in parallel
  // with the schema change,
  operation_state->AcquireSchemaLock(&schema_lock_);

  operation_state->set_schema(schema);
  return Status::OK();
}

Status Tablet::AlterSchema(AlterSchemaOperationState *operation_state) {
  DCHECK(key_schema_.KeyEquals(*DCHECK_NOTNULL(operation_state->schema())))
      << "Schema keys cannot be altered";

  // Prevent any concurrent flushes. Otherwise, we run into issues where
  // we have an MRS in the rowset tree, and we can't alter its schema
  // in-place.
  std::lock_guard<Semaphore> lock(rowsets_flush_sem_);

  {
    bool same_schema = schema()->Equals(*operation_state->schema());

    // If the current version >= new version, there is nothing to do.
    if (metadata_->schema_version() >= operation_state->schema_version()) {
      LOG(INFO) << "Already running schema version " << metadata_->schema_version()
                << " got alter request for version " << operation_state->schema_version();
      return Status::OK();
    }

    LOG(INFO) << "Alter schema from " << schema()->ToString()
              << " version " << metadata_->schema_version()
              << " to " << operation_state->schema()->ToString()
              << " version " << operation_state->schema_version();
    DCHECK(schema_lock_.is_locked());

    // Find out which columns have been deleted in this schema change, and add them to metadata.
    for (const auto& col : schema()->column_ids()) {
      if (operation_state->schema()->find_column_by_id(col) == Schema::kColumnNotFound) {
        DeletedColumn deleted_col(col, clock_->Now());
        LOG(INFO) << "Column " << col.ToString() << " recorded as deleted.";
        metadata_->AddDeletedColumn(deleted_col);
      }
    }

    metadata_->SetSchema(*operation_state->schema(), operation_state->schema_version());
    if (operation_state->has_new_table_name()) {
      metadata_->SetTableName(operation_state->new_table_name());
      if (metric_entity_) {
        metric_entity_->SetAttribute("table_name", operation_state->new_table_name());
      }
    }

    // If the current schema and the new one are equal, there is nothing to do.
    if (same_schema) {
      return metadata_->Flush();
    }
  }

  // The rest of this function is old Kudu code, which will be deleted.
  // Replace the MemRowSet.
  RowSetsInCompaction input;
  shared_ptr<MemRowSet> old_ms;
  if (table_type() == TableType::KUDU_COLUMNAR_TABLE_TYPE) {
    std::lock_guard <rw_spinlock> lock(component_lock_);
    RETURN_NOT_OK(ReplaceMemRowSetUnlocked(&input, &old_ms));
  }

  // TODO(KUDU-915): ideally we would release the schema_lock here so that
  // we don't block access to the tablet while we flush the MRS.
  // However, doing so opens up some subtle issues with the ordering of
  // the alter's COMMIT message against the COMMIT messages of other
  // writes. A "big hammer" fix has been applied here to hold the lock
  // all the way until the COMMIT message has been appended to the WAL.

  // Flush the old MemRowSet.
  if (table_type() == TableType::KUDU_COLUMNAR_TABLE_TYPE) {
    return FlushInternal(input, old_ms);
  }
  return Status::OK();
}

Status Tablet::RewindSchemaForBootstrap(const Schema& new_schema,
                                        int64_t schema_version) {
  if (table_type_ != TableType::KUDU_COLUMNAR_TABLE_TYPE) {
    return Status::OK();
  }
  CHECK_EQ(state_, kBootstrapping);

  // We know that the MRS should be empty at this point, because we
  // rewind the schema before replaying any operations. So, we just
  // swap in a new one with the correct schema, rather than attempting
  // to flush.
  LOG(INFO) << "Rewinding schema during bootstrap to " << new_schema.ToString();

  metadata_->SetSchema(new_schema, schema_version);
  {
    std::lock_guard<rw_spinlock> lock(component_lock_);

    shared_ptr<MemRowSet> old_mrs = components_->memrowset;
    shared_ptr<RowSetTree> old_rowsets = components_->rowsets;
    CHECK(old_mrs->empty());
    int64_t old_mrs_id = old_mrs->mrs_id();
    // We have to reset the components here before creating the new MemRowSet,
    // or else the new MRS will end up trying to claim the same MemTracker ID
    // as the old one.
    components_.reset();
    old_mrs.reset();
    shared_ptr<MemRowSet> new_mrs(new MemRowSet(old_mrs_id, new_schema,
                                                log_anchor_registry_.get(), mem_tracker_));
    components_ = new TabletComponents(new_mrs, old_rowsets);
  }
  return Status::OK();
}

void Tablet::UpdateMonotonicCounter(int64_t value) {
  int64_t counter = monotonic_counter_;
  while (true) {
    if (counter >= value) {
      break;
    }
    if (monotonic_counter_.compare_exchange_weak(counter, value)) {
      break;
    }
  }
}


void Tablet::SetCompactionHooksForTests(
    const shared_ptr<Tablet::CompactionFaultHooks> &hooks) {
  compaction_hooks_ = hooks;
}

void Tablet::SetFlushHooksForTests(
    const shared_ptr<Tablet::FlushFaultHooks> &hooks) {
  flush_hooks_ = hooks;
}

void Tablet::SetFlushCompactCommonHooksForTests(
    const shared_ptr<Tablet::FlushCompactCommonHooks> &hooks) {
  common_hooks_ = hooks;
}

int32_t Tablet::CurrentMrsIdForTests() const {
  shared_lock<rw_spinlock> lock(component_lock_);
  return components_->memrowset->mrs_id();
}

////////////////////////////////////////////////////////////
// CompactRowSetsOp
////////////////////////////////////////////////////////////

CompactRowSetsOp::CompactRowSetsOp(Tablet* tablet)
    : MaintenanceOp(Substitute("CompactRowSetsOp($0)", tablet->tablet_id()),
                    MaintenanceOp::HIGH_IO_USAGE),
      last_num_mrs_flushed_(0),
      last_num_rs_compacted_(0),
      tablet_(tablet) {
}

void CompactRowSetsOp::UpdateStats(MaintenanceOpStats* stats) {
  std::lock_guard<simple_spinlock> l(lock_);

  // Any operation that changes the on-disk row layout invalidates the
  // cached stats.
  TabletMetrics* metrics = tablet_->metrics();
  if (metrics) {
    uint64_t new_num_mrs_flushed = metrics->flush_mrs_duration->TotalCount();
    uint64_t new_num_rs_compacted = metrics->compact_rs_duration->TotalCount();
    if (prev_stats_.valid() &&
        new_num_mrs_flushed == last_num_mrs_flushed_ &&
        new_num_rs_compacted == last_num_rs_compacted_) {
      *stats = prev_stats_;
      return;
    } else {
      last_num_mrs_flushed_ = new_num_mrs_flushed;
      last_num_rs_compacted_ = new_num_rs_compacted;
    }
  }

  tablet_->UpdateCompactionStats(&prev_stats_);
  *stats = prev_stats_;
}

bool CompactRowSetsOp::Prepare() {
  std::lock_guard<simple_spinlock> l(lock_);
  // Invalidate the cached stats so that another section of the tablet can
  // be compacted concurrently.
  //
  // TODO: we should acquire the rowset compaction locks here. Otherwise, until
  // Compact() acquires them, the maintenance manager may compute the same
  // stats for this op and run it again, even though Perform() will end up
  // performing a much less fruitful compaction. See KUDU-790 for more details.
  prev_stats_.Clear();
  return true;
}

void CompactRowSetsOp::Perform() {
  WARN_NOT_OK(tablet_->Compact(Tablet::COMPACT_NO_FLAGS),
              Substitute("Compaction failed on $0", tablet_->tablet_id()));
}

scoped_refptr<Histogram> CompactRowSetsOp::DurationHistogram() const {
  return tablet_->metrics()->compact_rs_duration;
}

scoped_refptr<AtomicGauge<uint32_t> > CompactRowSetsOp::RunningGauge() const {
  return tablet_->metrics()->compact_rs_running;
}

////////////////////////////////////////////////////////////
// MinorDeltaCompactionOp
////////////////////////////////////////////////////////////

MinorDeltaCompactionOp::MinorDeltaCompactionOp(Tablet* tablet)
    : MaintenanceOp(Substitute("MinorDeltaCompactionOp($0)", tablet->tablet_id()),
                    MaintenanceOp::HIGH_IO_USAGE),
      last_num_mrs_flushed_(0),
      last_num_dms_flushed_(0),
      last_num_rs_compacted_(0),
      last_num_rs_minor_delta_compacted_(0),
      tablet_(tablet) {
}

void MinorDeltaCompactionOp::UpdateStats(MaintenanceOpStats* stats) {
  std::lock_guard<simple_spinlock> l(lock_);

  // Any operation that changes the number of REDO files invalidates the
  // cached stats.
  TabletMetrics* metrics = tablet_->metrics();
  if (metrics) {
    uint64_t new_num_mrs_flushed = metrics->flush_mrs_duration->TotalCount();
    uint64_t new_num_dms_flushed = metrics->flush_dms_duration->TotalCount();
    uint64_t new_num_rs_compacted = metrics->compact_rs_duration->TotalCount();
    uint64_t new_num_rs_minor_delta_compacted =
        metrics->delta_minor_compact_rs_duration->TotalCount();
    if (prev_stats_.valid() &&
        new_num_mrs_flushed == last_num_mrs_flushed_ &&
        new_num_dms_flushed == last_num_dms_flushed_ &&
        new_num_rs_compacted == last_num_rs_compacted_ &&
        new_num_rs_minor_delta_compacted == last_num_rs_minor_delta_compacted_) {
      *stats = prev_stats_;
      return;
    } else {
      last_num_mrs_flushed_ = new_num_mrs_flushed;
      last_num_dms_flushed_ = new_num_dms_flushed;
      last_num_rs_compacted_ = new_num_rs_compacted;
      last_num_rs_minor_delta_compacted_ = new_num_rs_minor_delta_compacted;
    }
  }

  double perf_improv = tablet_->GetPerfImprovementForBestDeltaCompact(
      RowSet::MINOR_DELTA_COMPACTION, nullptr);
  prev_stats_.set_perf_improvement(perf_improv);
  prev_stats_.set_runnable(perf_improv > 0);
  *stats = prev_stats_;
}

bool MinorDeltaCompactionOp::Prepare() {
  std::lock_guard<simple_spinlock> l(lock_);
  // Invalidate the cached stats so that another rowset in the tablet can
  // be delta compacted concurrently.
  //
  // TODO: See CompactRowSetsOp::Prepare().
  prev_stats_.Clear();
  return true;
}

void MinorDeltaCompactionOp::Perform() {
  WARN_NOT_OK(tablet_->CompactWorstDeltas(RowSet::MINOR_DELTA_COMPACTION),
              Substitute("Minor delta compaction failed on $0", tablet_->tablet_id()));
}

scoped_refptr<Histogram> MinorDeltaCompactionOp::DurationHistogram() const {
  return tablet_->metrics()->delta_minor_compact_rs_duration;
}

scoped_refptr<AtomicGauge<uint32_t> > MinorDeltaCompactionOp::RunningGauge() const {
  return tablet_->metrics()->delta_minor_compact_rs_running;
}

////////////////////////////////////////////////////////////
// MajorDeltaCompactionOp
////////////////////////////////////////////////////////////

MajorDeltaCompactionOp::MajorDeltaCompactionOp(Tablet* tablet)
    : MaintenanceOp(Substitute("MajorDeltaCompactionOp($0)", tablet->tablet_id()),
                    MaintenanceOp::HIGH_IO_USAGE),
      last_num_mrs_flushed_(0),
      last_num_dms_flushed_(0),
      last_num_rs_compacted_(0),
      last_num_rs_minor_delta_compacted_(0),
      last_num_rs_major_delta_compacted_(0),
      tablet_(tablet) {
}

void MajorDeltaCompactionOp::UpdateStats(MaintenanceOpStats* stats) {
  std::lock_guard<simple_spinlock> l(lock_);

  // Any operation that changes the size of the on-disk data invalidates the
  // cached stats.
  TabletMetrics* metrics = tablet_->metrics();
  if (metrics) {
    int64_t new_num_mrs_flushed = metrics->flush_mrs_duration->TotalCount();
    int64_t new_num_dms_flushed = metrics->flush_dms_duration->TotalCount();
    int64_t new_num_rs_compacted = metrics->compact_rs_duration->TotalCount();
    int64_t new_num_rs_minor_delta_compacted =
        metrics->delta_minor_compact_rs_duration->TotalCount();
    int64_t new_num_rs_major_delta_compacted =
        metrics->delta_major_compact_rs_duration->TotalCount();
    if (prev_stats_.valid() &&
        new_num_mrs_flushed == last_num_mrs_flushed_ &&
        new_num_dms_flushed == last_num_dms_flushed_ &&
        new_num_rs_compacted == last_num_rs_compacted_ &&
        new_num_rs_minor_delta_compacted == last_num_rs_minor_delta_compacted_ &&
        new_num_rs_major_delta_compacted == last_num_rs_major_delta_compacted_) {
      *stats = prev_stats_;
      return;
    } else {
      last_num_mrs_flushed_ = new_num_mrs_flushed;
      last_num_dms_flushed_ = new_num_dms_flushed;
      last_num_rs_compacted_ = new_num_rs_compacted;
      last_num_rs_minor_delta_compacted_ = new_num_rs_minor_delta_compacted;
      last_num_rs_major_delta_compacted_ = new_num_rs_major_delta_compacted;
    }
  }

  double perf_improv = tablet_->GetPerfImprovementForBestDeltaCompact(
      RowSet::MAJOR_DELTA_COMPACTION, nullptr);
  prev_stats_.set_perf_improvement(perf_improv);
  prev_stats_.set_runnable(perf_improv > 0);
  *stats = prev_stats_;
}

bool MajorDeltaCompactionOp::Prepare() {
  std::lock_guard<simple_spinlock> l(lock_);
  // Invalidate the cached stats so that another rowset in the tablet can
  // be delta compacted concurrently.
  //
  // TODO: See CompactRowSetsOp::Prepare().
  prev_stats_.Clear();
  return true;
}

void MajorDeltaCompactionOp::Perform() {
  WARN_NOT_OK(tablet_->CompactWorstDeltas(RowSet::MAJOR_DELTA_COMPACTION),
              Substitute("Major delta compaction failed on $0", tablet_->tablet_id()));
}

scoped_refptr<Histogram> MajorDeltaCompactionOp::DurationHistogram() const {
  return tablet_->metrics()->delta_major_compact_rs_duration;
}

scoped_refptr<AtomicGauge<uint32_t> > MajorDeltaCompactionOp::RunningGauge() const {
  return tablet_->metrics()->delta_major_compact_rs_running;
}

////////////////////////////////////////////////////////////
// Tablet
////////////////////////////////////////////////////////////

Status Tablet::PickRowSetsToCompact(RowSetsInCompaction *picked,
                                    CompactFlags flags) const {
  CHECK_EQ(state_, kOpen);
  // Grab a local reference to the current RowSetTree. This is to avoid
  // holding the component_lock_ for too long. See the comment on component_lock_
  // in tablet.h for details on why that would be bad.
  shared_ptr<RowSetTree> rowsets_copy;
  {
    shared_lock<rw_spinlock> lock(component_lock_);
    rowsets_copy = components_->rowsets;
  }

  std::lock_guard<std::mutex> compact_lock(compact_select_lock_);
  CHECK_EQ(picked->num_rowsets(), 0);

  unordered_set<RowSet*> picked_set;

  if (flags & FORCE_COMPACT_ALL) {
    // Compact all rowsets, regardless of policy.
    for (const shared_ptr<RowSet>& rs : rowsets_copy->all_rowsets()) {
      if (rs->IsAvailableForCompaction()) {
        picked_set.insert(rs.get());
      }
    }
  } else {
    // Let the policy decide which rowsets to compact.
    double quality = 0;
    RETURN_NOT_OK(compaction_policy_->PickRowSets(*rowsets_copy, &picked_set, &quality, NULL));
    VLOG(2) << "Compaction quality: " << quality;
  }

  shared_lock<rw_spinlock> lock(component_lock_);
  for (const shared_ptr<RowSet>& rs : components_->rowsets->all_rowsets()) {
    if (picked_set.erase(rs.get()) == 0) {
      // Not picked.
      continue;
    }

    // Grab the compact_flush_lock: this prevents any other concurrent
    // compaction from selecting this same rowset, and also ensures that
    // we don't select a rowset which is currently in the middle of being
    // flushed.
    std::unique_lock<std::mutex> lock(*rs->compact_flush_lock(), std::try_to_lock);
    CHECK(lock.owns_lock()) << rs->ToString() << " appeared available for "
        "compaction when inputs were selected, but was unable to lock its "
        "compact_flush_lock to prepare for compaction.";

    // Push the lock on our scoped list, so we unlock when done.
    picked->AddRowSet(rs, std::move(lock));
  }

  // When we iterated through the current rowsets, we should have found all of the
  // rowsets that we picked. If we didn't, that implies that some other thread swapped
  // them out while we were making our selection decision -- that's not possible
  // since we only picked rowsets that were marked as available for compaction.
  if (!picked_set.empty()) {
    for (const RowSet* not_found : picked_set) {
      LOG(ERROR) << "Rowset selected for compaction but not available anymore: "
                 << not_found->ToString();
    }
    LOG(FATAL) << "Was unable to find all rowsets selected for compaction";
  }
  return Status::OK();
}

void Tablet::GetRowSetsForTests(RowSetVector* out) {
  if (table_type_ != TableType::KUDU_COLUMNAR_TABLE_TYPE) {
    out->clear();
    return;
  }

  shared_ptr<RowSetTree> rowsets_copy;
  {
    shared_lock<rw_spinlock> lock(component_lock_);
    rowsets_copy = components_->rowsets;
  }
  for (const shared_ptr<RowSet>& rs : rowsets_copy->all_rowsets()) {
    out->push_back(rs);
  }
}

void Tablet::RegisterMaintenanceOps(MaintenanceManager* maint_mgr) {
  if (table_type_ != TableType::KUDU_COLUMNAR_TABLE_TYPE) {
    return;
  }

  CHECK_EQ(state_, kOpen);
  DCHECK(maintenance_ops_.empty());

  gscoped_ptr<MaintenanceOp> rs_compact_op(new CompactRowSetsOp(this));
  maint_mgr->RegisterOp(rs_compact_op.get());
  maintenance_ops_.push_back(rs_compact_op.release());

  gscoped_ptr<MaintenanceOp> minor_delta_compact_op(new MinorDeltaCompactionOp(this));
  maint_mgr->RegisterOp(minor_delta_compact_op.get());
  maintenance_ops_.push_back(minor_delta_compact_op.release());

  gscoped_ptr<MaintenanceOp> major_delta_compact_op(new MajorDeltaCompactionOp(this));
  maint_mgr->RegisterOp(major_delta_compact_op.get());
  maintenance_ops_.push_back(major_delta_compact_op.release());
}

void Tablet::UnregisterMaintenanceOps() {
  for (MaintenanceOp* op : maintenance_ops_) {
    op->Unregister();
  }
  STLDeleteElements(&maintenance_ops_);
}

bool Tablet::HasSSTables() const {
  DCHECK_NE(table_type_, TableType::KUDU_COLUMNAR_TABLE_TYPE);
  std::vector<rocksdb::LiveFileMetaData> live_files_metadata;
  rocksdb_->GetLiveFilesMetaData(&live_files_metadata);
  return !live_files_metadata.empty();
}

yb::OpId Tablet::MaxPersistentOpId() const {
  DCHECK_NE(table_type_, TableType::KUDU_COLUMNAR_TABLE_TYPE);
  return rocksdb_->GetFlushedOpId();
}

Status Tablet::FlushMetadata(const RowSetVector& to_remove,
                             const RowSetMetadataVector& to_add,
                             int64_t mrs_being_flushed) {
  if (table_type_ != TableType::KUDU_COLUMNAR_TABLE_TYPE) {
    return metadata_->Flush();
  }
  RowSetMetadataIds to_remove_meta;
  for (const shared_ptr<RowSet>& rowset : to_remove) {
    // Skip MemRowSet & DuplicatingRowSets which don't have metadata.
    if (rowset->metadata().get() == nullptr) {
      continue;
    }
    to_remove_meta.insert(rowset->metadata()->id());
  }

  return metadata_->UpdateAndFlush(to_remove_meta, to_add, mrs_being_flushed);
}

Status Tablet::DoCompactionOrFlush(const RowSetsInCompaction &input, int64_t mrs_being_flushed) {
  if (table_type_ != TableType::KUDU_COLUMNAR_TABLE_TYPE) {
    return Status::OK();
  }

  const char *op_name =
      (mrs_being_flushed == TabletMetadata::kNoMrsFlushed) ? "Compaction" : "Flush";
  TRACE_EVENT2("tablet", "Tablet::DoCompactionOrFlush",
               "tablet_id", tablet_id(),
               "op", op_name);

  MvccSnapshot flush_snap(mvcc_);
  LOG(INFO) << op_name << ": entering phase 1 (flushing snapshot). Phase 1 snapshot: "
            << flush_snap.ToString();

  if (common_hooks_) {
    RETURN_NOT_OK_PREPEND(common_hooks_->PostTakeMvccSnapshot(),
                          "PostTakeMvccSnapshot hook failed");
  }

  shared_ptr<CompactionInput> merge;
  RETURN_NOT_OK(input.CreateCompactionInput(flush_snap, schema(), &merge));

  RollingDiskRowSetWriter drsw(metadata_.get(), merge->schema(), bloom_sizing(),
                               compaction_policy_->target_rowset_size());
  RETURN_NOT_OK_PREPEND(drsw.Open(), "Failed to open DiskRowSet for flush");
  RETURN_NOT_OK_PREPEND(FlushCompactionInput(merge.get(), flush_snap, &drsw),
                        "Flush to disk failed");
  RETURN_NOT_OK_PREPEND(drsw.Finish(), "Failed to finish DRS writer");

  if (common_hooks_) {
    RETURN_NOT_OK_PREPEND(common_hooks_->PostWriteSnapshot(),
                          "PostWriteSnapshot hook failed");
  }

  // Though unlikely, it's possible that all of the input rows were actually
  // GCed in this compaction. In that case, we don't actually want to reopen.
  bool gced_all_input = drsw.written_count() == 0;
  if (gced_all_input) {
    LOG(INFO) << op_name << " resulted in no output rows (all input rows "
              << "were GCed!)  Removing all input rowsets.";

    // Write out the new Tablet Metadata and remove old rowsets.
    // TODO: Consensus catch-up may want to preserve the compaction inputs.
    RETURN_NOT_OK_PREPEND(FlushMetadata(input.rowsets(),
                                        RowSetMetadataVector(),
                                        mrs_being_flushed),
                          "Failed to flush new tablet metadata");

    AtomicSwapRowSets(input.rowsets(), RowSetVector());

    return Status::OK();
  }

  // The RollingDiskRowSet writer wrote out one or more RowSets as the
  // output. Open these into 'new_rowsets'.
  vector<shared_ptr<RowSet> > new_disk_rowsets;
  RowSetMetadataVector new_drs_metas;
  drsw.GetWrittenRowSetMetadata(&new_drs_metas);

  if (metrics_.get()) metrics_->bytes_flushed->IncrementBy(drsw.written_size());
  CHECK(!new_drs_metas.empty());
  {
    TRACE_EVENT0("tablet", "Opening compaction results");
    for (const shared_ptr<RowSetMetadata>& meta : new_drs_metas) {
      shared_ptr<DiskRowSet> new_rowset;
      Status s = DiskRowSet::Open(meta, log_anchor_registry_.get(), &new_rowset, mem_tracker_);
      if (!s.ok()) {
        LOG(WARNING) << "Unable to open snapshot " << op_name << " results "
                     << meta->ToString() << ": " << s.ToString();
        return s;
      }
      new_disk_rowsets.push_back(new_rowset);
    }
  }

  // Setup for Phase 2: Start duplicating any new updates into the new on-disk
  // rowsets.
  //
  // During Phase 1, we may have missed some updates which came into the input
  // rowsets while we were writing. So, we can't immediately start reading from
  // the on-disk rowsets alone. Starting here, we continue to read from the
  // original rowset(s), but mirror updates to both the input and the output
  // data.
  //
  // It's crucial that, during the rest of the compaction, we do not allow the
  // output rowsets to flush their deltas to disk. This is to avoid the following
  // bug:
  // - during phase 1, hybrid_time 1 updates a flushed row. This is only reflected in the
  //   input rowset. (ie it is a "missed delta")
  // - during phase 2, hybrid_time 2 updates the same row. This is reflected in both the
  //   input and output, because of the DuplicatingRowSet.
  // - now suppose the output rowset were allowed to flush deltas. This would create the
  //   first DeltaFile for the output rowset, with only hybrid_time 2.
  // - Now we run the "ReupdateMissedDeltas", and copy over the first transaction to the output
  //   DMS, which later flushes.
  // The end result would be that redos[0] has hybrid_time 2, and redos[1] has hybrid_time 1.
  // This breaks an invariant that the redo files are time-ordered, and would we would probably
  // reapply the deltas in the wrong order on the read path.
  //
  // The way that we avoid this case is that DuplicatingRowSet's FlushDeltas method is a
  // no-op.
  LOG(INFO) << op_name << ": entering phase 2 (starting to duplicate updates "
            << "in new rowsets)";
  shared_ptr<DuplicatingRowSet> inprogress_rowset(
      new DuplicatingRowSet(input.rowsets(), new_disk_rowsets));

  // The next step is to swap in the DuplicatingRowSet, and at the same time, determine an
  // MVCC snapshot which includes all of the transactions that saw a pre-DuplicatingRowSet
  // version of components_.
  MvccSnapshot non_duplicated_txns_snap;
  vector<HybridTime> applying_during_swap;
  {
    TRACE_EVENT0("tablet", "Swapping DuplicatingRowSet");
    // Taking component_lock_ in write mode ensures that no new transactions
    // can StartApplying() (or snapshot components_) during this block.
    std::lock_guard<rw_spinlock> lock(component_lock_);
    AtomicSwapRowSetsUnlocked(input.rowsets(), { inprogress_rowset });

    // NOTE: transactions may *commit* in between these two lines.
    // We need to make sure all such transactions end up in the
    // 'applying_during_swap' list, the 'non_duplicated_txns_snap' snapshot,
    // or both. Thus it's crucial that these next two lines are in this order!
    mvcc_.GetApplyingOperationsHybridTimes(&applying_during_swap);
    non_duplicated_txns_snap = MvccSnapshot(mvcc_);
  }

  // All transactions committed in 'non_duplicated_txns_snap' saw the pre-swap components_.
  // Additionally, any transactions that were APPLYING during the above block by definition
  // _started_ doing so before the swap. Hence those transactions also need to get included in
  // non_duplicated_txns_snap. To do so, we wait for them to commit, and then
  // manually include them into our snapshot.
  if (VLOG_IS_ON(1) && !applying_during_swap.empty()) {
    VLOG(1) << "Waiting for " << applying_during_swap.size() << " mid-APPLY txns to commit "
            << "before finishing compaction...";
    for (const HybridTime& ht : applying_during_swap) {
      VLOG(1) << "  " << ht.value();
    }
  }

  // This wait is a little bit conservative - technically we only need to wait for
  // those transactions in 'applying_during_swap', but MVCC doesn't implement the
  // ability to wait for a specific set. So instead we wait for all currently applying --
  // a bit more than we need, but still correct.
  mvcc_.WaitForApplyingOperationsToCommit();

  // Then we want to consider all those transactions that were in-flight when we did the
  // swap as committed in 'non_duplicated_txns_snap'.
  non_duplicated_txns_snap.AddCommittedHybridTimes(applying_during_swap);

  if (common_hooks_) {
    RETURN_NOT_OK_PREPEND(common_hooks_->PostSwapInDuplicatingRowSet(),
                          "PostSwapInDuplicatingRowSet hook failed");
  }

  // Phase 2. Here we re-scan the compaction input, copying those missed updates into the
  // new rowset's DeltaTracker.
  LOG(INFO) << op_name << " Phase 2: carrying over any updates which arrived during Phase 1";
  LOG(INFO) << "Phase 2 snapshot: " << non_duplicated_txns_snap.ToString();
  RETURN_NOT_OK_PREPEND(
      input.CreateCompactionInput(non_duplicated_txns_snap, schema(), &merge),
      Substitute("Failed to create $0 inputs", op_name).c_str());

  // Update the output rowsets with the deltas that came in in phase 1, before we swapped
  // in the DuplicatingRowSets. This will perform a flush of the updated DeltaTrackers
  // in the end so that the data that is reported in the log as belonging to the input
  // rowsets is flushed.
  RETURN_NOT_OK_PREPEND(ReupdateMissedDeltas(metadata_->tablet_id(),
                                             merge.get(),
                                             flush_snap,
                                             non_duplicated_txns_snap,
                                             new_disk_rowsets),
                        Substitute("Failed to re-update deltas missed during $0 phase 1",
                                   op_name).c_str());

  if (common_hooks_) {
    RETURN_NOT_OK_PREPEND(common_hooks_->PostReupdateMissedDeltas(),
                          "PostReupdateMissedDeltas hook failed");
  }

  // ------------------------------
  // Flush was successful.

  // Write out the new Tablet Metadata and remove old rowsets.
  RETURN_NOT_OK_PREPEND(FlushMetadata(input.rowsets(), new_drs_metas, mrs_being_flushed),
                        "Failed to flush new tablet metadata");

  // Replace the compacted rowsets with the new on-disk rowsets, making them visible now that
  // their metadata was written to disk.
  AtomicSwapRowSets({ inprogress_rowset }, new_disk_rowsets);

  LOG(INFO) << op_name << " successful on " << drsw.written_count()
            << " rows " << "(" << drsw.written_size() << " bytes)";

  if (common_hooks_) {
    RETURN_NOT_OK_PREPEND(common_hooks_->PostSwapNewRowSet(),
                          "PostSwapNewRowSet hook failed");
  }

  return Status::OK();
}

Status Tablet::Compact(CompactFlags flags) {
  CHECK_EQ(state_, kOpen);

  if (table_type_ != TableType::KUDU_COLUMNAR_TABLE_TYPE) {
    // TODO: Add calls into RocksDB compaction.
    return Status::OK();
  }

  RowSetsInCompaction input;
  // Step 1. Capture the rowsets to be merged
  RETURN_NOT_OK_PREPEND(PickRowSetsToCompact(&input, flags),
                        "Failed to pick rowsets to compact");
  if (input.num_rowsets() < 2) {
    VLOG(1) << "Not enough rowsets to run compaction! Aborting...";
    return Status::OK();
  }
  LOG(INFO) << "Compaction: stage 1 complete, picked "
            << input.num_rowsets() << " rowsets to compact";
  if (compaction_hooks_) {
    RETURN_NOT_OK_PREPEND(compaction_hooks_->PostSelectIterators(),
                          "PostSelectIterators hook failed");
  }

  input.DumpToLog();

  return DoCompactionOrFlush(input,
                             TabletMetadata::kNoMrsFlushed);
}

void Tablet::UpdateCompactionStats(MaintenanceOpStats* stats) {
  if (table_type_ != TableType::KUDU_COLUMNAR_TABLE_TYPE) {
    return;
  }

  // TODO: use workload statistics here to find out how "hot" the tablet has
  // been in the last 5 minutes, and somehow scale the compaction quality
  // based on that, so we favor hot tablets.
  double quality = 0;
  unordered_set<RowSet*> picked_set_ignored;

  shared_ptr<RowSetTree> rowsets_copy;
  {
    shared_lock<rw_spinlock> lock(component_lock_);
    rowsets_copy = components_->rowsets;
  }

  {
    std::lock_guard<std::mutex> compact_lock(compact_select_lock_);
    WARN_NOT_OK(compaction_policy_->PickRowSets(*rowsets_copy, &picked_set_ignored, &quality, NULL),
                Substitute("Couldn't determine compaction quality for $0", tablet_id()));
  }

  VLOG(1) << "Best compaction for " << tablet_id() << ": " << quality;

  stats->set_runnable(quality >= 0);
  stats->set_perf_improvement(quality);
}

Status Tablet::DebugDump(vector<string> *lines) {
  switch (table_type_) {
    case TableType::KUDU_COLUMNAR_TABLE_TYPE:
      return KuduDebugDump(lines);
    case TableType::YQL_TABLE_TYPE:
    case TableType::REDIS_TABLE_TYPE:
      DocDBDebugDump(lines);
      return Status::OK();
  }
  FATAL_INVALID_ENUM_VALUE(TableType, table_type_);
}

Status Tablet::KuduDebugDump(vector<string> *lines) {
  shared_lock<rw_spinlock> lock(component_lock_);

  LOG_STRING(INFO, lines) << "Dumping tablet:";
  LOG_STRING(INFO, lines) << "---------------------------";

  LOG_STRING(INFO, lines) << "MRS " << components_->memrowset->ToString() << ":";
  RETURN_NOT_OK(components_->memrowset->DebugDump(lines));

  for (const shared_ptr<RowSet> &rs : components_->rowsets->all_rowsets()) {
    LOG_STRING(INFO, lines) << "RowSet " << rs->ToString() << ":";
    RETURN_NOT_OK(rs->DebugDump(lines));
  }

  return Status::OK();
}

void Tablet::DocDBDebugDump(vector<string> *lines) {
  LOG_STRING(INFO, lines) << "Dumping tablet:";
  LOG_STRING(INFO, lines) << "---------------------------";
  yb::docdb::DocDBDebugDump(rocksdb_.get(), LOG_STRING(INFO, lines));
}

Status Tablet::CaptureConsistentIterators(
    const Schema *projection,
    const MvccSnapshot &snap,
    const ScanSpec *spec,
    const boost::optional<TransactionId>& transaction_id,
    vector<shared_ptr<RowwiseIterator> > *iters) const {

  switch (table_type_) {
    case TableType::KUDU_COLUMNAR_TABLE_TYPE:
      return KuduColumnarCaptureConsistentIterators(projection, snap, spec, iters);
    case TableType::YQL_TABLE_TYPE:
      return QLCaptureConsistentIterators(projection, snap, spec, transaction_id, iters);
    default:
      LOG(FATAL) << __FUNCTION__ << " is undefined for table type " << table_type_;
  }
  return STATUS(IllegalState, "This should never happen");
}

Status Tablet::QLCaptureConsistentIterators(
    const Schema *projection,
    const MvccSnapshot &snap,
    const ScanSpec *spec,
    const boost::optional<TransactionId>& transaction_id,
    vector<shared_ptr<RowwiseIterator> > *iters) const {
  GUARD_AGAINST_ROCKSDB_SHUTDOWN;

  TransactionOperationContextOpt txn_op_ctx =
      CreateTransactionOperationContext(transaction_id);
  iters->clear();
  iters->push_back(std::make_shared<DocRowwiseIterator>(
      *projection, *schema(), txn_op_ctx, rocksdb_.get(), snap.LastCommittedHybridTime(),
      // We keep the pending operation counter incremented while the iterator exists so that
      // RocksDB does not get deallocated while we're using it.
      &pending_op_counter_));
  return Status::OK();
}

Status Tablet::KuduColumnarCaptureConsistentIterators(
    const Schema *projection,
    const MvccSnapshot &snap,
    const ScanSpec *spec,
    vector<shared_ptr<RowwiseIterator> > *iters) const {
  shared_lock<rw_spinlock> lock(component_lock_);
  // Construct all the iterators locally first, so that if we fail
  // in the middle, we don't modify the output arguments.
  vector<shared_ptr<RowwiseIterator> > ret;

  // Grab the memrowset iterator.
  gscoped_ptr<RowwiseIterator> ms_iter;
  RETURN_NOT_OK(components_->memrowset->NewRowIterator(projection, snap, &ms_iter));
  ret.push_back(shared_ptr<RowwiseIterator>(ms_iter.release()));

  // Cull row-sets in the case of key-range queries.
  if (spec != nullptr && spec->lower_bound_key() && spec->exclusive_upper_bound_key()) {
    // TODO : support open-ended intervals
    // TODO: the upper bound key is exclusive, but the RowSetTree function takes
    // an inclusive interval. So, we might end up fetching one more rowset than
    // necessary.
    vector<RowSet *> interval_sets;
    components_->rowsets->FindRowSetsIntersectingInterval(
        spec->lower_bound_key()->encoded_key(),
        spec->exclusive_upper_bound_key()->encoded_key(),
        &interval_sets);
    for (const RowSet *rs : interval_sets) {
      gscoped_ptr<RowwiseIterator> row_it;
      RETURN_NOT_OK_PREPEND(rs->NewRowIterator(projection, snap, &row_it),
                            Substitute("Could not create iterator for rowset $0",
                                       rs->ToString()));
      ret.push_back(shared_ptr<RowwiseIterator>(row_it.release()));
    }
    ret.swap(*iters);
    return Status::OK();
  }

  // If there are no encoded predicates or they represent an open-ended range, then
  // fall back to grabbing all rowset iterators
  for (const shared_ptr<RowSet> &rs : components_->rowsets->all_rowsets()) {
    gscoped_ptr<RowwiseIterator> row_it;
    RETURN_NOT_OK_PREPEND(rs->NewRowIterator(projection, snap, &row_it),
                          Substitute("Could not create iterator for rowset $0",
                                     rs->ToString()));
    ret.push_back(shared_ptr<RowwiseIterator>(row_it.release()));
  }

  // Swap results into the parameters.
  ret.swap(*iters);
  return Status::OK();
}

namespace {

Result<IsolationLevel> GetIsolationLevel(const KeyValueWriteBatchPB& write_batch,
                                         TransactionParticipant* transaction_participant) {
  if (!write_batch.has_transaction()) {
    return IsolationLevel::NON_TRANSACTIONAL;
  }
  if (write_batch.transaction().has_isolation()) {
    return write_batch.transaction().isolation();
  }
  auto id = FullyDecodeTransactionId(write_batch.transaction().transaction_id());
  RETURN_NOT_OK(id);
  auto stored_metadata = transaction_participant->Metadata(*id);
  if (!stored_metadata) {
    return STATUS_FORMAT(IllegalState, "Missing metadata for transaction: $0", *id);
  }
  return stored_metadata->isolation;
}

} // namespace

Status Tablet::StartDocWriteOperation(const docdb::DocOperations &doc_ops,
                                      LockBatch *keys_locked,
                                      KeyValueWriteBatchPB* write_batch) {
  auto isolation_level = GetIsolationLevel(*write_batch, transaction_participant_.get());
  RETURN_NOT_OK(isolation_level);
  bool need_read_snapshot = false;
  docdb::PrepareDocWriteOperation(
      doc_ops, metrics_->write_lock_latency, *isolation_level, &shared_lock_manager_, keys_locked,
      &need_read_snapshot);

  HybridTime hybrid_time;
  unique_ptr<ScopedReadOperation> read_txn;
  if (need_read_snapshot) {
    read_txn.reset(new ScopedReadOperation(this));
    hybrid_time = read_txn->GetReadTimestamp();
  }

  if (*isolation_level == IsolationLevel::NON_TRANSACTIONAL &&
      metadata_->schema().table_properties().is_transactional()) {
    auto now = clock_->Now();
    auto result = docdb::ResolveOperationConflicts(
        doc_ops, now, rocksdb_.get(), transaction_participant_.get());
    RETURN_NOT_OK(result);
    if (now != *result) {
      clock_->Update(*result);
    }
  }

  // We expect all read operations for this transaction to be done in ApplyDocWriteOperation.
  // Once read_txn goes out of scope, the read point is deregistered.
  RETURN_NOT_OK(docdb::ApplyDocWriteOperation(
      doc_ops, hybrid_time, rocksdb_.get(), write_batch, &monotonic_counter_));

  if (*isolation_level != IsolationLevel::NON_TRANSACTIONAL) {
    auto result = docdb::ResolveTransactionConflicts(*write_batch,
                                                     clock_->Now(),
                                                     rocksdb_.get(),
                                                     transaction_participant_.get());
    if (!result.ok()) {
      *keys_locked = LockBatch();  // Unlock the keys.
      return result;
    }
  }

  return Status::OK();
}

size_t Tablet::MemRowSetSize() const {
  if (table_type_ != TableType::KUDU_COLUMNAR_TABLE_TYPE) {
    return 0;
  }
  scoped_refptr<TabletComponents> comps;
  GetComponents(&comps);

  if (comps) {
    return comps->memrowset->memory_footprint();
  }
  return 0;
}

bool Tablet::MemRowSetEmpty() const {
  if (table_type_ != TableType::KUDU_COLUMNAR_TABLE_TYPE) {
    return true;
  }

  scoped_refptr<TabletComponents> comps;
  GetComponents(&comps);

  return comps->memrowset->empty();
}

size_t Tablet::MemRowSetLogRetentionSize(const MaxIdxToSegmentMap& max_idx_to_segment_size) const {
  if (table_type_ != TableType::KUDU_COLUMNAR_TABLE_TYPE) {
    return 0;
  }

  scoped_refptr<TabletComponents> comps;
  GetComponents(&comps);

  return GetLogRetentionSizeForIndex(comps->memrowset->MinUnflushedLogIndex(),
                                     max_idx_to_segment_size);
}

size_t Tablet::EstimateOnDiskSize() const {
  scoped_refptr<TabletComponents> comps;
  GetComponents(&comps);

  if (!comps) return 0;

  size_t ret = 0;
  for (const shared_ptr<RowSet> &rowset : comps->rowsets->all_rowsets()) {
    ret += rowset->EstimateOnDiskSize();
  }

  return ret;
}

size_t Tablet::DeltaMemStoresSize() const {
  scoped_refptr<TabletComponents> comps;
  GetComponents(&comps);

  size_t ret = 0;
  for (const shared_ptr<RowSet> &rowset : comps->rowsets->all_rowsets()) {
    ret += rowset->DeltaMemStoreSize();
  }

  return ret;
}

bool Tablet::DeltaMemRowSetEmpty() const {
  if (table_type_ != TableType::KUDU_COLUMNAR_TABLE_TYPE) {
    return false;
  }

  scoped_refptr<TabletComponents> comps;
  GetComponents(&comps);

  for (const shared_ptr<RowSet> &rowset : comps->rowsets->all_rowsets()) {
    if (!rowset->DeltaMemStoreEmpty()) {
      return false;
    }
  }

  return true;
}

void Tablet::GetInfoForBestDMSToFlush(const MaxIdxToSegmentMap& max_idx_to_segment_size,
                                      int64_t* mem_size, int64_t* retention_size) const {
  shared_ptr<RowSet> rowset = FindBestDMSToFlush(max_idx_to_segment_size);

  if (rowset) {
    *retention_size = GetLogRetentionSizeForIndex(rowset->MinUnflushedLogIndex(),
                                                  max_idx_to_segment_size);
    *mem_size = rowset->DeltaMemStoreSize();
  } else {
    *retention_size = 0;
    *mem_size = 0;
  }
}

Status Tablet::FlushDMSWithHighestRetention(
    const MaxIdxToSegmentMap& max_idx_to_segment_size) const {
  shared_ptr<RowSet> rowset = FindBestDMSToFlush(max_idx_to_segment_size);
  if (rowset) {
    return rowset->FlushDeltas();
  }
  return Status::OK();
}

shared_ptr<RowSet> Tablet::FindBestDMSToFlush(
    const MaxIdxToSegmentMap& max_idx_to_segment_size) const {
  scoped_refptr<TabletComponents> comps;
  GetComponents(&comps);
  int64_t mem_size = 0;
  int64_t retention_size = 0;
  shared_ptr<RowSet> best_dms;
  for (const shared_ptr<RowSet> &rowset : comps->rowsets->all_rowsets()) {
    if (rowset->DeltaMemStoreEmpty()) {
      continue;
    }
    int64_t size = GetLogRetentionSizeForIndex(rowset->MinUnflushedLogIndex(),
                                               max_idx_to_segment_size);
    if ((size > retention_size) ||
        (size == retention_size &&
         (rowset->DeltaMemStoreSize() > mem_size))) {
      mem_size = rowset->DeltaMemStoreSize();
      retention_size = size;
      best_dms = rowset;
    }
  }
  return best_dms;
}

int64_t Tablet::GetLogRetentionSizeForIndex(int64_t min_log_index,
                                            const MaxIdxToSegmentMap& max_idx_to_segment_size) {
  if (max_idx_to_segment_size.size() == 0 || min_log_index == -1) {
    return 0;
  }
  int64_t total_size = 0;
  for (const MaxIdxToSegmentMap::value_type& entry : max_idx_to_segment_size) {
    if (min_log_index > entry.first) {
      continue;  // We're not in this segment, probably someone else is retaining it.
    }
    total_size += entry.second;
  }
  return total_size;
}

Status Tablet::FlushBiggestDMS() {
  if (table_type_ != TableType::KUDU_COLUMNAR_TABLE_TYPE) {
    return Status::OK();
  }
  CHECK_EQ(state_, kOpen);
  scoped_refptr<TabletComponents> comps;
  GetComponents(&comps);

  int64_t max_size = -1;
  shared_ptr<RowSet> biggest_drs;
  for (const shared_ptr<RowSet> &rowset : comps->rowsets->all_rowsets()) {
    int64_t current = rowset->DeltaMemStoreSize();
    if (current > max_size) {
      max_size = current;
      biggest_drs = rowset;
    }
  }
  return max_size > 0 ? biggest_drs->FlushDeltas() : Status::OK();
}

Status Tablet::CompactWorstDeltas(RowSet::DeltaCompactionType type) {
  if (table_type_ != TableType::KUDU_COLUMNAR_TABLE_TYPE) {
    return Status::OK();
  }
  CHECK_EQ(state_, kOpen);
  shared_ptr<RowSet> rs;
  // We're required to grab the rowset's compact_flush_lock under the compact_select_lock_.
  std::unique_lock<std::mutex> lock;
  double perf_improv;
  {
    // We only want to keep the selection lock during the time we look at rowsets to compact.
    // The returned rowset is guaranteed to be available to lock since locking must be done
    // under this lock.
    std::lock_guard<std::mutex> compact_lock(compact_select_lock_);
    perf_improv = GetPerfImprovementForBestDeltaCompactUnlocked(type, &rs);
    if (rs) {
      lock = std::unique_lock<std::mutex>(*rs->compact_flush_lock(), std::try_to_lock);
      CHECK(lock.owns_lock());
    } else {
      return Status::OK();
    }
  }

  // We just released compact_select_lock_ so other compactions can select and run, but the
  // rowset is ours.
  DCHECK_NE(perf_improv, 0);
  if (type == RowSet::MINOR_DELTA_COMPACTION) {
    RETURN_NOT_OK_PREPEND(rs->MinorCompactDeltaStores(),
                          "Failed minor delta compaction on " + rs->ToString());
  } else if (type == RowSet::MAJOR_DELTA_COMPACTION) {
    RETURN_NOT_OK_PREPEND(down_cast<DiskRowSet*>(rs.get())->MajorCompactDeltaStores(),
                          "Failed major delta compaction on " + rs->ToString());
  }
  return Status::OK();
}

double Tablet::GetPerfImprovementForBestDeltaCompact(RowSet::DeltaCompactionType type,
                                                     shared_ptr<RowSet>* rs) const {
  if (table_type_ != TableType::KUDU_COLUMNAR_TABLE_TYPE) {
    return 0;
  }
  std::lock_guard<std::mutex> compact_lock(compact_select_lock_);
  return GetPerfImprovementForBestDeltaCompactUnlocked(type, rs);
}

double Tablet::GetPerfImprovementForBestDeltaCompactUnlocked(RowSet::DeltaCompactionType type,
                                                             shared_ptr<RowSet>* rs) const {
  std::unique_lock<std::mutex> cs_lock(compact_select_lock_, std::try_to_lock);
  DCHECK(!cs_lock.owns_lock());
  scoped_refptr<TabletComponents> comps;
  GetComponents(&comps);
  double worst_delta_perf = 0;
  shared_ptr<RowSet> worst_rs;
  for (const shared_ptr<RowSet> &rowset : comps->rowsets->all_rowsets()) {
    if (!rowset->IsAvailableForCompaction()) {
      continue;
    }
    double perf_improv = rowset->DeltaStoresCompactionPerfImprovementScore(type);
    if (perf_improv > worst_delta_perf) {
      worst_rs = rowset;
      worst_delta_perf = perf_improv;
    }
  }
  if (rs && worst_delta_perf > 0) {
    *rs = worst_rs;
  }
  return worst_delta_perf;
}

size_t Tablet::num_rowsets() const {
  if (table_type_ != TableType::KUDU_COLUMNAR_TABLE_TYPE) {
    return 0;
  }
  shared_lock<rw_spinlock> lock(component_lock_);
  return components_->rowsets->all_rowsets().size();
}

void Tablet::PrintRSLayout(ostream* o) {
  if (table_type_ != TableType::KUDU_COLUMNAR_TABLE_TYPE) {
    *o << "<p>This tablet doesn't use a rowset representation</p>";
    return;
  }
  shared_ptr<RowSetTree> rowsets_copy;
  {
    shared_lock<rw_spinlock> lock(component_lock_);
    rowsets_copy = components_->rowsets;
  }
  std::lock_guard<std::mutex> compact_lock(compact_select_lock_);
  // Run the compaction policy in order to get its log and highlight those
  // rowsets which would be compacted next.
  vector<string> log;
  unordered_set<RowSet*> picked;
  double quality;
  Status s = compaction_policy_->PickRowSets(*rowsets_copy, &picked, &quality, &log);
  if (!s.ok()) {
    *o << "<b>Error:</b> " << EscapeForHtmlToString(s.ToString());
    return;
  }

  if (!picked.empty()) {
    *o << "<p>";
    *o << "Highlighted rowsets indicate those that would be compacted next if a "
       << "compaction were to run on this tablet.";
    *o << "</p>";
  }

  vector<RowSetInfo> min, max;
  RowSetInfo::CollectOrdered(*rowsets_copy, &min, &max);
  DumpCompactionSVG(min, picked, o, false);

  *o << "<h2>Compaction policy log</h2>" << std::endl;

  *o << "<pre>" << std::endl;
  for (const string& s : log) {
    *o << EscapeForHtmlToString(s) << std::endl;
  }
  *o << "</pre>" << std::endl;
}

////////////////////////////////////////////////////////////
// Tablet::Iterator
////////////////////////////////////////////////////////////

Tablet::Iterator::Iterator(const Tablet* tablet, const Schema& projection,
                           MvccSnapshot snap, const OrderMode order,
                           const boost::optional<TransactionId>& transaction_id)
    : tablet_(tablet),
      projection_(projection),
      snap_(std::move(snap)),
      order_(order),
      transaction_id_(transaction_id),
      arena_(256, 4096),
      encoder_(&tablet_->key_schema(), &arena_) {}

Tablet::Iterator::~Iterator() {}

Status Tablet::Iterator::Init(ScanSpec *spec) {
  DCHECK(iter_.get() == nullptr);

  RETURN_NOT_OK(tablet_->GetMappedReadProjection(projection_, &projection_));

  vector<shared_ptr<RowwiseIterator> > iters;
  if (spec != nullptr) {
    VLOG(3) << "Before encoding range preds: " << spec->ToString();
    encoder_.EncodeRangePredicates(spec, true);
    VLOG(3) << "After encoding range preds: " << spec->ToString();
  }

  RETURN_NOT_OK(tablet_->CaptureConsistentIterators(
      &projection_, snap_, spec, transaction_id_, &iters));

  switch (order_) {
    case ORDERED:
      iter_.reset(new MergeIterator(projection_, iters));
      break;
    case UNORDERED:
    default:
      iter_.reset(new UnionIterator(iters));
      break;
  }

  RETURN_NOT_OK(iter_->Init(spec));
  return Status::OK();
}

bool Tablet::Iterator::HasNext() const {
  DCHECK(iter_.get() != nullptr) << "Not initialized!";
  return iter_->HasNext();
}

Status Tablet::Iterator::NextBlock(RowBlock *dst) {
  DCHECK(iter_.get() != nullptr) << "Not initialized!";
  return iter_->NextBlock(dst);
}

string Tablet::Iterator::ToString() const {
  string s;
  s.append("tablet iterator: ");
  if (iter_.get() == nullptr) {
    s.append("NULL");
  } else {
    s.append(iter_->ToString());
  }
  return s;
}

void Tablet::Iterator::GetIteratorStats(vector<IteratorStats>* stats) const {
  iter_->GetIteratorStats(stats);
}

HybridTime Tablet::SafeTimestampToRead() const {
  return mvcc_.GetMaxSafeTimeToReadAt();
}

HybridTime Tablet::OldestReadPoint() const {
  std::lock_guard<std::mutex> lock(active_readers_mutex_);
  if (active_readers_cnt_.empty()) {
    return SafeTimestampToRead();
  }
  return active_readers_cnt_.begin()->first;
}

void Tablet::RegisterReaderTimestamp(HybridTime read_point) {
  std::lock_guard<std::mutex> lock(active_readers_mutex_);
  active_readers_cnt_[read_point]++;
}

void Tablet::UnregisterReader(HybridTime timestamp) {
  std::lock_guard<std::mutex> lock(active_readers_mutex_);
  active_readers_cnt_[timestamp]--;
  if (active_readers_cnt_[timestamp] == 0) {
    active_readers_cnt_.erase(timestamp);
  }
}

void Tablet::ForceRocksDBCompactInTest() {
  rocksdb_->CompactRange(rocksdb::CompactRangeOptions(),
      /* begin = */ nullptr,
      /* end = */ nullptr);
  uint64_t compaction_pending, running_compactions;

  while (true) {
    rocksdb_->GetIntProperty("rocksdb.compaction-pending", &compaction_pending);
    rocksdb_->GetIntProperty("rocksdb.num-running-compactions", &running_compactions);
    if (!compaction_pending && !running_compactions) {
      return;
    }

    SleepFor(MonoDelta::FromMilliseconds(10));
  }
}

std::string Tablet::DocDBDumpStrInTest() {
  return docdb::DocDBDebugDumpToStr(rocksdb_.get());
}

void Tablet::LostLeadership() {
  if (transaction_coordinator_) {
    transaction_coordinator_->ClearLocks();
  }
}

uint64_t Tablet::GetTotalSSTFileSizes() const {
  std::lock_guard<rw_spinlock> lock(component_lock_);
  if (!rocksdb_) {
    return 0;
  }
  return rocksdb_->GetTotalSSTFileSize();
}

Result<TransactionOperationContextOpt> Tablet::CreateTransactionOperationContext(
    const TransactionMetadataPB& transaction_metadata) const {
  if (metadata_->schema().table_properties().is_transactional()) {
    if (transaction_metadata.has_transaction_id()) {
      Result<TransactionId> txn_id = FullyDecodeTransactionId(
          transaction_metadata.transaction_id());
      RETURN_NOT_OK(txn_id);
      return Result<TransactionOperationContextOpt>(boost::make_optional(
          TransactionOperationContext(*txn_id, transaction_participant())));
    } else {
      // We still need context with transaction participant in order to resolve intents during
      // possible reads.
      return Result<TransactionOperationContextOpt>(boost::make_optional(
          TransactionOperationContext(GenerateTransactionId(), transaction_participant())));
    }
  } else {
    return Result<TransactionOperationContextOpt>(boost::none);
  }
}

TransactionOperationContextOpt Tablet::CreateTransactionOperationContext(
    const boost::optional<TransactionId>& transaction_id) const {
  if (metadata_->schema().table_properties().is_transactional()) {
    if (transaction_id.is_initialized()) {
      return TransactionOperationContext(transaction_id.get(), transaction_participant());
    } else {
      // We still need context with transaction participant in order to resolve intents during
      // possible reads.
      return TransactionOperationContext(GenerateTransactionId(), transaction_participant());
    }
  } else {
    return boost::none;
  }
}

ScopedReadOperation::ScopedReadOperation(AbstractTablet* tablet)
    : tablet_(tablet), timestamp_(tablet_->SafeTimestampToRead()) {
  tablet_->RegisterReaderTimestamp(timestamp_);
}

ScopedReadOperation::~ScopedReadOperation() {
  tablet_->UnregisterReader(timestamp_);
}

HybridTime ScopedReadOperation::GetReadTimestamp() {
  return timestamp_;
}

}  // namespace tablet
}  // namespace yb
