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
#include "yb/tablet/tablet_bootstrap.h"

#include "yb/consensus/consensus.h"
#include "yb/consensus/log_anchor_registry.h"
#include "yb/consensus/log_reader.h"
#include "yb/server/hybrid_clock.h"
#include "yb/tablet/row_op.h"
#include "yb/tablet/tablet.h"
#include "yb/tablet/tablet_peer.h"
#include "yb/tablet/operations/alter_schema_operation.h"
#include "yb/tablet/operations/update_txn_operation.h"
#include "yb/tablet/operations/write_operation.h"
#include "yb/util/fault_injection.h"
#include "yb/util/flag_tags.h"
#include "yb/util/opid.h"
#include "yb/util/logging.h"
#include "yb/util/stopwatch.h"

DEFINE_bool(skip_remove_old_recovery_dir, false,
            "Skip removing WAL recovery dir after startup. (useful for debugging)");
TAG_FLAG(skip_remove_old_recovery_dir, hidden);

DEFINE_test_flag(double, fault_crash_during_log_replay, 0.0,
                 "Fraction of the time when the tablet will crash immediately "
                 "after processing a log entry during log replay.");

DECLARE_uint64(max_clock_sync_error_usec);

namespace yb {
namespace tablet {

using namespace std::literals; // NOLINT
using namespace std::placeholders;
using std::shared_ptr;

using log::Log;
using log::LogEntryPB;
using log::LogOptions;
using log::LogReader;
using log::ReadableLogSegment;
using consensus::ChangeConfigRecordPB;
using consensus::RaftConfigPB;
using consensus::CommitMsg;
using consensus::ConsensusBootstrapInfo;
using consensus::ConsensusMetadata;
using consensus::MinimumOpId;
using consensus::OperationType;
using consensus::OpId;
using consensus::OpIdEquals;
using consensus::OpIdToString;
using consensus::ReplicateMsg;
using strings::Substitute;
using tserver::AlterSchemaRequestPB;
using tserver::WriteRequestPB;

static string DebugInfo(const string& tablet_id,
                        int segment_seqno,
                        int entry_idx,
                        const string& segment_path,
                        const LogEntryPB& entry) {
  // Truncate the debug string to a reasonable length for logging.
  // Otherwise, glog will truncate for us and we may miss important
  // information which came after this long string.
  string debug_str = entry.ShortDebugString();
  if (debug_str.size() > 500) {
    debug_str.resize(500);
    debug_str.append("...");
  }
  return Substitute("Debug Info: Error playing entry $0 of segment $1 of tablet $2. "
                    "Segment path: $3. Entry: $4", entry_idx, segment_seqno, tablet_id,
                    segment_path, debug_str);
}

// ============================================================================
//  Class ReplayState.
// ============================================================================
ReplayState::ReplayState(const OpId& last_op_id)
    : last_stored_op_id(last_op_id) {
  // If we know last flushed op id, then initialize committed_op_id with it.
  if (last_op_id.term() > yb::OpId::kUnknownTerm) {
    committed_op_id = last_op_id;
    rocksdb_applied_index = -1;
  } else {
    // Fallback to old logic.
    rocksdb_applied_index = last_op_id.index();
  }
}

// Return true if 'b' is allowed to immediately follow 'a' in the log.
bool ReplayState::IsValidSequence(const OpId& a, const OpId& b) {
  if (a.term() == 0 && a.index() == 0) {
    // Not initialized - can start with any opid.
    return true;
  }

  // Within the same term, we should never skip entries.
  // We can, however go backwards (see KUDU-783 for an example)
  if (b.term() == a.term() &&
      b.index() > a.index() + 1) {
    return false;
  }

  return true;
}

// Return a Corruption status if 'id' seems to be out-of-sequence in the log.
Status ReplayState::CheckSequentialReplicateId(const ReplicateMsg& msg) {
  DCHECK(msg.has_id());
  if (PREDICT_FALSE(!IsValidSequence(prev_op_id, msg.id()))) {
    string op_desc = Substitute("$0 REPLICATE (Type: $1)",
                                OpIdToString(msg.id()),
                                OperationType_Name(msg.op_type()));
    return STATUS(Corruption,
      Substitute("Unexpected opid following opid $0. Operation: $1",
                 OpIdToString(prev_op_id),
                 op_desc));
  }

  prev_op_id = msg.id();
  return Status::OK();
}

void ReplayState::UpdateCommittedOpId(const OpId& id) {
  if (consensus::OpIdLessThan(committed_op_id, id)) {
    committed_op_id = id;
  }
}

void ReplayState::AddEntriesToStrings(const OpIndexToEntryMap& entries,
                                      vector<string>* strings) const {
  for (const OpIndexToEntryMap::value_type& map_entry : entries) {
    LogEntryPB* entry = DCHECK_NOTNULL(map_entry.second.get());
    strings->push_back(Substitute("   [$0] $1", map_entry.first, entry->ShortDebugString()));
  }
}

void ReplayState::DumpReplayStateToStrings(vector<string>* strings)  const {
  strings->push_back(Substitute(
      "ReplayState: "
      "Previous OpId: $0, "
      "Committed OpId: $1, "
      "Pending Replicates: $2, "
      "Pending Commits: $3, "
      "Flushed: $4",
      OpIdToString(prev_op_id),
      OpIdToString(committed_op_id),
      pending_replicates.size(),
      pending_commits.size(),
      OpIdToString(last_stored_op_id)));
  if (num_entries_applied_to_rocksdb > 0) {
    strings->push_back(Substitute("Log entries applied to RocksDB: $0",
                                  num_entries_applied_to_rocksdb));
  }
  if (!pending_replicates.empty()) {
    strings->push_back(Substitute("Dumping REPLICATES ($0 items):", pending_replicates.size()));
    AddEntriesToStrings(pending_replicates, strings);
  }
  if (!pending_commits.empty()) {
    strings->push_back(Substitute("Dumping COMMITS ($0 items):", pending_commits.size()));
    AddEntriesToStrings(pending_commits, strings);
  }
}

bool ReplayState::CanApply(int64_t index, LogEntryPB* entry) {
  if (rocksdb_applied_index != -1) {
    if (index != rocksdb_applied_index + 1) {
      return false;
    }
  }
  return consensus::OpIdCompare(entry->replicate().id(), committed_op_id) <= 0;
}

// ============================================================================
//  Class TabletBootstrap.
// ============================================================================
TabletBootstrap::TabletBootstrap(const BootstrapTabletData& data)
    : data_(data),
      meta_(data.meta),
      mem_tracker_(data.mem_tracker),
      metric_registry_(data.metric_registry),
      listener_(data.listener),
      log_anchor_registry_(data.log_anchor_registry),
      tablet_options_(data.tablet_options) {}

Status TabletBootstrap::Bootstrap(shared_ptr<TabletClass>* rebuilt_tablet,
                                  scoped_refptr<Log>* rebuilt_log,
                                  ConsensusBootstrapInfo* consensus_info) {
  string tablet_id = meta_->tablet_id();
  TableType table_type = meta_->table_type();

  // Replay requires a valid Consensus metadata file to exist in order to
  // compare the committed consensus configuration seqno with the log entries and also to persist
  // committed but unpersisted changes.
  RETURN_NOT_OK_PREPEND(ConsensusMetadata::Load(meta_->fs_manager(), tablet_id,
                                                meta_->fs_manager()->uuid(), &cmeta_),
                        "Unable to load Consensus metadata");

  // Make sure we don't try to locally bootstrap a tablet that was in the middle
  // of a remote bootstrap. It's likely that not all files were copied over
  // successfully.
  TabletDataState tablet_data_state = meta_->tablet_data_state();
  if (tablet_data_state != TABLET_DATA_READY) {
    return STATUS(Corruption, "Unable to locally bootstrap tablet " + tablet_id + ": " +
                              "TabletMetadata bootstrap state is " +
                              TabletDataState_Name(tablet_data_state));
  }

  if (table_type == TableType::KUDU_COLUMNAR_TABLE_TYPE) {
    meta_->PinFlush();
  }

  listener_->StatusMessage("Bootstrap starting.");

  if (VLOG_IS_ON(1)) {
    TabletSuperBlockPB super_block;
    RETURN_NOT_OK(meta_->ToSuperBlock(&super_block));
    VLOG_WITH_PREFIX(1) << "Tablet Metadata: " << super_block.DebugString();
  }

  if (table_type == TableType::KUDU_COLUMNAR_TABLE_TYPE) {
    RETURN_NOT_OK(flushed_stores_.InitFrom(*meta_.get()));
  }

  bool has_blocks;
  RETURN_NOT_OK(OpenTablet(&has_blocks));

  bool needs_recovery;
  RETURN_NOT_OK(PrepareRecoveryDir(&needs_recovery));
  if (needs_recovery) {
    RETURN_NOT_OK(OpenLogReaderInRecoveryDir());
  }

  // This is a new tablet, nothing left to do.
  if (!has_blocks && !needs_recovery) {
    LOG_WITH_PREFIX(INFO) << "No blocks or log segments found. Creating new log.";
    RETURN_NOT_OK_PREPEND(OpenNewLog(), "Failed to open new log");
    RETURN_NOT_OK(FinishBootstrap("No bootstrap required, opened a new log",
                                  rebuilt_log,
                                  rebuilt_tablet));
    consensus_info->last_id = MinimumOpId();
    consensus_info->last_committed_id = MinimumOpId();
    return Status::OK();
  }

  // If there were blocks, there must be segments to replay. This is required
  // by Raft, since we always need to know the term and index of the last
  // logged op in order to vote, know how to respond to AppendEntries(), etc.
  if (has_blocks && !needs_recovery) {
    return STATUS(IllegalState, Substitute("Tablet $0: Found rowsets but no log "
                                           "segments could be found.",
                                           tablet_id));
  }

  // Before playing any segments we set the safe and clean times to 'kMin' so that
  // the MvccManager will accept all transactions that we replay as uncommitted.
  tablet_->mvcc_manager()->OfflineAdjustSafeTime(HybridTime::kMin);
  RETURN_NOT_OK_PREPEND(PlaySegments(consensus_info), "Failed log replay. Reason");

  // Flush the consensus metadata once at the end to persist our changes, if any.
  RETURN_NOT_OK(cmeta_->Flush());

  RETURN_NOT_OK(RemoveRecoveryDir());
  RETURN_NOT_OK(FinishBootstrap("Bootstrap complete.", rebuilt_log, rebuilt_tablet));

  return Status::OK();
}

Status TabletBootstrap::FinishBootstrap(const string& message,
                                        scoped_refptr<log::Log>* rebuilt_log,
                                        shared_ptr<TabletClass>* rebuilt_tablet) {
  // Add a callback to TabletMetadata that makes sure that each time we flush the metadata
  // we also wait for in-flights to finish and for their wal entry to be fsynced.
  // This might be a bit conservative in some situations but it will prevent us from
  // ever flushing the metadata referring to tablet data blocks containing data whose
  // commit entries are not durable, a pre-requisite for recovery.
  meta_->SetPreFlushCallback(
      Bind(&FlushInflightsToLogCallback::WaitForInflightsAndFlushLog,
           make_scoped_refptr(new FlushInflightsToLogCallback(tablet_.get(),
                                                              log_))));
  tablet_->MarkFinishedBootstrapping();
  if (tablet_->table_type() == TableType::KUDU_COLUMNAR_TABLE_TYPE) {
    RETURN_NOT_OK(tablet_->metadata()->UnPinFlush());
  }
  listener_->StatusMessage(message);
  rebuilt_tablet->reset(tablet_.release());
  rebuilt_log->swap(log_);
  return Status::OK();
}

Status TabletBootstrap::OpenTablet(bool* has_blocks) {
  auto tablet = std::make_unique<TabletClass>(
      meta_, data_.clock, mem_tracker_, metric_registry_, log_anchor_registry_, tablet_options_,
      data_.transaction_participant_context, data_.transaction_coordinator_context);
  // doing nothing for now except opening a tablet locally.
  LOG_TIMING_PREFIX(INFO, LogPrefix(), "opening tablet") {
    RETURN_NOT_OK(tablet->Open());
  }
  switch (tablet->table_type()) {
    case TableType::KUDU_COLUMNAR_TABLE_TYPE:
      *has_blocks = tablet->num_rowsets() != 0;
      break;
    case TableType::YQL_TABLE_TYPE: FALLTHROUGH_INTENDED;
    case TableType::REDIS_TABLE_TYPE:
      *has_blocks = tablet->HasSSTables();
      break;
    default:
      LOG(FATAL) << "Invalid table type " << tablet->table_type();
  }
  tablet_ = std::move(tablet);
  return Status::OK();
}

Status TabletBootstrap::PrepareRecoveryDir(bool* needs_recovery) {
  *needs_recovery = false;

  FsManager* fs_manager = tablet_->metadata()->fs_manager();
  const string& log_dir = tablet_->metadata()->wal_dir();

  // If the recovery directory exists, then we crashed mid-recovery.
  // Throw away any logs from the previous recovery attempt and restart the log
  // replay process from the beginning using the same recovery dir as last time.
  const string recovery_path = fs_manager->GetTabletWalRecoveryDir(log_dir);
  if (fs_manager->Exists(recovery_path)) {
    LOG_WITH_PREFIX(INFO) << "Previous recovery directory found at " << recovery_path << ": "
                          << "Replaying log files from this location instead of " << log_dir;

    // Since we have a recovery directory, clear out the log_dir by recursively
    // deleting it and creating a new one so that we don't end up with remnants
    // of old WAL segments or indexes after replay.
    if (fs_manager->env()->FileExists(log_dir)) {
      LOG_WITH_PREFIX(INFO) << "Deleting old log files from previous recovery attempt in "
                            << log_dir;
      RETURN_NOT_OK_PREPEND(fs_manager->env()->DeleteRecursively(log_dir),
                            "Could not recursively delete old log dir " + log_dir);
    }

    RETURN_NOT_OK_PREPEND(fs_manager->CreateDirIfMissing(DirName(log_dir)),
                          "Failed to create table log directory " + DirName(log_dir));

    RETURN_NOT_OK_PREPEND(fs_manager->CreateDirIfMissing(log_dir),
                          "Failed to create tablet log directory " + log_dir);

    *needs_recovery = true;
    return Status::OK();
  }

  // If we made it here, there was no pre-existing recovery dir.
  // Now we look for log files in log_dir, and if we find any then we rename
  // the whole log_dir to a recovery dir and return needs_recovery = true.
  RETURN_NOT_OK_PREPEND(fs_manager->CreateDirIfMissing(DirName(log_dir)),
                        "Failed to create table log directory " + DirName(log_dir));

  RETURN_NOT_OK_PREPEND(fs_manager->CreateDirIfMissing(log_dir),
                        "Failed to create tablet log directory " + log_dir);

  vector<string> children;
  RETURN_NOT_OK_PREPEND(fs_manager->ListDir(log_dir, &children),
                        "Couldn't list log segments.");
  for (const string& child : children) {
    if (!log::IsLogFileName(child)) {
      continue;
    }

    string source_path = JoinPathSegments(log_dir, child);
    string dest_path = JoinPathSegments(recovery_path, child);
    LOG_WITH_PREFIX(INFO) << "Will attempt to recover log segment " << source_path
                          << " to " << dest_path;
    *needs_recovery = true;
  }

  if (*needs_recovery) {
    // Atomically rename the log directory to the recovery directory
    // and then re-create the log directory.
    LOG_WITH_PREFIX(INFO) << "Moving log directory " << log_dir << " to recovery directory "
                          << recovery_path << " in preparation for log replay";
    RETURN_NOT_OK_PREPEND(fs_manager->env()->RenameFile(log_dir, recovery_path),
                          Substitute("Could not move log directory $0 to recovery dir $1",
                                     log_dir, recovery_path));
    RETURN_NOT_OK_PREPEND(fs_manager->env()->CreateDir(log_dir),
                          "Failed to recreate log directory " + log_dir);
  }
  return Status::OK();
}

Status TabletBootstrap::OpenLogReaderInRecoveryDir() {
  VLOG_WITH_PREFIX(1) << "Opening log reader in log recovery dir "
      << meta_->fs_manager()->GetTabletWalRecoveryDir(tablet_->metadata()->wal_dir());
  // Open the reader.
  RETURN_NOT_OK_PREPEND(LogReader::OpenFromRecoveryDir(tablet_->metadata()->fs_manager(),
                                                       tablet_->metadata()->tablet_id(),
                                                       tablet_->metadata()->wal_dir(),
                                                       tablet_->GetMetricEntity().get(),
                                                       &log_reader_),
                        "Could not open LogReader. Reason");
  return Status::OK();
}

Status TabletBootstrap::RemoveRecoveryDir() {
  FsManager* fs_manager = tablet_->metadata()->fs_manager();
  const string recovery_path = fs_manager->GetTabletWalRecoveryDir(tablet_->metadata()->wal_dir());
  CHECK(fs_manager->Exists(recovery_path))
      << "Tablet WAL recovery dir " << recovery_path << " does not exist.";

  LOG_WITH_PREFIX(INFO) << "Preparing to delete log recovery files and directory " << recovery_path;

  string tmp_path = Substitute("$0-$1", recovery_path, GetCurrentTimeMicros());
  LOG_WITH_PREFIX(INFO) << "Renaming log recovery dir from "  << recovery_path
                        << " to " << tmp_path;
  RETURN_NOT_OK_PREPEND(fs_manager->env()->RenameFile(recovery_path, tmp_path),
                        Substitute("Could not rename old recovery dir from: $0 to: $1",
                                   recovery_path, tmp_path));

  if (FLAGS_skip_remove_old_recovery_dir) {
    LOG_WITH_PREFIX(INFO) << "--skip_remove_old_recovery_dir enabled. NOT deleting " << tmp_path;
    return Status::OK();
  }
  LOG_WITH_PREFIX(INFO) << "Deleting all files from renamed log recovery directory " << tmp_path;
  RETURN_NOT_OK_PREPEND(fs_manager->env()->DeleteRecursively(tmp_path),
                        "Could not remove renamed recovery dir " + tmp_path);
  LOG_WITH_PREFIX(INFO) << "Completed deletion of old log recovery files and directory "
                        << tmp_path;
  return Status::OK();
}

Status TabletBootstrap::OpenNewLog() {
  OpId init;
  init.set_term(0);
  init.set_index(0);
  RETURN_NOT_OK(Log::Open(LogOptions(),
                          tablet_->metadata()->fs_manager(),
                          tablet_->tablet_id(),
                          tablet_->metadata()->wal_dir(),
                          *tablet_->schema(),
                          tablet_->metadata()->schema_version(),
                          tablet_->GetMetricEntity(),
                          &log_));
  // Disable sync temporarily in order to speed up appends during the
  // bootstrap process.
  log_->DisableSync();
  return Status::OK();
}

// Handle the given log entry. If OK is returned, then takes ownership of 'entry'.
// Otherwise, caller frees.
Status TabletBootstrap::HandleEntry(ReplayState* state, std::unique_ptr<LogEntryPB>* entry_ptr) {
  auto& entry = **entry_ptr;
  if (VLOG_IS_ON(1)) {
    VLOG_WITH_PREFIX(1) << "Handling entry: " << entry.ShortDebugString();
  }

  switch (entry.type()) {
    case log::REPLICATE:
      RETURN_NOT_OK(HandleReplicateMessage(state, entry_ptr));
      break;
    case log::COMMIT:
      if (tablet_->table_type() == TableType::KUDU_COLUMNAR_TABLE_TYPE) {
        // check the unpaired ops for the matching replicate msg, abort if not found
        RETURN_NOT_OK(HandleCommitMessage(state, entry_ptr));
      } else if (entry.has_commit() && entry.commit().op_type() == consensus::NO_OP) {
        // These entry types are still expected to appear when a no-op is replicated. We should
        // eventually get rid of them too for non-Kudu tables.
        entry_ptr->reset();
      } else {
        LOG(FATAL) << "COMMIT entries other than no-ops should not be used by non-Kudu tables: "
            << entry.ShortDebugString();
      }
      break;
    default:
      return STATUS(Corruption, Substitute("Unexpected log entry type: $0", entry.type()));
  }
  MAYBE_FAULT(FLAGS_fault_crash_during_log_replay);
  return Status::OK();
}

// Takes ownership of 'replicate_entry' on OK status.
Status TabletBootstrap::HandleReplicateMessage(ReplayState* state,
                                               std::unique_ptr<LogEntryPB>* replicate_entry_ptr) {
  auto& replicate_entry = **replicate_entry_ptr;
  stats_.ops_read++;

  const ReplicateMsg& replicate = replicate_entry.replicate();
  RETURN_NOT_OK(state->CheckSequentialReplicateId(replicate));
  DCHECK(replicate.has_hybrid_time());
  UpdateClock(replicate.hybrid_time());

  // This sets the monotonic counter to at least replicate.monotonic_counter() atomically.
  tablet_->UpdateMonotonicCounter(replicate.monotonic_counter());

  const bool non_kudu = tablet_->table_type() != TableType::KUDU_COLUMNAR_TABLE_TYPE;
  const OpId op_id = replicate_entry.replicate().id();
  if (non_kudu && op_id.index() == state->last_stored_op_id.index()) {
    // We need to set the committed OpId to be at least what's been applied to RocksDB. The reason
    // we could not do it before starting log replay is that we don't know the term number of the
    // last write operation flushed into a RocksDB SSTable, even though we know its Raft index
    // (rocksdb_max_persistent_index).
    //
    // In fact, there could be multiple log entries with the Raft index equal to
    // rocksdb_max_persistent_index, but with different terms, in case a higher term's leader
    // "truncated" and overwrote uncommitted log entries from a lower term. Note that such
    // "truncation" only happens in memory, not on disk: the leader keeps appending new entries to
    // the log, but for all intents and purposes we can think of it as of real log truncation.
    //
    // Even in the above case, with index jumping back as entries get overwritten, it is always
    // safe to bump committed_op_id to at least the current OpId here. We will never apply entries
    // from pending_replicates that are not known to be committed after bumping up committed_op_id
    // here, because pending_replicates always contains entries with monotonically increasing
    // consecutive indexes, ending with the current index, which is equal to
    // rocksdb_max_persistent_index, and we only ever apply entries with an index greater than that.
    //
    // Also see the other place where we update state->committed_op_id in the end of this function.
    state->UpdateCommittedOpId(replicate.id());

    // We also update the MVCC safe time to make sure this committed entry is visible to readers
    // as every committed entry should be. Unlike the committed op id, though, we can't just update
    // the safe time here, as this entry could be overwritten by a later entry with a later term but
    // an earlier hybrid time (TODO: would that still be possible when we have leader leases?).
    // Instead, we only keep the last value of hybrid time of the entry at this index, and update
    // safe time based on it in the end. We do require that we keep at least one committed entry
    // in the log, though.
    state->rocksdb_last_entry_hybrid_time = HybridTime(replicate.hybrid_time());
  }

  // Append the replicate message to the log as is
  RETURN_NOT_OK(log_->Append(replicate_entry_ptr->get()));

  if (non_kudu && op_id.index() <= state->last_stored_op_id.index()) {
    // Do not update the bootstrap in-memory state for log records that have already been applied
    // to RocksDB, or were overwritten by a later entry with a higher term that has already been
    // applied to RocksDB.
    replicate_entry_ptr->reset();
    return Status::OK();
  }

  auto iter = state->pending_replicates.lower_bound(op_id.index());

  // If there was a entry with the same index we're overwriting then we need to delete
  // that entry and all entries with higher indexes.
  if (iter != state->pending_replicates.end() && iter->first == op_id.index()) {
    auto& existing_entry = iter->second;
    auto& last_entry = state->pending_replicates.rbegin()->second;

    LOG_WITH_PREFIX(INFO) << "Overwriting operations starting at: "
                          << existing_entry->replicate().id()
                          << " up to: " << last_entry->replicate().id()
                          << " with operation: " << replicate.id();
    stats_.ops_overwritten += std::distance(iter, state->pending_replicates.end());
    state->pending_replicates.erase(iter, state->pending_replicates.end());
  }

  CHECK(state->pending_replicates.emplace(op_id.index(), std::move(*replicate_entry_ptr)).second);

  if (non_kudu) {
    CHECK(replicate.has_committed_op_id())
        << "Replicate message has no committed_op_id for table type "
        << TableType_Name(tablet_->table_type()) << ". Replicate message:\n"
        << replicate.DebugString();

    // For RocksDB-backed tables we include the commit index as of the time a REPLICATE entry was
    // added to the leader's log into that entry. This allows us to decide when we can replay a
    // REPLICATE entry during bootstrap without Kudu's local COMMIT messages.
    state->UpdateCommittedOpId(replicate.committed_op_id());

    state->ApplyCommittedPendingReplicates(
        std::bind(&TabletBootstrap::HandleEntryPair, this, _1, nullptr));
  }
  return Status::OK();
}

// Takes ownership of 'commit_entry' on OK status.
Status TabletBootstrap::HandleCommitMessage(ReplayState* state,
                                            std::unique_ptr<LogEntryPB>* commit_entry_ptr) {
  LogEntryPB& commit_entry = **commit_entry_ptr;
  // We don't use COMMIT messages at all for RocksDB-backed tables.
  CHECK_EQ(tablet_->table_type(), TableType::KUDU_COLUMNAR_TABLE_TYPE);
  DCHECK(commit_entry.has_commit()) << "Not a commit message: " << commit_entry.DebugString();

  // Match up the COMMIT record with the original entry that it's applied to.
  const OpId& committed_op_id = commit_entry.commit().commited_op_id();
  state->UpdateCommittedOpId(committed_op_id);

  // If there are no pending replicates, or if this commit's index is lower than the
  // the first pending replicate on record this is likely an orphaned commit.
  if (state->pending_replicates.empty() ||
      (*state->pending_replicates.begin()).first > committed_op_id.index()) {
    VLOG_WITH_PREFIX(2) << "Found orphaned commit for " << committed_op_id;
    RETURN_NOT_OK(CheckOrphanedCommitAlreadyFlushed(commit_entry.commit()));
    stats_.orphaned_commits++;
    commit_entry_ptr->reset();
    return Status::OK();
  }

  // If this commit does not correspond to the first replicate message in the pending
  // replicates set we keep it to apply later...
  if ((*state->pending_replicates.begin()).first != committed_op_id.index()) {
    if (!ContainsKey(state->pending_replicates, committed_op_id.index())) {
      return STATUS(Corruption, Substitute("Could not find replicate for commit: $0",
                                           commit_entry.ShortDebugString()));
    }
    VLOG_WITH_PREFIX(2) << "Adding pending commit for " << committed_op_id;
    CHECK(state->pending_commits.emplace(committed_op_id.index(),
                                         std::move(*commit_entry_ptr)).second);
    return Status::OK();
  }

  // ... if it does, we apply it and all the commits that immediately follow in the sequence.
  OpId last_applied = commit_entry.commit().commited_op_id();
  RETURN_NOT_OK(ApplyCommitMessage(state, commit_entry));
  commit_entry_ptr->reset();

  auto iter = state->pending_commits.begin();
  while (iter != state->pending_commits.end()) {
    if ((*iter).first == last_applied.index() + 1) {
      std::unique_ptr<LogEntryPB> buffered_commit_entry = std::move(iter->second);
      state->pending_commits.erase(iter++);
      last_applied = buffered_commit_entry->commit().commited_op_id();
      RETURN_NOT_OK(ApplyCommitMessage(state, *buffered_commit_entry));
      continue;
    }
    break;
  }

  return Status::OK();
}

bool TabletBootstrap::AreAllStoresAlreadyFlushed(const CommitMsg& commit) {
  for (const OperationResultPB& op_result : commit.result().ops()) {
    for (const MemStoreTargetPB& mutated_store : op_result.mutated_stores()) {
      if (!flushed_stores_.WasStoreAlreadyFlushed(mutated_store)) {
        return false;
      }
    }
  }
  return true;
}

bool TabletBootstrap::AreAnyStoresAlreadyFlushed(const CommitMsg& commit) {
  for (const OperationResultPB& op_result : commit.result().ops()) {
    for (const MemStoreTargetPB& mutated_store : op_result.mutated_stores()) {
      if (flushed_stores_.WasStoreAlreadyFlushed(mutated_store)) {
        return true;
      }
    }
  }
  return false;
}

Status TabletBootstrap::CheckOrphanedCommitAlreadyFlushed(const CommitMsg& commit) {
  if (!AreAllStoresAlreadyFlushed(commit)) {
    TabletSuperBlockPB super;
    WARN_NOT_OK(meta_->ToSuperBlock(&super), LogPrefix() + "Couldn't build TabletSuperBlockPB");
    return STATUS(Corruption, Substitute("CommitMsg was orphaned but it referred to "
        "unflushed stores. Commit: $0. TabletMetadata: $1", commit.ShortDebugString(),
        super.ShortDebugString()));
  }
  return Status::OK();
}

Status TabletBootstrap::ApplyCommitMessage(ReplayState* state, const LogEntryPB& commit_entry) {
  const OpId& committed_op_id = commit_entry.commit().commited_op_id();
  VLOG_WITH_PREFIX(2) << "Applying commit for " << committed_op_id;
  // They should also have an associated replicate index (it may have been in a
  // deleted log segment though).
  auto it = state->pending_replicates.find(committed_op_id.index());

  if (it != state->pending_replicates.end()) {
    std::unique_ptr<LogEntryPB> pending_replicate_entry = std::move(it->second);
    state->pending_replicates.erase(it);
    // We found a replicate with the same index, make sure it also has the same
    // term.
    if (!OpIdEquals(committed_op_id, pending_replicate_entry->replicate().id())) {
      string error_msg = Substitute("Committed operation's OpId: $0 didn't match the"
          "commit message's committed OpId: $1. Pending operation: $2, Commit message: $3",
          pending_replicate_entry->replicate().id().ShortDebugString(),
          committed_op_id.ShortDebugString(),
          pending_replicate_entry->replicate().ShortDebugString(),
          commit_entry.commit().ShortDebugString());
      LOG_WITH_PREFIX(DFATAL) << error_msg;
      return STATUS(Corruption, error_msg);
    }
    RETURN_NOT_OK(HandleEntryPair(pending_replicate_entry.get(), &commit_entry));
    stats_.ops_committed++;
  } else {
    stats_.orphaned_commits++;
    RETURN_NOT_OK(CheckOrphanedCommitAlreadyFlushed(commit_entry.commit()));
  }

  return Status::OK();
}

Status TabletBootstrap::HandleOperation(OperationType op_type,
                                        ReplicateMsg* replicate,
                                        const CommitMsg* commit) {
  switch (op_type) {
    case consensus::WRITE_OP:
      return PlayWriteRequest(replicate, commit);

    case consensus::ALTER_SCHEMA_OP:
      return PlayAlterSchemaRequest(replicate, commit);

    case consensus::CHANGE_CONFIG_OP:
      return PlayChangeConfigRequest(replicate, commit);

    case consensus::NO_OP:
      return PlayNoOpRequest(replicate, commit);

    case consensus::UPDATE_TRANSACTION_OP:
      return PlayUpdateTransactionRequest(replicate, commit);

    // Unexpected cases:
    case consensus::SNAPSHOT_OP:
      return STATUS(IllegalState, Substitute(
          "The operation is not supported in the community edition: $0", op_type));

    case consensus::UNKNOWN_OP:
      return STATUS(IllegalState, Substitute("Unsupported operation type: $0", op_type));
  }

  FATAL_INVALID_ENUM_VALUE(consensus::OperationType, op_type);
}

// Never deletes 'replicate_entry' or 'commit_entry'.
Status TabletBootstrap::HandleEntryPair(LogEntryPB* replicate_entry,
                                        const LogEntryPB* commit_entry) {
  ReplicateMsg* replicate = replicate_entry->mutable_replicate();
  const CommitMsg* commit = commit_entry == nullptr ? nullptr : &commit_entry->commit();
  const OperationType op_type =
      commit == nullptr ? replicate_entry->replicate().op_type() : commit->op_type();

  {
    const auto status = HandleOperation(op_type, replicate, commit);
    if (!status.ok()) {
      return status.CloneAndAppend(Format(
          "Failed to play $0 request. ReplicateMsg: { $1 }, CommitMsg: { $2 }",
          OperationType_Name(op_type),
          *replicate,
          commit ? commit->ShortDebugString() : "N/A"s));
    }
  }

  // Non-tablet operations should not advance the safe time, because they are
  // not started serially and so may have hybrid_times that are out of order.
  if (op_type == consensus::NO_OP || op_type == consensus::CHANGE_CONFIG_OP) {
    return Status::OK();
  }

  // Handle safe time advancement:
  //
  // If this operation has an external consistency mode other than COMMIT_WAIT, we know that no
  // future transaction will have a hybrid_time that is lower than it, so we can just advance the
  // safe hybrid_time to this operation's hybrid_time.
  //
  // If the hybrid clock is disabled, all transactions will fall into this category.
  HybridTime safe_time;
  if (replicate->write_request().external_consistency_mode() != COMMIT_WAIT) {
    safe_time = HybridTime(replicate->hybrid_time());
  // ... else we set the safe hybrid_time to be the transaction's hybrid_time minus the maximum
  // clock error. This opens the door for problems if the flags changed across reboots, but this is
  // unlikely and the problem would manifest itself immediately and clearly (mvcc would complain
  // the operation is already committed, with a CHECK failure).
  } else {
    DCHECK(data_.clock->SupportsExternalConsistencyMode(COMMIT_WAIT))
        << "The provided clock does not support COMMIT_WAIT external consistency mode.";
    safe_time = server::HybridClock::AddPhysicalTimeToHybridTime(
        HybridTime(replicate->hybrid_time()),
        MonoDelta::FromMicroseconds(-FLAGS_max_clock_sync_error_usec));
  }
  tablet_->mvcc_manager()->OfflineAdjustSafeTime(safe_time);

  return Status::OK();
}

void TabletBootstrap::DumpReplayStateToLog(const ReplayState& state) {
  // Dump the replay state, this will log the pending replicates as well as the pending commits,
  // which might be useful for debugging.
  vector<string> state_dump;
  state.DumpReplayStateToStrings(&state_dump);
  constexpr int kMaxLinesToDump = 1000;
  static_assert(kMaxLinesToDump % 2 == 0, "Expected kMaxLinesToDump to be even");
  if (state_dump.size() <= kMaxLinesToDump) {
    for (const string& line : state_dump) {
      LOG_WITH_PREFIX(INFO) << line;
    }
  } else {
    int i = 0;
    for (const string& line : state_dump) {
      LOG_WITH_PREFIX(INFO) << line;
      if (++i >= kMaxLinesToDump / 2) break;
    }
    LOG_WITH_PREFIX(INFO) << "(" << state_dump.size() - kMaxLinesToDump << " lines skipped)";
    for (i = state_dump.size() - kMaxLinesToDump / 2; i < state_dump.size(); ++i) {
      LOG_WITH_PREFIX(INFO) << state_dump[i];
    }
  }
}

Status TabletBootstrap::PlaySegments(ConsensusBootstrapInfo* consensus_info) {
  // We initialize state->rocksdb_applied_index with MaxPersistentSequenceNumber(), and only apply
  // a log entry with index equal to state->rocksdb_applied_index, before incrementing that
  // variable. Together with the check based on state->committed_op_id, this ensures we apply
  // all committed entries in the right order, even when the Raft index of entries we encounter in
  // the log jumps back and the term gets increased due to leader changes and logical log
  // "truncation".
  bool non_kudu = tablet_->table_type() != TableType::KUDU_COLUMNAR_TABLE_TYPE;
  auto persistent_op_id = MinimumOpId();
  if (non_kudu) {
    auto flushed_op_id = tablet_->MaxPersistentOpId();
    persistent_op_id.set_term(flushed_op_id.term);
    persistent_op_id.set_index(flushed_op_id.index);
  }
  ReplayState state(persistent_op_id);

  if (non_kudu) {
    LOG_WITH_PREFIX(INFO) << "Max persistent index in RocksDB's SSTables before bootstrap: "
                          << state.last_stored_op_id.ShortDebugString();
  }

  log::SegmentSequence segments;
  RETURN_NOT_OK(log_reader_->GetSegmentsSnapshot(&segments));

  // The first thing to do is to rewind the tablet's schema back to the schema
  // as of the point in time where the logs begin. We must replay the writes
  // in the logs with the correct point-in-time schema.
  //
  // We only do this for legacy Kudu columnar-format tables. For QL tables,
  // the write transactions write docdb key values directly, without checking the schema.
  // This means that we can replay them without rewinding the schema. Entries for columns
  // that were deleted will still be cleaned up by the compaction filter.
  if (!segments.empty() && tablet_->table_type() == TableType::KUDU_COLUMNAR_TABLE_TYPE) {
    const scoped_refptr<ReadableLogSegment>& segment = segments[0];
    // Set the point-in-time schema for the tablet based on the log header.
    Schema pit_schema;
    RETURN_NOT_OK_PREPEND(SchemaFromPB(segment->header().schema(), &pit_schema),
                          "Couldn't decode log segment schema");
    RETURN_NOT_OK_PREPEND(tablet_->RewindSchemaForBootstrap(
                              pit_schema, segment->header().schema_version()),
                          "couldn't set point-in-time schema");
  }

  // We defer opening the log until here, so that we properly reproduce the
  // point-in-time schema from the log we're reading into the log we're
  // writing.
  RETURN_NOT_OK_PREPEND(OpenNewLog(), "Failed to open new log");

  int segment_count = 0;
  for (const scoped_refptr<ReadableLogSegment>& segment : segments) {
    log::LogEntries entries;
    // TODO: Optimize this to not read the whole thing into memory?
    Status read_status = segment->ReadEntries(&entries);
    for (int entry_idx = 0; entry_idx < entries.size(); ++entry_idx) {
      Status s = HandleEntry(&state, &entries[entry_idx]);
      if (!s.ok()) {
        LOG(INFO) << "Dumping replay state to log";
        DumpReplayStateToLog(state);
        RETURN_NOT_OK_PREPEND(s, DebugInfo(tablet_->tablet_id(),
                                           segment->header().sequence_number(),
                                           entry_idx, segment->path(),
                                           *entries[entry_idx]));
      }
    }

    // If the LogReader failed to read for some reason, we'll still try to
    // replay as many entries as possible, and then fail with Corruption.
    // TODO: this is sort of scary -- why doesn't LogReader expose an
    // entry-by-entry iterator-like API instead? Seems better to avoid
    // exposing the idea of segments to callers.
    if (PREDICT_FALSE(!read_status.ok())) {
      return STATUS(Corruption, Substitute("Error reading Log Segment of tablet $0: $1 "
                                           "(Read up to entry $2 of segment $3, in path $4)",
                                           tablet_->tablet_id(),
                                           read_status.ToString(),
                                           entries.size(),
                                           segment->header().sequence_number(),
                                           segment->path()));
    }

    // TODO: could be more granular here and log during the segments as well,
    // plus give info about number of MB processed, but this is better than
    // nothing.
    listener_->StatusMessage(Substitute("Bootstrap replayed $0/$1 log segments. "
                                        "Stats: $2. Pending: $3 replicates",
                                        segment_count + 1, log_reader_->num_segments(),
                                        stats_.ToString(),
                                        state.pending_replicates.size()));
    segment_count++;
  }

  // If we have non-applied commits they all must belong to pending operations and
  // they should only pertain to unflushed stores. This is specific to Kudu tables, because we don't
  // use local COMMIT messages in YB tables.
  if (!state.pending_commits.empty()) {
    for (const OpIndexToEntryMap::value_type& entry : state.pending_commits) {
      if (!ContainsKey(state.pending_replicates, entry.first)) {
        LOG(INFO) << "Dumping replay state to log";
        DumpReplayStateToLog(state);
        return STATUS(Corruption, "Had orphaned commits at the end of replay.");
      }
      if (AreAnyStoresAlreadyFlushed(entry.second->commit())) {
        LOG(INFO) << "Dumping replay state to log";
        DumpReplayStateToLog(state);
        TabletSuperBlockPB super;
        WARN_NOT_OK(meta_->ToSuperBlock(&super), "Couldn't build TabletSuperBlockPB.");
        return STATUS(Corruption, Substitute("CommitMsg was pending but it referred to "
            "flushed stores. Commit: $0. TabletMetadata: $1",
            entry.second->commit().ShortDebugString(), super.ShortDebugString()));
      }
    }
  }

  // Note that we don't pass the information contained in the pending commits along with
  // ConsensusBootstrapInfo. We know that this is safe as they must refer to unflushed
  // stores (we make doubly sure above).
  //
  // Example/Explanation:
  // Say we have two different operations that touch the same row, one insert and one
  // mutate. Since we use Early Lock Release the commit for the second (mutate) operation
  // might end up in the log before the insert's commit. This wouldn't matter since
  // we replay in order, but a corner case here is that we might crash before we
  // write the commit for the insert, meaning it might not be present at all.
  //
  // One possible log for this situation would be:
  // - Replicate 10.10 (insert)
  // - Replicate 10.11 (mutate)
  // - Commit    10.11 (mutate)
  // ~CRASH while Commit 10.10 is in-flight~
  //
  // We can't replay 10.10 during bootstrap because we haven't seen its commit, but
  // since we can't replay out-of-order we won't replay 10.11 either, in fact we'll
  // pass them both as "pending" to consensus to be applied again.
  //
  // The reason why it is safe to simply disregard 10.11's commit is that we know that
  // it must refer only to unflushed stores. We know this because one important flush/compact
  // pre-condition is:
  // - No flush will become visible on reboot (meaning we won't durably update the tablet
  //   metadata), unless the snapshot under which the flush/compact was performed has no
  //   in-flight transactions and all the messages that are in-flight to the log are durable.
  //
  // In our example this means that if we had flushed/compacted after 10.10 was applied
  // (meaning losing the commit message would lead to corruption as we might re-apply it)
  // then the commit for 10.10 would be durable. Since it isn't then no flush/compaction
  // occurred after 10.10 was applied and thus we can disregard the commit message for
  // 10.11 and simply apply both 10.10 and 10.11 as if we hadn't applied them before.
  //
  // This generalizes to:
  // - If a committed replicate message with index Y is missing a commit message,
  //   no later committed replicate message (with index > Y) is visible across reboots
  //   in the tablet data.

  LOG(INFO) << "Dumping replay state to log at the end of " << __FUNCTION__;
  DumpReplayStateToLog(state);

  // Set up the ConsensusBootstrapInfo structure for the caller.
  for (auto& e : state.pending_replicates) {
    // For RocksDB-backed tables, we only allow log entries with an index later than the index of
    // the last log entry already applied to RocksDB to be passed to the tablet as
    // "orphaned replicates". This will make sure we don't try to write to RocksDB with
    // non-monotonic sequence ids, but still create ConsensusRound instances for writes that have
    // not been persisted into RocksDB.
    if (tablet_->table_type() == TableType::KUDU_COLUMNAR_TABLE_TYPE ||
        e.first > state.rocksdb_applied_index) {
      consensus_info->orphaned_replicates.emplace_back(e.second->release_replicate());
    }
  }
  if (tablet_->table_type() != TableType::KUDU_COLUMNAR_TABLE_TYPE) {
    LOG(INFO) << "rocksdb_applied_index=" << state.rocksdb_applied_index
              << ", number of orphaned replicates=" << consensus_info->orphaned_replicates.size();
    // In case there were no log records that told us that the commit index advanced past
    // rocksdb_applied_index, update safe time with the timestamp of the latest log record that
    // had rocksdb_applied_index as its index, because that entry must be the one that was
    // committed.
    tablet_->mvcc_manager()->OfflineAdjustSafeTime(state.rocksdb_last_entry_hybrid_time);
    if (tablet_->mvcc_manager()->GetMaxSafeTimeToReadAt() == HybridTime::kMin &&
        state.rocksdb_applied_index > 0) {
      return STATUS(Corruption,
                    "Even though RocksDB is not empty, we were not able to set safe time "
                    "correctly on tablet bootstrap. Did we fail to keep at least one committed "
                    "entry in the log?");
    }
  }
  consensus_info->last_id = state.prev_op_id;
  consensus_info->last_committed_id = state.committed_op_id;

  return Status::OK();
}

Status TabletBootstrap::AppendCommitMsg(const CommitMsg& commit_msg) {
  LogEntryPB commit_entry;
  commit_entry.set_type(log::COMMIT);
  CommitMsg* commit = commit_entry.mutable_commit();
  commit->CopyFrom(commit_msg);
  return log_->Append(&commit_entry);
}

Status TabletBootstrap::PlayWriteRequest(ReplicateMsg* replicate_msg,
                                         const CommitMsg* commit_msg) {
  DCHECK(replicate_msg->has_hybrid_time());
  WriteRequestPB* write = replicate_msg->mutable_write_request();

  WriteOperationState operation_state(nullptr, write, nullptr);
  operation_state.mutable_op_id()->CopyFrom(replicate_msg->id());
  operation_state.set_hybrid_time(HybridTime(replicate_msg->hybrid_time()));

  tablet_->StartOperation(&operation_state);
  if (tablet_->table_type() == TableType::KUDU_COLUMNAR_TABLE_TYPE) {
    // In case of RocksDB-backed tables we will call ApplyRowOperations, which will itself call
    // StartApplying.
    tablet_->StartApplying(&operation_state);
  }

  // Use committed OpId for mem store anchoring.
  operation_state.mutable_op_id()->CopyFrom(replicate_msg->id());

  if (write->has_row_operations()) {
    DCHECK(!write->has_write_batch());
    DCHECK_EQ(tablet_->table_type(), TableType::KUDU_COLUMNAR_TABLE_TYPE);
  }

  if (write->has_write_batch()) {
    DCHECK(!write->has_row_operations());
    DCHECK_NE(tablet_->table_type(), TableType::KUDU_COLUMNAR_TABLE_TYPE);
  }

  if (write->has_row_operations() || write->has_write_batch()) {
    RETURN_NOT_OK(PlayRowOperations(&operation_state,
                                    commit_msg == nullptr ? nullptr : &commit_msg->result()));
  }

  if (commit_msg != nullptr) {
    // Append the commit msg to the log but replace the result with the new one.
    LogEntryPB commit_entry;
    commit_entry.set_type(log::COMMIT);
    CommitMsg* commit = commit_entry.mutable_commit();
    commit->CopyFrom(*commit_msg);
    operation_state.ReleaseTxResultPB(commit->mutable_result());
    RETURN_NOT_OK(log_->Append(&commit_entry));
  }

  return Status::OK();
}

Status TabletBootstrap::PlayAlterSchemaRequest(ReplicateMsg* replicate_msg,
                                               const CommitMsg* commit_msg) {
  AlterSchemaRequestPB* alter_schema = replicate_msg->mutable_alter_schema_request();

  // Decode schema
  Schema schema;
  RETURN_NOT_OK(SchemaFromPB(alter_schema->schema(), &schema));

  AlterSchemaOperationState operation_state(nullptr, alter_schema);

  // TODO(KUDU-860): we should somehow distinguish if an alter table failed on its original
  // attempt (e.g due to being an invalid request, or a request with a too-early
  // schema version).

  RETURN_NOT_OK(tablet_->CreatePreparedAlterSchema(&operation_state, &schema));

  // Apply the alter schema to the tablet
  RETURN_NOT_OK_PREPEND(tablet_->AlterSchema(&operation_state), "Failed to AlterSchema:");

  // Also update the log information. Normally, the AlterSchema() call above
  // takes care of this, but our new log isn't hooked up to the tablet yet.
  log_->SetSchemaForNextLogSegment(schema, operation_state.schema_version());

  return commit_msg == nullptr ? Status::OK() : AppendCommitMsg(*commit_msg);
}

Status TabletBootstrap::PlayChangeConfigRequest(ReplicateMsg* replicate_msg,
                                                const CommitMsg* commit_msg) {
  ChangeConfigRecordPB* change_config = replicate_msg->mutable_change_config_record();
  RaftConfigPB config = change_config->new_config();

  int64_t cmeta_opid_index =  cmeta_->committed_config().opid_index();
  if (replicate_msg->id().index() > cmeta_opid_index) {
    DCHECK(!config.has_opid_index());
    config.set_opid_index(replicate_msg->id().index());
    VLOG_WITH_PREFIX(1) << "WAL replay found Raft configuration with log index "
                        << config.opid_index()
                        << " that is greater than the committed config's index "
                        << cmeta_opid_index
                        << ". Applying this configuration change.";
    cmeta_->set_committed_config(config);
    // We flush once at the end of bootstrap.
  } else {
    VLOG_WITH_PREFIX(1) << "WAL replay found Raft configuration with log index "
                        << replicate_msg->id().index()
                        << ", which is less than or equal to the committed "
                        << "config's index " << cmeta_opid_index << ". "
                        << "Skipping application of this config change.";
  }

  return commit_msg == nullptr ? Status::OK() : AppendCommitMsg(*commit_msg);
}

Status TabletBootstrap::PlayNoOpRequest(ReplicateMsg* replicate_msg, const CommitMsg* commit_msg) {
  return commit_msg == nullptr ? Status::OK() : AppendCommitMsg(*commit_msg);
}

Status TabletBootstrap::PlayUpdateTransactionRequest(ReplicateMsg* replicate_msg,
                                                     const CommitMsg* commit_msg) {
  DCHECK(replicate_msg->has_hybrid_time());

  UpdateTxnOperationState operation_state(
      nullptr, replicate_msg->mutable_transaction_state());
  operation_state.mutable_op_id()->CopyFrom(replicate_msg->id());
  operation_state.set_hybrid_time(HybridTime(replicate_msg->hybrid_time()));

  auto transaction_coordinator = tablet_->transaction_coordinator();

  TransactionCoordinator::ReplicatedData replicated_data = {
      ProcessingMode::NON_LEADER,
      tablet_.get(),
      *operation_state.request(),
      operation_state.op_id(),
      operation_state.hybrid_time()
  };
  RETURN_NOT_OK(transaction_coordinator->ProcessReplicated(replicated_data));

  return Status::OK();
}

Status TabletBootstrap::PlayRowOperations(WriteOperationState* operation_state,
                                          const TxResultPB* result) {
  Schema inserts_schema;
  RETURN_NOT_OK_PREPEND(SchemaFromPB(operation_state->request()->schema(), &inserts_schema),
                        "Couldn't decode client schema");

  RETURN_NOT_OK_PREPEND(
      tablet_->DecodeWriteOperations(&inserts_schema, operation_state),
      Substitute("Could not decode row operations: $0",
                 operation_state->request()->row_operations().ShortDebugString()));

  if (result != nullptr) {
    CHECK_EQ(operation_state->row_ops().size(), result->ops_size());
  }

  switch (tablet_->table_type()) {
    case TableType::KUDU_COLUMNAR_TABLE_TYPE:
      RETURN_NOT_OK_PREPEND(tablet_->AcquireKuduRowLocks(operation_state),
                            "Failed to acquire row locks");
      RETURN_NOT_OK(FilterAndApplyOperations(operation_state, result));
      break;
    case TableType::YQL_TABLE_TYPE: FALLTHROUGH_INTENDED;
    case TableType::REDIS_TABLE_TYPE:
      tablet_->ApplyRowOperations(operation_state);
      break;
    default:
      LOG(FATAL) << "Invalid table type: " << tablet_->table_type();
  }

  return Status::OK();
}

Status TabletBootstrap::FilterAndApplyOperations(WriteOperationState* operation_state,
                                                 const TxResultPB* orig_result) {
  int32_t op_idx = 0;
  for (RowOp* op : operation_state->row_ops()) {
    const OperationResultPB* orig_op_result =
        orig_result == nullptr ? nullptr : &orig_result->ops(op_idx++);

    // check if the operation failed in the original transaction
    if (orig_op_result != nullptr && PREDICT_FALSE(orig_op_result->has_failed_status())) {
      Status status = StatusFromPB(orig_op_result->failed_status());
      if (VLOG_IS_ON(1)) {
        VLOG_WITH_PREFIX(1) << "Skipping operation that originally resulted in error. OpId: "
                            << operation_state->op_id().DebugString() << " op index: "
                            << op_idx - 1 << " original error: "
                            << status.ToString();
      }
      op->SetFailed(status);
      continue;
    }

    // Check if it should be filtered out because it's already flushed.
    switch (op->decoded_op.type) {
      case RowOperationsPB::INSERT:
        stats_.inserts_seen++;
        if (orig_op_result == nullptr || !orig_op_result->flushed()) {
          RETURN_NOT_OK(FilterInsert(operation_state, op, orig_op_result));
        } else {
          op->SetAlreadyFlushed();
          stats_.inserts_ignored++;
          continue;
        }
        break;
      case RowOperationsPB::UPDATE:
      case RowOperationsPB::DELETE:
        stats_.mutations_seen++;
        if (orig_op_result == nullptr || !orig_op_result->flushed()) {
          RETURN_NOT_OK(FilterMutate(operation_state, op, orig_op_result));
        } else {
          op->SetAlreadyFlushed();
          stats_.mutations_ignored++;
          continue;
        }
        break;
      default:
        LOG_WITH_PREFIX(FATAL) << "Bad op type: " << op->decoded_op.type;
        break;
    }
    if (op->result != nullptr) {
      continue;
    }

    // Actually apply it.
    tablet_->ApplyKuduRowOperation(operation_state, op);
    DCHECK(op->result != nullptr);

    // We expect that the above Apply() will always succeed, because we're
    // applying an operation that we know succeeded before the server
    // restarted. If it doesn't succeed, something is wrong and we are
    // diverging from our prior state, so bail.
    if (op->result->has_failed_status()) {
      return STATUS(Corruption, "Operation which previously succeeded failed "
                                "during log replay",
                                Substitute("Op: $0\nFailure: $1",
                                           op->ToString(*tablet_->schema()),
                                           op->result->failed_status().ShortDebugString()));
    }
  }
  return Status::OK();
}

Status TabletBootstrap::FilterInsert(WriteOperationState* operation_state,
                                     RowOp* op,
                                     const OperationResultPB* op_result) {
  DCHECK_EQ(op->decoded_op.type, RowOperationsPB::INSERT);
  if (op_result == nullptr) return Status::OK();

  if (PREDICT_FALSE(op_result->mutated_stores_size() != 1 ||
                    !op_result->mutated_stores(0).has_mrs_id())) {
    return STATUS(Corruption, Substitute("Insert operation result must have an mrs_id: $0",
                                         op_result->ShortDebugString()));
  }
  // check if the insert is already flushed
  if (flushed_stores_.WasStoreAlreadyFlushed(op_result->mutated_stores(0))) {
    if (VLOG_IS_ON(1)) {
      VLOG_WITH_PREFIX(1) << "Skipping insert that was already flushed. OpId: "
                          << operation_state->op_id().DebugString()
                          << " flushed to: " << op_result->mutated_stores(0).mrs_id()
                          << " latest durable mrs id: "
                          << tablet_->metadata()->last_durable_mrs_id();
    }

    op->SetAlreadyFlushed();
    stats_.inserts_ignored++;
  }
  return Status::OK();
}

Status TabletBootstrap::FilterMutate(WriteOperationState* operation_state,
                                     RowOp* op,
                                     const OperationResultPB* op_result) {
  DCHECK(op->decoded_op.type == RowOperationsPB::UPDATE ||
         op->decoded_op.type == RowOperationsPB::DELETE)
    << RowOperationsPB::Type_Name(op->decoded_op.type);
  if (op_result == nullptr) return Status::OK();

  int num_mutated_stores = op_result->mutated_stores_size();
  if (PREDICT_FALSE(num_mutated_stores == 0 || num_mutated_stores > 2)) {
    return STATUS(Corruption, Substitute("Mutations must have one or two mutated_stores: $0",
                                         op_result->ShortDebugString()));
  }

  // The mutation may have been duplicated, so we'll check whether any of the
  // output targets was "unflushed".
  int num_unflushed_stores = 0;
  for (const MemStoreTargetPB& mutated_store : op_result->mutated_stores()) {
    if (!flushed_stores_.WasStoreAlreadyFlushed(mutated_store)) {
      num_unflushed_stores++;
    } else {
      if (VLOG_IS_ON(1)) {
        string mutation = op->decoded_op.changelist.ToString(*tablet_->schema());
        VLOG_WITH_PREFIX(1) << "Skipping mutation to " << mutated_store.ShortDebugString()
                            << " that was already flushed. "
                            << "OpId: " << operation_state->op_id().DebugString();
      }
    }
  }

  if (num_unflushed_stores == 0) {
    // The mutation was fully flushed.
    op->SetFailed(STATUS(AlreadyPresent, "Update was already flushed."));
    stats_.mutations_ignored++;
    return Status::OK();
  }

  if (num_unflushed_stores == 2) {
    // 18:47 < dralves> off the top of my head, if we crashed before writing the meta
    //                  at the end of a flush/compation then both mutations could
    //                  potentually be considered unflushed
    // This case is not currently covered by any tests -- we need to add test coverage
    // for this. See KUDU-218. It's likely the correct behavior is just to apply the edit,
    // ie not fatal below.
    LOG_WITH_PREFIX(DFATAL) << "TODO: add test coverage for case where op is unflushed "
                            << "in both duplicated targets";
  }

  return Status::OK();
}

void TabletBootstrap::UpdateClock(uint64_t hybrid_time) {
  data_.clock->Update(HybridTime(hybrid_time));
}

string TabletBootstrap::LogPrefix() const {
  return Substitute("T $0 P $1: ", meta_->tablet_id(), meta_->fs_manager()->uuid());
}

// ============================================================================
//  Class FlushedStoresSnapshot.
// ============================================================================
Status FlushedStoresSnapshot::InitFrom(const TabletMetadata& meta) {
  CHECK(flushed_dms_by_drs_id_.empty()) << "already initted";
  last_durable_mrs_id_ = meta.last_durable_mrs_id();
  for (const shared_ptr<RowSetMetadata>& rsmd : meta.rowsets()) {
    if (!InsertIfNotPresent(&flushed_dms_by_drs_id_, rsmd->id(),
                            rsmd->last_durable_redo_dms_id())) {
      return STATUS(Corruption, Substitute(
          "Duplicate DRS ID $0 in tablet metadata. "
          "Found DRS $0 with last durable redo DMS ID $1 while trying to "
          "initialize DRS $0 with last durable redo DMS ID $2",
          rsmd->id(),
          flushed_dms_by_drs_id_[rsmd->id()],
          rsmd->last_durable_redo_dms_id()));
    }
  }
  return Status::OK();
}

bool FlushedStoresSnapshot::WasStoreAlreadyFlushed(const MemStoreTargetPB& target) const {
  if (target.has_mrs_id()) {
    DCHECK(!target.has_rs_id());
    DCHECK(!target.has_dms_id());

    // The original mutation went to the MRS. It is flushed if it went to an MRS
    // with a lower ID than the latest flushed one.
    return target.mrs_id() <= last_durable_mrs_id_;
  } else {
    // The original mutation went to a DRS's delta store.
    int64_t last_durable_dms_id;
    if (!FindCopy(flushed_dms_by_drs_id_, target.rs_id(), &last_durable_dms_id)) {
      // if we have no data about this RowSet, then it must have been flushed and
      // then deleted.
      // TODO: how do we avoid a race where we get an update on a rowset before
      // it is persisted? add docs about the ordering of flush.
      return true;
    }

    // If the original rowset that we applied the edit to exists, check whether
    // the edit was in a flushed DMS or a live one.
    if (target.dms_id() <= last_durable_dms_id) {
      return true;
    }

    return false;
  }
}

// ============================================================================
//  Class TabletBootstrap::Stats.
// ============================================================================
string TabletBootstrap::Stats::ToString() const {
  return Substitute("ops{read=$0 overwritten=$1 applied=$2} "
                    "inserts{seen=$3 ignored=$4} "
                    "mutations{seen=$5 ignored=$6} "
                    "orphaned_commits=$7",
                    ops_read, ops_overwritten, ops_committed,
                    inserts_seen, inserts_ignored,
                    mutations_seen, mutations_ignored,
                    orphaned_commits);
}

} // namespace tablet
} // namespace yb
