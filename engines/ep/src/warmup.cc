/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2012-Present Couchbase, Inc.
 *
 *   Use of this software is governed by the Business Source License included
 *   in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
 *   in that file, in accordance with the Business Source License, use of this
 *   software will be governed by the Apache License, Version 2.0, included in
 *   the file licenses/APL2.txt.
 */

#include "warmup.h"

#include "bucket_logger.h"
#include "callbacks.h"
#include "checkpoint_manager.h"
#include "collections/collection_persisted_stats.h"
#include "collections/manager.h"
#include "collections/vbucket_manifest_handles.h"
#include "ep_bucket.h"
#include "ep_engine.h"
#include "ep_vb.h"
#include "failover-table.h"
#include "flusher.h"
#include "item.h"
#include "kvstore/kvstore.h"
#include "mutation_log.h"
#include "vb_visitors.h"
#include "vbucket_bgfetch_item.h"
#include "vbucket_state.h"
#include <executor/executorpool.h>
#include <phosphor/phosphor.h>
#include <platform/dirutils.h>
#include <platform/timeutils.h>
#include <statistics/cbstat_collector.h>
#include <utilities/logtags.h>
#include <array>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <utility>

struct WarmupCookie {
    WarmupCookie(EPBucket* s, StatusCallback<GetValue>& c)
        : cb(c), epstore(s), loaded(0), skipped(0), error(0) { /* EMPTY */
    }
    StatusCallback<GetValue>& cb;
    EPBucket* epstore;
    size_t loaded;
    size_t skipped;
    size_t error;
};

void logWarmupStats(EPBucket& epstore) {
    EPStats& stats = epstore.getEPEngine().getEpStats();
    std::chrono::duration<double, std::chrono::seconds::period> seconds =
            epstore.getWarmup()->getTime();
    double keys_per_seconds = stats.warmedUpValues / seconds.count();
    double megabytes = stats.getPreciseTotalMemoryUsed() / 1.0e6;
    double megabytes_per_seconds = megabytes / seconds.count();
    EP_LOG_INFO(
            "Warmup completed: {} keys and {} values loaded in {} ({} keys/s), "
            "mem_used now at {} MB ({} MB/s)",
            stats.warmedUpKeys,
            stats.warmedUpValues,
            cb::time2text(
                    std::chrono::nanoseconds(epstore.getWarmup()->getTime())),
            keys_per_seconds,
            megabytes,
            megabytes_per_seconds);
}

//////////////////////////////////////////////////////////////////////////////
//                                                                          //
//    Helper class used to insert data into the epstore                     //
//                                                                          //
//////////////////////////////////////////////////////////////////////////////

/**
 * Helper class used to insert items into the storage by using
 * the KVStore::dump method to load items from the database
 */
class LoadStorageKVPairCallback : public StatusCallback<GetValue> {
public:
    LoadStorageKVPairCallback(
            EPBucket& ep,
            bool maybeEnableTraffic,
            WarmupState::State warmupState,
            std::optional<const std::chrono::steady_clock::duration>
                    deltaDeadlineFromNow = std::nullopt);

    void callback(GetValue& val) override;

    void updateDeadLine() {
        if (deltaDeadlineFromNow) {
            deadline =
                    (std::chrono::steady_clock::now() + *deltaDeadlineFromNow);
            pausedDueToDeadLine = false;
        }
    };

    bool isPausedDueToDeadLine() const {
        return pausedDueToDeadLine;
    };

private:
    bool shouldEject() const;

    void purge();

    VBucketMap& vbuckets;
    EPStats& stats;
    EPBucket& epstore;
    bool hasPurged;
    std::optional<const std::chrono::steady_clock::duration>
            deltaDeadlineFromNow;
    std::chrono::steady_clock::time_point deadline;
    bool pausedDueToDeadLine = false;

    /// If true, call EPBucket::maybeEnableTraffic() after each KV pair loaded.
    const bool maybeEnableTraffic;
    WarmupState::State warmupState;
};

using CacheLookupCallBackPtr = std::unique_ptr<StatusCallback<CacheLookup>>;

class LoadValueCallback : public StatusCallback<CacheLookup> {
public:
    LoadValueCallback(VBucketMap& vbMap, WarmupState::State warmupState)
        : vbuckets(vbMap), warmupState(warmupState) {
    }

    void callback(CacheLookup& lookup) override;

private:
    VBucketMap& vbuckets;
    WarmupState::State warmupState;
};

// Warmup Tasks ///////////////////////////////////////////////////////////////

class WarmupInitialize : public GlobalTask {
public:
    WarmupInitialize(EPBucket& st, Warmup* w)
        : GlobalTask(&st.getEPEngine(), TaskId::WarmupInitialize, 0, false),
          _warmup(w) {
        _warmup->addToTaskSet(uid);
    }

    std::string getDescription() const override {
        return "Warmup - initialize";
    }

    std::chrono::microseconds maxExpectedDuration() const override {
        // Typically takes single-digits ms.
        return std::chrono::milliseconds(50);
    }

    bool run() override {
        TRACE_EVENT0("ep-engine/task", "WarmupInitialize");
        _warmup->initialize();
        _warmup->removeFromTaskSet(uid);
        return false;
    }

private:
    Warmup* _warmup;
};

class WarmupCreateVBuckets : public GlobalTask {
public:
    WarmupCreateVBuckets(EPBucket& st, uint16_t sh, Warmup* w)
        : GlobalTask(&st.getEPEngine(), TaskId::WarmupCreateVBuckets, 0, false),
          _shardId(sh),
          _warmup(w),
          _description("Warmup - creating vbuckets: shard " +
                       std::to_string(_shardId)) {
        _warmup->addToTaskSet(uid);
    }

    std::string getDescription() const override {
        return _description;
    }

    std::chrono::microseconds maxExpectedDuration() const override {
        // VB creation typically takes some 10s of milliseconds.
        return std::chrono::milliseconds(100);
    }

    bool run() override {
        TRACE_EVENT0("ep-engine/task", "WarmupCreateVBuckets");
        _warmup->createVBuckets(_shardId);
        _warmup->removeFromTaskSet(uid);
        return false;
    }

private:
    uint16_t _shardId;
    Warmup* _warmup;
    const std::string _description;
};

class WarmupLoadingCollectionCounts : public GlobalTask {
public:
    WarmupLoadingCollectionCounts(EPBucket& st, uint16_t sh, Warmup& w)
        : GlobalTask(&st.getEPEngine(),
                     TaskId::WarmupLoadingCollectionCounts,
                     0,
                     false),
          shardId(sh),
          warmup(w) {
        warmup.addToTaskSet(uid);
    }

    std::string getDescription() const override {
        return "Warmup - loading collection counts: shard " +
               std::to_string(shardId);
    }

    std::chrono::microseconds maxExpectedDuration() const override {
        // This task has to open each VB's data-file and (certainly for
        // couchstore) read a small document per defined collection
        return std::chrono::seconds(10);
    }

    bool run() override {
        TRACE_EVENT0("ep-engine/task", "WarmupLoadingCollectionCounts");
        warmup.loadCollectionStatsForShard(shardId);
        warmup.removeFromTaskSet(uid);
        return false;
    }

private:
    uint16_t shardId;
    Warmup& warmup;
};

class WarmupEstimateDatabaseItemCount : public GlobalTask {
public:
    WarmupEstimateDatabaseItemCount(EPBucket& st, uint16_t sh, Warmup* w)
        : GlobalTask(&st.getEPEngine(),
                     TaskId::WarmupEstimateDatabaseItemCount,
                     0,
                     false),
          _shardId(sh),
          _warmup(w),
          _description("Warmup - estimate item count: shard " +
                       std::to_string(_shardId)) {
        _warmup->addToTaskSet(uid);
    }

    std::string getDescription() const override {
        return _description;
    }

    std::chrono::microseconds maxExpectedDuration() const override {
        // Typically takes a few 10s of milliseconds (need to open kstore files
        // and read statistics.
        return std::chrono::milliseconds(100);
    }

    bool run() override {
        TRACE_EVENT0("ep-engine/task", "WarpupEstimateDatabaseItemCount");
        _warmup->estimateDatabaseItemCount(_shardId);
        _warmup->removeFromTaskSet(uid);
        return false;
    }

private:
    uint16_t _shardId;
    Warmup* _warmup;
    const std::string _description;
};

/**
 * Warmup task which loads any prepared SyncWrites which are not yet marked
 * as Committed (or Aborted) from disk.
 */
class WarmupLoadPreparedSyncWrites : public GlobalTask {
public:
    WarmupLoadPreparedSyncWrites(EventuallyPersistentEngine* engine,
                                 uint16_t shard,
                                 Warmup& warmup)
        : GlobalTask(engine, TaskId::WarmupLoadPreparedSyncWrites, 0, false),
          shardId(shard),
          warmup(warmup),
          description("Warmup - loading prepared SyncWrites: shard " +
                      std::to_string(shardId)){};

    std::string getDescription() const override {
        return description;
    }

    std::chrono::microseconds maxExpectedDuration() const override {
        // Runtime is a function of how many prepared sync writes exist in the
        // buckets for this shard - can be minutes in large datasets.
        // Given this large variation; set max duration to a "way out" value
        // which we don't expect to see.
        return std::chrono::minutes(10);
    }

    bool run() override {
        TRACE_EVENT1("ep-engine/task",
                     "WarmupLoadPreparedSyncWrites",
                     "shard",
                     shardId);
        warmup.loadPreparedSyncWrites(shardId);
        warmup.removeFromTaskSet(uid);
        return false;
    }

private:
    uint16_t shardId;
    Warmup& warmup;
    const std::string description;
};

/**
 * Warmup task which moves all warmed-up VBuckets into the bucket's vbMap
 */
class WarmupPopulateVBucketMap : public GlobalTask {
public:
    WarmupPopulateVBucketMap(EPBucket& st, uint16_t shard, Warmup& warmup)
        : GlobalTask(&st.getEPEngine(),
                     TaskId::WarmupPopulateVBucketMap,
                     0,
                     false),
          shardId(shard),
          warmup(warmup),
          description("Warmup - populate VB Map: shard " +
                      std::to_string(shardId)){};

    std::string getDescription() const override {
        return description;
    }

    std::chrono::microseconds maxExpectedDuration() const override {
        // Runtime is expected to be quick, we're just adding pointers to a map
        // with some locking
        return std::chrono::milliseconds(1);
    }

    bool run() override {
        TRACE_EVENT1(
                "ep-engine/task", "WarmupPopulateVBucketMap", "shard", shardId);
        warmup.populateVBucketMap(shardId);
        warmup.removeFromTaskSet(uid);
        return false;
    }

private:
    uint16_t shardId;
    Warmup& warmup;
    const std::string description;
};

class WarmupBackfillTask;
/**
 * Implementation of a PauseResumeVBVisitor to be used for the
 * WarmupBackfillTask WarmupVbucketVisitor keeps record of the current vbucket
 * being backfilled and the current state of scan context.
 */
class WarmupVbucketVisitor : public PauseResumeVBVisitor {
public:
    WarmupVbucketVisitor(EPBucket& ep, const WarmupBackfillTask& task)
        : ep(ep), backfillTask(task){};

    bool visit(VBucket& vb) override;

private:
    EPBucket& ep;
    bool needToScanAgain = false;
    const WarmupBackfillTask& backfillTask;
    std::unique_ptr<BySeqnoScanContext> currentScanCtx;
};

/**
 * Abstract Task to perform a backfill during warmup on a shards vbuckets, in a
 * pause-resume fashion.
 *
 * The task will also transition the warmup's state to the next warmup state
 * once threadTaskCount has meet the total number of shards.
 */
class WarmupBackfillTask : public GlobalTask {
public:
    /**
     * Constructor of WarmupBackfillTask
     * @param bucket EPBucket the task is back filling for
     * @param shardId of the shard we're performing the backfill on
     * @param warmup ref to the warmup class the backfill is for
     * @param taskId of the the backfill that is to be performed
     * @param taskDesc description of the task
     * @param threadTaskCount ref to atomic size_t that keeps count of how many
     * of the per tasks shards have been completed. If this value is equal to
     * the number of shards the run() method will transition warmup to the next
     * state.
     */
    WarmupBackfillTask(EPBucket& bucket,
                       size_t shardId,
                       Warmup& warmup,
                       TaskId taskId,
                       std::string_view taskDesc,
                       std::atomic<size_t>& threadTaskCount)
        : GlobalTask(&bucket.getEPEngine(), taskId, 0, true),
          warmup(warmup),
          shardId(shardId),
          description(fmt::format("Warmup - {} shard {}", taskDesc, shardId)),
          currentNumBackfillTasks(threadTaskCount),
          filter(warmup.shardVbIds[shardId]),
          visitor(bucket, *this),
          epStorePosition(bucket.startPosition()) {
        warmup.addToTaskSet(uid);
    }

    std::string getDescription() const override {
        return description;
    }

    std::chrono::microseconds maxExpectedDuration() const override {
        /**
         * Empirical testing using perf_bucket_warmup() in ep_perfsuite has
         * shown that 10ms is a sweet spot for back filling maxDuration as it
         * allows ~1000 items to be loaded before meeting the deadline and
         * doesn't show a regression as compared with before the back filling
         * tasks being performed in a pause/resume fashion.
         */
        return std::chrono::milliseconds(10);
    }

    bool run() override {
        TRACE_EVENT1(
                "ep-engine/task", "WarmupBackfillTask", "shard", getShardId());
        if (filter.empty() || engine->getEpStats().isShutdown) {
            // Technically "isShutdown" being true doesn't equate to a
            // successful task finish, however if we are shutting down we want
            // warmup to advance and be considered "done".
            finishTask(true);
            return false;
        }

        auto& kvBucket = *engine->getKVBucket();
        try {
            epStorePosition = kvBucket.pauseResumeVisit(
                    visitor, epStorePosition, &filter);
        } catch (std::exception& e) {
            EP_LOG_CRITICAL(
                    "WarmupBackfillTask::run(): caught exception while running "
                    "backfill - aborting warmup: {}",
                    e.what());
            finishTask(false);
            return false;
        }
        if (epStorePosition == kvBucket.endPosition()) {
            finishTask(true);
            return false;
        }

        return true;
    }

    size_t getShardId() const {
        return shardId;
    };

    Warmup& getWarmup() const {
        return warmup;
    };

    virtual WarmupState::State getNextState() const = 0;
    virtual ValueFilter getValueFilter() const = 0;
    virtual bool maybeEnableTraffic() const = 0;
    virtual CacheLookupCallBackPtr makeCacheLookupCallback() const = 0;

protected:
    Warmup& warmup;

private:
    /**
     * Finish the current task, transitioning to the next phase of warmup if
     * backfill has successfully finished for all shards.
     * @param success True if task finished successfully, else false.
     */
    void finishTask(bool success) {
        warmup.removeFromTaskSet(uid);
        if (!success) {
            // Unsuccessful task runs don't count against required task
            // completions.
            return;
        }
        // If this is the last backfill task (all shards have finished) then
        // move us to the next state.
        if (++currentNumBackfillTasks ==
            engine->getKVBucket()->getVBuckets().getNumShards()) {
            warmup.transition(getNextState());
        }
    }

    const size_t shardId;
    const std::string description;
    std::atomic<size_t>& currentNumBackfillTasks;
    VBucketFilter filter;
    WarmupVbucketVisitor visitor;
    KVBucketIface::Position epStorePosition;
};

bool WarmupVbucketVisitor::visit(VBucket& vb) {
    auto* kvstore = ep.getROUnderlyingByShard(backfillTask.getShardId());

    if (!currentScanCtx) {
        auto kvLookup = std::make_unique<LoadStorageKVPairCallback>(
                ep,
                backfillTask.maybeEnableTraffic(),
                backfillTask.getWarmup().getWarmupState(),
                backfillTask.maxExpectedDuration());
        currentScanCtx = kvstore->initBySeqnoScanContext(
                std::move(kvLookup),
                backfillTask.makeCacheLookupCallback(),
                vb.getId(),
                0,
                DocumentFilter::NO_DELETES,
                backfillTask.getValueFilter(),
                SnapshotSource::Head);
        if (!currentScanCtx) {
            throw std::runtime_error(fmt::format(
                    "WarmupVbucketVisitor::visit(): {} shardId:{} failed to "
                    "create BySeqnoScanContext, for backfill task:'{}'",
                    vb.getId(),
                    backfillTask.getShardId(),
                    backfillTask.getDescription()));
        }
    }
    // Update backfill deadline for when we need to next pause
    auto kvCallback = dynamic_cast<LoadStorageKVPairCallback&>(
            currentScanCtx->getValueCallback());
    kvCallback.updateDeadLine();

    ep.getEPEngine().hangWarmupHook();

    auto errorCode = kvstore->scan(*currentScanCtx);
    switch (errorCode) {
    case scan_success:
        // Finished backfill for this vbucket so we need to reset the scan ctx,
        // so that we can create a scan ctx for the next vbucket.
        currentScanCtx.reset();
        needToScanAgain = false;
        return true;

    case scan_again:
        needToScanAgain = kvCallback.isPausedDueToDeadLine();
        // if the 'scan_again' was due to a OOM (e.i. not due to our deadline
        // being met)causing warmup to be completed then log this and return
        // false as we shouldn't keep scanning this vbucket
        if (!needToScanAgain) {
            // skip loading remaining VBuckets as memory limit was reached
            EP_LOG_INFO(
                    "WarmupVbucketVisitor::visit(): {} shardId:{} "
                    "lastReadSeqno:{} needToScanAgain:{} vbucket "
                    "memory limit has been reached",
                    vb.getId(),
                    backfillTask.getShardId(),
                    currentScanCtx->lastReadSeqno,
                    needToScanAgain);
            // Backfill canceled due to OOM so destroy the scan ctx
            currentScanCtx.reset();
        }
        return !needToScanAgain;

    case scan_failed:
        // Disk error scanning keys - cannot continue warmup.
        currentScanCtx.reset();
        throw std::runtime_error(fmt::format(
                "WarmupVbucketVisitor::visit(): {} shardId:{} failed to "
                "scan BySeqnoScanContext, for backfill task:'{}'",
                vb.getId(),
                backfillTask.getShardId(),
                backfillTask.getDescription()));
    }
    folly::assume_unreachable();
}

/**
 * [Value-eviction only]
 * Task that loads all keys into memory for each vBucket in the given shard in a
 * pause resume fashion.
 */
class WarmupKeyDump : public WarmupBackfillTask {
public:
    WarmupKeyDump(EPBucket& bucket,
                  size_t shardId,
                  Warmup& warmup,
                  std::atomic<size_t>& threadTaskCount)
        : WarmupBackfillTask(bucket,
                             shardId,
                             warmup,
                             TaskId::WarmupKeyDump,
                             "key dump",
                             threadTaskCount){};

    WarmupState::State getNextState() const override {
        return WarmupState::State::CheckForAccessLog;
    }

    ValueFilter getValueFilter() const override {
        return ValueFilter::KEYS_ONLY;
    };

    bool maybeEnableTraffic() const override {
        return false;
    };

    CacheLookupCallBackPtr makeCacheLookupCallback() const override {
        return std::make_unique<NoLookupCallback>();
    };
};

class WarmupCheckforAccessLog : public GlobalTask {
public:
    WarmupCheckforAccessLog(EPBucket& st, Warmup* w)
        : GlobalTask(
                  &st.getEPEngine(), TaskId::WarmupCheckforAccessLog, 0, false),
          _warmup(w) {
        _warmup->addToTaskSet(uid);
    }

    std::string getDescription() const override {
        return "Warmup - check for access log";
    }

    std::chrono::microseconds maxExpectedDuration() const override {
        // Checking for the access log is a disk task (so can take a variable
        // amount of time), however it should be relatively quick as we are
        // just checking files exist.
        return std::chrono::milliseconds(100);
    }

    bool run() override {
        TRACE_EVENT0("ep-engine/task", "WarmupCheckForAccessLog");
        _warmup->checkForAccessLog();
        _warmup->removeFromTaskSet(uid);
        return false;
    }

private:
    Warmup* _warmup;
};

class WarmupLoadAccessLog : public GlobalTask {
public:
    WarmupLoadAccessLog(EPBucket& st, uint16_t sh, Warmup* w)
        : GlobalTask(&st.getEPEngine(), TaskId::WarmupLoadAccessLog, 0, false),
          _shardId(sh),
          _warmup(w),
          _description("Warmup - loading access log: shard " +
                       std::to_string(_shardId)) {
        _warmup->addToTaskSet(uid);
    }

    std::string getDescription() const override {
        return _description;
    }

    std::chrono::microseconds maxExpectedDuration() const override {
        // Runtime is a function of the number of keys in the access log files;
        // can be many minutes in large datasets.
        // Given this large variation; set max duration to a "way out" value
        // which we don't expect to see.
        return std::chrono::hours(1);
    }

    bool run() override {
        TRACE_EVENT0("ep-engine/task", "WarmupLoadAccessLog");
        _warmup->loadingAccessLog(_shardId);
        _warmup->removeFromTaskSet(uid);
        return false;
    }

private:
    uint16_t _shardId;
    Warmup* _warmup;
    const std::string _description;
};

/**
 * [Full-eviction only]
 * Task that loads both keys and values into memory for each vBucket in the
 * given shard in a pause resume fashion.
 */
class WarmupLoadingKVPairs : public WarmupBackfillTask {
public:
    WarmupLoadingKVPairs(EPBucket& bucket,
                         size_t shardId,
                         Warmup& warmup,
                         std::atomic<size_t>& threadTaskCount)
        : WarmupBackfillTask(bucket,
                             shardId,
                             warmup,
                             TaskId::WarmupLoadingKVPairs,
                             "loading KV Pairs",
                             threadTaskCount){};

    WarmupState::State getNextState() const override {
        return WarmupState::State::Done;
    };

    ValueFilter getValueFilter() const override {
        return warmup.store.getValueFilterForCompressionMode();
    };

    bool maybeEnableTraffic() const override {
        return warmup.store.getItemEvictionPolicy() == EvictionPolicy::Full;
    };

    CacheLookupCallBackPtr makeCacheLookupCallback() const override {
        return std::make_unique<LoadValueCallback>(warmup.store.vbMap,
                                                   warmup.getWarmupState());
    };
};

/**
 * Task that loads values into memory for each vBucket in the given shard in a
 * pause resume fashion.
 */
class WarmupLoadingData : public WarmupBackfillTask {
public:
    WarmupLoadingData(EPBucket& bucket,
                      size_t shardId,
                      Warmup& warmup,
                      std::atomic<size_t>& threadTaskCount)
        : WarmupBackfillTask(bucket,
                             shardId,
                             warmup,
                             TaskId::WarmupLoadingData,
                             "loading data",
                             threadTaskCount){};

    WarmupState::State getNextState() const override {
        return WarmupState::State::Done;
    };

    ValueFilter getValueFilter() const override {
        return warmup.store.getValueFilterForCompressionMode();
    };

    bool maybeEnableTraffic() const override {
        return true;
    };

    CacheLookupCallBackPtr makeCacheLookupCallback() const override {
        return std::make_unique<LoadValueCallback>(warmup.store.vbMap,
                                                   warmup.getWarmupState());
    };
};

class WarmupCompletion : public GlobalTask {
public:
    WarmupCompletion(EPBucket& st, Warmup* w)
        : GlobalTask(&st.getEPEngine(), TaskId::WarmupCompletion, 0, false),
          _warmup(w) {
        _warmup->addToTaskSet(uid);
    }

    std::string getDescription() const override {
        return "Warmup - completion";
    }

    std::chrono::microseconds maxExpectedDuration() const override {
        // This task should be very quick - just the final warmup steps.
        return std::chrono::milliseconds(1);
    }

    bool run() override {
        TRACE_EVENT0("ep-engine/task", "WarmupCompletion");
        _warmup->done();
        _warmup->removeFromTaskSet(uid);
        return false;
    }

private:
    Warmup* _warmup;
};

static bool batchWarmupCallback(Vbid vbId,
                                const std::set<StoredDocKey>& fetches,
                                void* arg) {
    auto *c = static_cast<WarmupCookie *>(arg);

    if (!c->epstore->maybeEnableTraffic()) {
        vb_bgfetch_queue_t items2fetch;
        for (auto& key : fetches) {
            // Access log only records Committed keys, therefore construct
            // DiskDocKey with pending == false.
            DiskDocKey diskKey{key, /*prepared*/ false};
            // Deleted below via a unique_ptr in the next loop
            vb_bgfetch_item_ctx_t& bg_itm_ctx = items2fetch[diskKey];
            bg_itm_ctx.addBgFetch(std::make_unique<FrontEndBGFetchItem>(
                    nullptr,
                    c->epstore->getValueFilterForCompressionMode(),
                    0));
        }

        c->epstore->getROUnderlying(vbId)->getMulti(vbId, items2fetch);

        // applyItem controls the  mode this loop operates in.
        // true we will attempt the callback (attempt a HashTable insert)
        // false we don't attempt the callback
        // in both cases the loop must delete the VBucketBGFetchItem we
        // allocated above.
        bool applyItem = true;
        for (auto& items : items2fetch) {
            vb_bgfetch_item_ctx_t& bg_itm_ctx = items.second;
            if (applyItem) {
                if (bg_itm_ctx.value.getStatus() == cb::engine_errc::success) {
                    // NB: callback will delete the GetValue's Item
                    c->cb.callback(bg_itm_ctx.value);
                } else {
                    EP_LOG_WARN(
                            "Warmup failed to load data for {}"
                            " key{{{}}} error = {}",
                            vbId,
                            cb::UserData{items.first.to_string()},
                            bg_itm_ctx.value.getStatus());
                    c->error++;
                }

                if (cb::engine_errc{c->cb.getStatus()} ==
                    cb::engine_errc::success) {
                    c->loaded++;
                } else {
                    // Failed to apply an Item, so fail the rest
                    applyItem = false;
                }
            } else {
                c->skipped++;
            }
        }

        return true;
    } else {
        c->skipped++;
        return false;
    }
}

const char *WarmupState::toString() const {
    return getStateDescription(state.load());
}

const char* WarmupState::getStateDescription(State st) const {
    switch (st) {
    case State::Initialize:
        return "initialize";
    case State::CreateVBuckets:
        return "creating vbuckets";
    case State::LoadingCollectionCounts:
        return "loading collection counts";
    case State::EstimateDatabaseItemCount:
        return "estimating database item count";
    case State::LoadPreparedSyncWrites:
        return "loading prepared SyncWrites";
    case State::PopulateVBucketMap:
        return "populating vbucket map";
    case State::KeyDump:
        return "loading keys";
    case State::CheckForAccessLog:
        return "determine access log availability";
    case State::LoadingAccessLog:
        return "loading access log";
    case State::LoadingKVPairs:
        return "loading k/v pairs";
    case State::LoadingData:
        return "loading data";
    case State::Done:
        return "done";
    }
    return "Illegal state";
}

void WarmupState::transition(State to, bool allowAnyState) {
    auto currentState = state.load();
    // If we're in the done state already this is a special case as it's always
    // our final state, which we may not transition from.
    if (currentState == State::Done) {
        return;
    }
    auto checkLegal = [this, &currentState, &to, &allowAnyState]() -> bool {
        if (allowAnyState || legalTransition(currentState, to)) {
            return true;
        }
        // Throw an exception to make it possible to test the logic ;)
        throw std::runtime_error(
                fmt::format("Illegal state transition from \"{}\" to {} ({})",
                            getStateDescription(currentState),
                            getStateDescription(to),
                            int(to)));
    };
    transitionHook();
    while (checkLegal() && !state.compare_exchange_weak(currentState, to)) {
        currentState = state.load();
        // If we're in the done state already this is a special case as it's
        // always our final state, which we may not transition from. It's
        // possible that the state has be set to Done by another threads, if
        // we're shutting down the bucket (See Warmup::stop() and is usage).
        if (currentState == State::Done) {
            break;
        }
    }
    EP_LOG_DEBUG("Warmup transition from state \"{}\" to \"{}\"",
                 getStateDescription(currentState),
                 getStateDescription(to));
}

bool WarmupState::legalTransition(State from, State to) const {
    switch (from) {
    case State::Initialize:
        return (to == State::CreateVBuckets);
    case State::CreateVBuckets:
        return (to == State::LoadingCollectionCounts);
    case State::LoadingCollectionCounts:
        return (to == State::EstimateDatabaseItemCount);
    case State::EstimateDatabaseItemCount:
        return (to == State::LoadPreparedSyncWrites);
    case State::LoadPreparedSyncWrites:
        return (to == State::PopulateVBucketMap);
    case State::PopulateVBucketMap:
        return (to == State::KeyDump || to == State::CheckForAccessLog);
    case State::KeyDump:
        return (to == State::LoadingKVPairs || to == State::CheckForAccessLog);
    case State::CheckForAccessLog:
        return (to == State::LoadingAccessLog || to == State::LoadingData ||
                to == State::LoadingKVPairs || to == State::Done);
    case State::LoadingAccessLog:
        return (to == State::Done || to == State::LoadingData);
    case State::LoadingKVPairs:
        return (to == State::Done);
    case State::LoadingData:
        return (to == State::Done);
    case State::Done:
        return false;
    }

    return false;
}

std::ostream& operator <<(std::ostream &out, const WarmupState &state)
{
    out << state.toString();
    return out;
}

LoadStorageKVPairCallback::LoadStorageKVPairCallback(
        EPBucket& ep,
        bool maybeEnableTraffic,
        WarmupState::State warmupState,
        std::optional<const std::chrono::steady_clock::duration>
                deltaDeadlineFromNow)
    : vbuckets(ep.vbMap),
      stats(ep.getEPEngine().getEpStats()),
      epstore(ep),
      hasPurged(false),
      deltaDeadlineFromNow(std::move(deltaDeadlineFromNow)),
      deadline(std::chrono::steady_clock::time_point::max()),
      maybeEnableTraffic(maybeEnableTraffic),
      warmupState(warmupState) {
}

void LoadStorageKVPairCallback::callback(GetValue& val) {
    if (deltaDeadlineFromNow && std::chrono::steady_clock::now() >= deadline) {
        pausedDueToDeadLine = true;
        // Use cb::engine_errc::no_memory to get KVStore to cancel the backfill
        setStatus(cb::engine_errc::no_memory);
        return;
    }

    // This callback method is responsible for deleting the Item
    std::unique_ptr<Item> i(std::move(val.item));

    // Don't attempt to load the system event documents.
    if (i->getKey().isInSystemCollection()) {
        return;
    }

    // Prepared SyncWrites are ignored here  -
    // they are handled in the earlier warmup State::LoadPreparedSyncWrites
    if (i->isPending()) {
        return;
    }

    bool stopLoading = false;
    if (i && !epstore.getWarmup()->isFinishedLoading()) {
        VBucketPtr vb = vbuckets.getBucket(i->getVBucketId());
        if (!vb) {
            setStatus(cb::engine_errc::not_my_vbucket);
            return;
        }
        bool succeeded(false);
        int retry = 2;
        do {
            if (i->getCas() == static_cast<uint64_t>(-1)) {
                if (val.isPartial()) {
                    i->setCas(0);
                } else {
                    i->setCas(vb->nextHLCCas());
                }
            }

            auto* epVb = dynamic_cast<EPVBucket*>(vb.get());
            if (!epVb) {
                setStatus(cb::engine_errc::not_my_vbucket);
                return;
            }

            const auto res = epVb->insertFromWarmup(*i,
                                                    shouldEject(),
                                                    val.isPartial(),
                                                    true /*check mem_used*/);
            switch (res) {
            case MutationStatus::NoMem:
                if (retry == 2) {
                    if (hasPurged) {
                        if (++stats.warmOOM == 1) {
                            EP_LOG_WARN(
                                    "LoadStorageKVPairCallback::callback(): {} "
                                    "Warmup dataload failure: max_size too "
                                    "low.",
                                    vb->getId());
                        }
                    } else {
                        EP_LOG_WARN(
                                "LoadStorageKVPairCallback::callback(): {} "
                                "Emergency startup purge to free space for "
                                "load.",
                                vb->getId());
                        purge();
                    }
                } else {
                    EP_LOG_WARN(
                            "LoadStorageKVPairCallback::callback(): {} "
                            "Cannot store an item after emergency purge.",
                            vb->getId());
                    ++stats.warmOOM;
                }
                break;
            case MutationStatus::InvalidCas:
                EP_LOG_DEBUG(
                        "LoadStorageKVPairCallback::callback(): {} "
                        "Value changed in memory before restore from disk. "
                        "Ignored disk value for: key{{{}}}.",
                        vb->getId(),
                        i->getKey().c_str());
                ++stats.warmDups;
                succeeded = true;
                break;
            case MutationStatus::NotFound:
                succeeded = true;
                break;
            default:
                throw std::logic_error(
                        "LoadStorageKVPairCallback::callback: "
                        "Unexpected result from HashTable::insert: " +
                        std::to_string(static_cast<uint16_t>(res)));
            }
        } while (!succeeded && retry-- > 0);

        if (maybeEnableTraffic) {
            stopLoading = epstore.maybeEnableTraffic();
        }

        switch (warmupState) {
        case WarmupState::State::KeyDump:
            if (stats.warmOOM) {
                epstore.getWarmup()->setOOMFailure();
                stopLoading = true;
            } else {
                ++stats.warmedUpKeys;
            }
            break;
        case WarmupState::State::LoadingData:
        case WarmupState::State::LoadingAccessLog:
            if (epstore.getItemEvictionPolicy() == EvictionPolicy::Full) {
                ++stats.warmedUpKeys;
            }
            ++stats.warmedUpValues;
            break;
        default:
            ++stats.warmedUpKeys;
            ++stats.warmedUpValues;
        }
    } else {
        stopLoading = true;
    }

    if (stopLoading) {
        // warmup has completed, return cb::engine_errc::no_memory to
        // cancel remaining data dumps from couchstore
        if (epstore.getWarmup()->setFinishedLoading()) {
            epstore.getWarmup()->setWarmupTime();
            epstore.warmupCompleted();
            logWarmupStats(epstore);
        }
        EP_LOG_INFO(
                "LoadStorageKVPairCallback::callback(): {} "
                "Engine warmup is complete, request to stop "
                "loading remaining database",
                i->getVBucketId());
        setStatus(cb::engine_errc::no_memory);
    } else {
        setStatus(cb::engine_errc::success);
    }
}

bool LoadStorageKVPairCallback::shouldEject() const {
    return stats.getEstimatedTotalMemoryUsed() >= stats.mem_low_wat;
}

void LoadStorageKVPairCallback::purge() {
    class EmergencyPurgeVisitor : public VBucketVisitor,
                                  public HashTableVisitor {
    public:
        explicit EmergencyPurgeVisitor(EPBucket& store) : epstore(store) {
        }

        void visitBucket(VBucket& vb) override {
            if (vBucketFilter(vb.getId())) {
                currentBucket = &vb;
                vb.ht.visit(*this);
                currentBucket = nullptr;
            }
        }

        bool visit(const HashTable::HashBucketLock& lh,
                   StoredValue& v) override {
            StoredValue* vPtr = &v;
            currentBucket->ht.unlocked_ejectItem(
                    lh, vPtr, epstore.getItemEvictionPolicy());
            return true;
        }

    private:
        EPBucket& epstore;
        // The current vbucket that the visitor is operating on. Only valid
        // while inside visitBucket().
        VBucket* currentBucket{nullptr};
    };

    auto vbucketIds(vbuckets.getBuckets());
    EmergencyPurgeVisitor epv(epstore);
    for (auto vbid : vbucketIds) {
        VBucketPtr vb = vbuckets.getBucket(vbid);
        if (vb) {
            epv.visitBucket(*vb);
        }
    }
    hasPurged = true;
}

void LoadValueCallback::callback(CacheLookup &lookup)
{
    // If not value-eviction (LoadingData), then skip attempting to check for
    // value already resident, given we assume nothing has been loaded for this
    // document yet.
    if (warmupState != WarmupState::State::LoadingData) {
        setStatus(cb::engine_errc::success);
        return;
    }

    // Prepared SyncWrites are ignored in the normal LoadValueCallback -
    // they are handled in an earlier warmup phase so return
    // cb::engine_errc::key_already_exists to indicate this key should be
    // skipped.
    if (lookup.getKey().isPrepared()) {
        setStatus(cb::engine_errc::key_already_exists);
        return;
    }

    VBucketPtr vb = vbuckets.getBucket(lookup.getVBucketId());
    if (!vb) {
        return;
    }

    // We explicitly want the committedSV (if exists).
    auto res = vb->ht.findOnlyCommitted(lookup.getKey().getDocKey());
    if (res.storedValue && res.storedValue->isResident()) {
        // Already resident in memory - skip loading from disk.
        setStatus(cb::engine_errc::key_already_exists);
        return;
    }

    // Otherwise - item value not in hashTable - continue with disk load.
    setStatus(cb::engine_errc::success);
}

//////////////////////////////////////////////////////////////////////////////
//                                                                          //
//    Implementation of the warmup class                                    //
//                                                                          //
//////////////////////////////////////////////////////////////////////////////

Warmup::Warmup(EPBucket& st, Configuration& config_)
    : store(st),
      config(config_),
      shardVbStates(store.vbMap.getNumShards()),
      shardVbIds(store.vbMap.getNumShards()),
      warmedUpVbuckets(config.getMaxVbuckets()) {
}

Warmup::~Warmup() = default;

void Warmup::addToTaskSet(size_t taskId) {
    std::lock_guard<std::mutex> lh(taskSetMutex);
    taskSet.insert(taskId);
}

void Warmup::removeFromTaskSet(size_t taskId) {
    std::lock_guard<std::mutex> lh(taskSetMutex);
    taskSet.erase(taskId);
}

void Warmup::setEstimatedWarmupCount(size_t to)
{
    estimatedWarmupCount.store(to);
}

size_t Warmup::getEstimatedItemCount() const {
    return estimatedItemCount.load();
}

void Warmup::start() {
    step();
}

void Warmup::stop() {
    {
        std::lock_guard<std::mutex> lh(taskSetMutex);
        if(taskSet.empty()) {
            return;
        }
        for (auto id : taskSet) {
            ExecutorPool::get()->cancel(id);
        }
        taskSet.clear();
    }
    transition(WarmupState::State::Done, true);
    done();

    // If we haven't already completed populateVBucketMap step, then
    // unblock (and cancel) any pending cookies so those connections don't
    // get stuck.
    // (On a normal, successful warmup these cookies would have already
    // been notified when populateVBucketMap finished).
    processCreateVBucketsComplete(cb::engine_errc::disconnect);
}

void Warmup::scheduleInitialize() {
    ExTask task = std::make_shared<WarmupInitialize>(store, this);
    ExecutorPool::get()->schedule(task);
}

void Warmup::initialize() {
    {
        std::lock_guard<std::mutex> lock(warmupStart.mutex);
        warmupStart.time = std::chrono::steady_clock::now();
    }

    auto session_stats = store.getOneROUnderlying()->getPersistedStats();
    auto it = session_stats.find("ep_force_shutdown");
    if (it != session_stats.end() && it.value() == "false") {
        cleanShutdown = true;
        // We want to ensure that if we crash from now and before the
        // StatSnap task runs. Then warmup again, that we will generate a new
        // failover entry and not treat the last shutdown as being clean. To do
        // this we just need to set 'ep_force_shutdown=true' in the stats.json
        // file.
        session_stats["ep_force_shutdown"] = "true";
        while (!store.getOneRWUnderlying()->snapshotStats(session_stats)) {
            EP_LOG_ERR_RAW(
                    "Warmup::initialize(): failed to persist snapshotStats "
                    "setting ep_force_shutdown=true, sleeping for 1 sec before "
                    "retrying");
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

    if (!store.getCollectionsManager().warmupLoadManifest(
                store.getEPEngine().getConfiguration().getDbname())) {
        EP_LOG_CRITICAL_RAW(
                "Warmup::initialize aborting as manifest cannot be loaded");
        return;
    }

    populateShardVbStates();

    for (uint16_t i = 0; i < store.vbMap.getNumShards(); i++) {
        accessLog.emplace_back(config.getAlogPath() + "." + std::to_string(i),
                               config.getAlogBlockSize());
    }

    transition(WarmupState::State::CreateVBuckets);
}

void Warmup::scheduleCreateVBuckets()
{
    threadtask_count = 0;
    for (size_t i = 0; i < store.vbMap.shards.size(); i++) {
        ExTask task = std::make_shared<WarmupCreateVBuckets>(store, i, this);
        ExecutorPool::get()->schedule(task);
    }
}

void Warmup::createVBuckets(uint16_t shardId) {
    size_t maxEntries = store.getEPEngine().getMaxFailoverEntries();

    // Iterate over all VBucket states defined for this shard, creating VBucket
    // objects if they do not already exist.
    for (const auto& itr : shardVbStates[shardId]) {
        Vbid vbid = itr.first;
        const vbucket_state& vbs = itr.second;

        // Collections and sync-repl requires that the VBucket datafiles have
        // 'namespacing' applied to the key space
        if (!vbs.supportsNamespaces) {
            EP_LOG_CRITICAL(
                    "Warmup::createVBuckets aborting warmup as {} datafile "
                    "is unusable, name-spacing is not enabled.",
                    vbid);
            return;
        }

        VBucketPtr vb = store.getVBucket(vbid);
        if (!vb) {
            std::unique_ptr<FailoverTable> table;
            if (vbs.transition.failovers.empty()) {
                table = std::make_unique<FailoverTable>(maxEntries);
            } else {
                table = std::make_unique<FailoverTable>(
                        vbs.transition.failovers, maxEntries, vbs.highSeqno);
            }
            KVShard* shard = store.getVBuckets().getShardByVbId(vbid);

            std::unique_ptr<Collections::VB::Manifest> manifest;
            if (config.isCollectionsEnabled()) {
                auto [getManifestStatus, persistedManifest] =
                        store.getROUnderlyingByShard(shardId)
                                ->getCollectionsManifest(vbid);
                if (!getManifestStatus) {
                    EP_LOG_CRITICAL(
                            "Warmup::createVBuckets: {} failed to read "
                            " collections manifest from disk",
                            vbid);
                    return;
                }

                manifest = std::make_unique<Collections::VB::Manifest>(
                        store.getSharedCollectionsManager(), persistedManifest);
            } else {
                manifest = std::make_unique<Collections::VB::Manifest>(
                        store.getSharedCollectionsManager());
            }

            const auto* topology =
                    vbs.transition.replicationTopology.empty()
                            ? nullptr
                            : &vbs.transition.replicationTopology;
            vb = store.makeVBucket(vbid,
                                   vbs.transition.state,
                                   shard,
                                   std::move(table),
                                   std::make_unique<NotifyNewSeqnoCB>(store),
                                   std::move(manifest),
                                   vbs.transition.state,
                                   vbs.highSeqno,
                                   vbs.lastSnapStart,
                                   vbs.lastSnapEnd,
                                   vbs.purgeSeqno,
                                   vbs.maxCas,
                                   vbs.hlcCasEpochSeqno,
                                   vbs.mightContainXattrs,
                                   topology,
                                   vbs.maxVisibleSeqno);

            if (vbs.transition.state == vbucket_state_active &&
                (!cleanShutdown ||
                 store.getCollectionsManager().needsUpdating(*vb))) {
                if (static_cast<uint64_t>(vbs.highSeqno) == vbs.lastSnapEnd) {
                    vb->failovers->createEntry(vbs.lastSnapEnd);
                } else {
                    vb->failovers->createEntry(vbs.lastSnapStart);
                }

                auto entry = vb->failovers->getLatestEntry();
                EP_LOG_INFO(
                        "Warmup::createVBuckets: {} created new failover entry "
                        "with uuid:{} and seqno:{} due to {}",
                        vbid,
                        entry.vb_uuid,
                        entry.by_seqno,
                        !cleanShutdown ? "unclean shutdown" : "manifest uid");
            }
            EPBucket* bucket = &this->store;
            vb->setFreqSaturatedCallback(
                    [bucket]() { bucket->wakeItemFreqDecayerTask(); });

            // Add the new vbucket to our local map, it will later be added
            // to the bucket's vbMap once the vbuckets are fully initialised
            // from KVStore data
            warmedUpVbuckets.insert(std::make_pair(vbid.get(), vb));
        }

        // Pass the max deleted seqno for each vbucket.
        vb->ht.setMaxDeletedRevSeqno(vbs.maxDeletedSeqno);

        // For each vbucket, set the last persisted seqno checkpoint
        vb->setPersistenceSeqno(vbs.highSeqno);
    }

    if (++threadtask_count == store.vbMap.getNumShards()) {
        transition(WarmupState::State::LoadingCollectionCounts);
    }
}

void Warmup::processCreateVBucketsComplete(cb::engine_errc status) {
    PendingCookiesQueue toNotify;
    {
        std::unique_lock<std::mutex> lock(pendingCookiesMutex);
        createVBucketsComplete = true;
        pendingCookies.swap(toNotify);
    }
    if (toNotify.empty()) {
        return;
    }

    EP_LOG_INFO("Warmup::processCreateVBucketsComplete unblocking {} cookie(s)",
                toNotify.size());
    for (const auto* c : toNotify) {
        store.getEPEngine().notifyIOComplete(c, status);
    }
}

bool Warmup::maybeWaitForVBucketWarmup(const CookieIface* cookie) {
    std::lock_guard<std::mutex> lg(pendingCookiesMutex);
    if (!createVBucketsComplete) {
        pendingCookies.push_back(cookie);
        return true;
    }
    return false;
}

void Warmup::scheduleLoadingCollectionCounts() {
    threadtask_count = 0;
    for (size_t i = 0; i < store.vbMap.shards.size(); i++) {
        ExTask task = std::make_shared<WarmupLoadingCollectionCounts>(
                store, i, *this);
        ExecutorPool::get()->schedule(task);
    }
}

void Warmup::loadCollectionStatsForShard(uint16_t shardId) {
    // get each VB in the shard and iterate its collections manifest
    // load the _local doc count value

    const auto* kvstore = store.getROUnderlyingByShard(shardId);
    // Iterate the VBs in the shard
    for (const auto vbid : shardVbIds[shardId]) {
        auto itr = warmedUpVbuckets.find(vbid.get());
        if (itr == warmedUpVbuckets.end()) {
            continue;
        }

        // Take the KVFileHandle before we lock the manifest to prevent lock
        // order inversions.
        auto kvstoreContext = kvstore->makeFileHandle(vbid);
        if (!kvstoreContext) {
            EP_LOG_CRITICAL(
                    "Warmup::loadCollectionStatsForShard() Unable to make "
                    "KVFileHandle for {}, aborting warmup as we will not be "
                    "able to check collection stats.",
                    vbid);
            return;
        }

        auto wh = itr->second->getManifest().wlock();
        // For each collection in the VB, get its stats
        for (auto& collection : wh) {
            // start tracking in-memory stats before items are warmed up.
            // This may be called repeatedly; it is idempotent.
            store.stats.trackCollectionStats(collection.first);

            // getCollectionStats() can still can fail if the data store on disk
            // has been corrupted between the call to makeFileHandle() and
            // getCollectionStats()
            auto [status, stats] = kvstore->getCollectionStats(
                    *kvstoreContext, collection.first);
            if (status == KVStore::GetCollectionStatsStatus::Failed) {
                EP_LOG_CRITICAL(
                        "Warmup::loadCollectionStatsForShard(): "
                        "getCollectionStats() failed for {}, aborting warmup "
                        "as we will not be "
                        "able to check collection stats.",
                        vbid);
                return;
            }
            // For NotFound we're ok to use the default initialised stats

            collection.second.setItemCount(stats.itemCount);
            collection.second.setPersistedHighSeqno(stats.highSeqno);
            collection.second.setDiskSize(stats.diskSize);
            // Set the in memory high seqno - might be 0 in the case of the
            // default collection so we have to reset the monotonic value
            collection.second.resetHighSeqno(stats.highSeqno);

            // And update the scope data size
            wh.updateDataSize(collection.second.getScopeID(), stats.diskSize);
        }
    }

    if (++threadtask_count == store.vbMap.getNumShards()) {
        transition(WarmupState::State::EstimateDatabaseItemCount);
    }
}

void Warmup::scheduleEstimateDatabaseItemCount()
{
    threadtask_count = 0;
    estimateTime.store(std::chrono::steady_clock::duration::zero());
    estimatedItemCount = 0;
    for (size_t i = 0; i < store.vbMap.shards.size(); i++) {
        ExTask task = std::make_shared<WarmupEstimateDatabaseItemCount>(
                store, i, this);
        ExecutorPool::get()->schedule(task);
    }
}

void Warmup::estimateDatabaseItemCount(uint16_t shardId) {
    auto st = std::chrono::steady_clock::now();
    size_t item_count = 0;

    for (const auto vbid : shardVbIds[shardId]) {
        size_t vbItemCount = 0;
        auto itr = warmedUpVbuckets.find(vbid.get());
        if (itr != warmedUpVbuckets.end()) {
            auto& epVb = static_cast<EPVBucket&>(*itr->second);
            epVb.setNumTotalItems(*store.getRWUnderlyingByShard(shardId));
            vbItemCount = epVb.getNumTotalItems();
        }
        item_count += vbItemCount;
    }

    estimatedItemCount.fetch_add(item_count);
    estimateTime.fetch_add(std::chrono::steady_clock::now() - st);

    if (++threadtask_count == store.vbMap.getNumShards()) {
        transition(WarmupState::State::LoadPreparedSyncWrites);
    }
}

void Warmup::scheduleLoadPreparedSyncWrites() {
    threadtask_count = 0;
    for (size_t i = 0; i < store.vbMap.shards.size(); i++) {
        ExTask task = std::make_shared<WarmupLoadPreparedSyncWrites>(
                &store.getEPEngine(), i, *this);
        ExecutorPool::get()->schedule(task);
    }
}

void Warmup::loadPreparedSyncWrites(uint16_t shardId) {
    for (const auto vbid : shardVbIds[shardId]) {
        auto itr = warmedUpVbuckets.find(vbid.get());
        if (itr == warmedUpVbuckets.end()) {
            continue;
        }

        // Our EPBucket function will do the load for us as we re-use the code
        // for rollback.
        auto& vb = *(itr->second);

        auto [itemsVisited, preparesLoaded, success] =
                store.loadPreparedSyncWrites(vb);
        if (!success) {
            EP_LOG_CRITICAL(
                    "Warmup::loadPreparedSyncWrites(): "
                    "EPBucket::loadPreparedSyncWrites() failed for {} aborting "
                    "Warmup",
                    vbid);
            return;
        }
        auto& epStats = store.getEPEngine().getEpStats();
        epStats.warmupItemsVisitedWhilstLoadingPrepares += itemsVisited;
        epStats.warmedUpPrepares += preparesLoaded;
    }

    if (++threadtask_count == store.vbMap.getNumShards()) {
        transition(WarmupState::State::PopulateVBucketMap);
    }
}

void Warmup::schedulePopulateVBucketMap() {
    threadtask_count = 0;
    for (size_t i = 0; i < store.vbMap.shards.size(); i++) {
        ExTask task =
                std::make_shared<WarmupPopulateVBucketMap>(store, i, *this);
        ExecutorPool::get()->schedule(task);
    }
}

void Warmup::populateVBucketMap(uint16_t shardId) {
    for (const auto vbid : shardVbIds[shardId]) {
        auto itr = warmedUpVbuckets.find(vbid.get());
        if (itr != warmedUpVbuckets.end()) {
            const auto& vbPtr = itr->second;
            // Take the vBucket lock to stop the flusher from racing with our
            // set vBucket state. It MUST go to disk in the first flush batch
            // or we run the risk of not rolling back replicas that we should
            auto lockedVb = store.getLockedVBucket(vbid);
            Expects(lockedVb.owns_lock());
            Expects(!lockedVb);

            vbPtr->checkpointManager->queueSetVBState();
            if (itr->second->getState() == vbucket_state_active) {
                // For all active vbuckets, call through to the manager so
                // that they are made 'current' with the manifest.
                store.getCollectionsManager().maybeUpdate(*itr->second);
            }
            auto result = store.flushVBucket_UNLOCKED(
                    {vbPtr, std::move(lockedVb.getLock())});
            // if flusher returned MoreAvailable::Yes, this indicates the single
            // flush of the vbucket state failed.
            if (result.moreAvailable == EPBucket::MoreAvailable::Yes) {
                // Disabling writes to this node as we're unable to persist
                // vbucket state to disk.
                EP_LOG_CRITICAL(
                        "Warmup::populateVBucketMap() flush state failed for "
                        "{} highSeqno:{}, write traffic will be disabled for "
                        "this node.",
                        vbid,
                        vbPtr->getHighSeqno());
                failedToSetAVbucketState = true;
            }

            store.vbMap.addBucket(vbPtr);
        }
    }

    if (++threadtask_count == store.vbMap.getNumShards()) {
        // All threads have finished populating the vBucket map (and potentially
        // flushing a new vBucket state), it's now safe for us to start the
        // flushers.
        store.startFlusher();

        warmedUpVbuckets.clear();
        // Once we have populated the VBMap we can allow setVB state changes
        processCreateVBucketsComplete(cb::engine_errc::success);
        if (store.getItemEvictionPolicy() == EvictionPolicy::Value) {
            transition(WarmupState::State::KeyDump);
        } else {
            transition(WarmupState::State::CheckForAccessLog);
        }
    }
}

void Warmup::scheduleBackfillTask(MakeBackfillTaskFn makeBackfillTask) {
    threadtask_count = 0;
    for (size_t shardId = 0; shardId < store.vbMap.shards.size(); ++shardId) {
        ExecutorPool::get()->schedule(makeBackfillTask(shardId));
    }
}

void Warmup::scheduleKeyDump() {
    auto createTask = [this](size_t shardId) -> ExTask {
        return std::make_shared<WarmupKeyDump>(
                store, shardId, *this, threadtask_count);
    };
    scheduleBackfillTask(createTask);
}

void Warmup::scheduleCheckForAccessLog()
{
    ExTask task = std::make_shared<WarmupCheckforAccessLog>(store, this);
    ExecutorPool::get()->schedule(task);
}

void Warmup::checkForAccessLog()
{
    {
        std::lock_guard<std::mutex> lock(warmupStart.mutex);
        metadata.store(std::chrono::steady_clock::now() - warmupStart.time);
    }
    EP_LOG_INFO("metadata loaded in {}",
                cb::time2text(std::chrono::nanoseconds(metadata.load())));

    if (store.maybeEnableTraffic()) {
        transition(WarmupState::State::Done);
    }

    size_t accesslogs = 0;
    for (size_t i = 0; i < store.vbMap.shards.size(); i++) {
        std::string curr = accessLog[i].getLogFile();
        std::string old = accessLog[i].getLogFile();
        old.append(".old");
        if (cb::io::isFile(curr) || cb::io::isFile(old)) {
            accesslogs++;
        }
    }
    if (accesslogs == store.vbMap.shards.size()) {
        transition(WarmupState::State::LoadingAccessLog);
    } else {
        if (store.getItemEvictionPolicy() == EvictionPolicy::Value) {
            transition(WarmupState::State::LoadingData);
        } else {
            transition(WarmupState::State::LoadingKVPairs);
        }
    }

}

void Warmup::scheduleLoadingAccessLog()
{
    threadtask_count = 0;
    for (size_t i = 0; i < store.vbMap.shards.size(); i++) {
        ExTask task = std::make_shared<WarmupLoadAccessLog>(store, i, this);
        ExecutorPool::get()->schedule(task);
    }
}

void Warmup::loadingAccessLog(uint16_t shardId)
{
    LoadStorageKVPairCallback load_cb(store, true, state.getState());
    bool success = false;
    auto stTime = std::chrono::steady_clock::now();
    if (accessLog[shardId].exists()) {
        try {
            accessLog[shardId].open();
            if (doWarmup(accessLog[shardId], shardVbStates[shardId], load_cb) !=
                (size_t)-1) {
                success = true;
            }
        } catch (MutationLog::ReadException &e) {
            corruptAccessLog = true;
            EP_LOG_WARN("Error reading warmup access log:  {}", e.what());
        }
    }

    if (!success) {
        // Do we have the previous file?
        std::string nm = accessLog[shardId].getLogFile();
        nm.append(".old");
        MutationLog old(nm);
        if (old.exists()) {
            try {
                old.open();
                if (doWarmup(old, shardVbStates[shardId], load_cb) !=
                    (size_t)-1) {
                    success = true;
                }
            } catch (MutationLog::ReadException &e) {
                corruptAccessLog = true;
                EP_LOG_WARN("Error reading old access log:  {}", e.what());
            }
        }
    }

    size_t numItems = store.getEPEngine().getEpStats().warmedUpValues;
    if (success && numItems) {
        EP_LOG_INFO("{} items loaded from access log, completed in {}",
                    uint64_t(numItems),
                    cb::time2text(std::chrono::steady_clock::now() - stTime));
    } else {
        size_t estimatedCount= store.getEPEngine().getEpStats().warmedUpKeys;
        setEstimatedWarmupCount(estimatedCount);
    }

    if (++threadtask_count == store.vbMap.getNumShards()) {
        if (!store.maybeEnableTraffic()) {
            transition(WarmupState::State::LoadingData);
        } else {
            transition(WarmupState::State::Done);
        }

    }
}

size_t Warmup::doWarmup(MutationLog& lf,
                        const std::map<Vbid, vbucket_state>& vbmap,
                        StatusCallback<GetValue>& cb) {
    MutationLogHarvester harvester(lf, &store.getEPEngine());
    std::map<Vbid, vbucket_state>::const_iterator it;
    for (it = vbmap.begin(); it != vbmap.end(); ++it) {
        harvester.setVBucket(it->first);
    }

    // To constrain the number of elements from the access log we have to keep
    // alive (there may be millions of items per-vBucket), process it
    // a batch at a time.
    std::chrono::nanoseconds log_load_duration{};
    std::chrono::nanoseconds log_apply_duration{};
    WarmupCookie cookie(&store, cb);

    auto alog_iter = lf.begin();
    do {
        // Load a chunk of the access log file
        auto start = std::chrono::steady_clock::now();
        alog_iter = harvester.loadBatch(alog_iter, config.getWarmupBatchSize());
        log_load_duration += (std::chrono::steady_clock::now() - start);

        // .. then apply it to the store.
        auto apply_start = std::chrono::steady_clock::now();
        harvester.apply(&cookie, &batchWarmupCallback);
        log_apply_duration += (std::chrono::steady_clock::now() - apply_start);
    } while (alog_iter != lf.end());

    size_t total = harvester.total();
    setEstimatedWarmupCount(total);
    EP_LOG_DEBUG("Completed log read in {} with {} entries",
                 cb::time2text(log_load_duration),
                 total);

    EP_LOG_DEBUG("Populated log in {} with(l: {}, s: {}, e: {})",
                 cb::time2text(log_apply_duration),
                 cookie.loaded,
                 cookie.skipped,
                 cookie.error);

    return cookie.loaded;
}

void Warmup::scheduleLoadingKVPairs() {
    // We reach here only if keyDump didn't return SUCCESS or if
    // in case of Full Eviction. Either way, set estimated value
    // count equal to the estimated item count, as very likely no
    // keys have been warmed up at this point.
    setEstimatedWarmupCount(estimatedItemCount);

    auto createTask = [this](size_t shardId) -> ExTask {
        return std::make_shared<WarmupLoadingKVPairs>(
                store, shardId, *this, threadtask_count);
    };
    scheduleBackfillTask(createTask);
}

void Warmup::scheduleLoadingData() {
    auto estimatedCount = store.getEPEngine().getEpStats().warmedUpKeys;
    setEstimatedWarmupCount(estimatedCount);

    auto createTask = [this](size_t shardId) -> ExTask {
        return std::make_shared<WarmupLoadingData>(
                store, shardId, *this, threadtask_count);
    };
    scheduleBackfillTask(createTask);
}

void Warmup::scheduleCompletion() {
    ExTask task = std::make_shared<WarmupCompletion>(store, this);
    ExecutorPool::get()->schedule(task);
}

void Warmup::done()
{
    if (setFinishedLoading()) {
        setWarmupTime();
        store.warmupCompleted();
        logWarmupStats(store);
    }
}

void Warmup::step() {
    switch (state.getState()) {
    case WarmupState::State::Initialize:
        scheduleInitialize();
        return;
    case WarmupState::State::CreateVBuckets:
        scheduleCreateVBuckets();
        return;
    case WarmupState::State::LoadingCollectionCounts:
        scheduleLoadingCollectionCounts();
        return;
    case WarmupState::State::EstimateDatabaseItemCount:
        scheduleEstimateDatabaseItemCount();
        return;
    case WarmupState::State::LoadPreparedSyncWrites:
        scheduleLoadPreparedSyncWrites();
        return;
    case WarmupState::State::PopulateVBucketMap:
        schedulePopulateVBucketMap();
        return;
    case WarmupState::State::KeyDump:
        scheduleKeyDump();
        return;
    case WarmupState::State::CheckForAccessLog:
        scheduleCheckForAccessLog();
        return;
    case WarmupState::State::LoadingAccessLog:
        scheduleLoadingAccessLog();
        return;
    case WarmupState::State::LoadingKVPairs:
        scheduleLoadingKVPairs();
        return;
    case WarmupState::State::LoadingData:
        scheduleLoadingData();
        return;
    case WarmupState::State::Done:
        scheduleCompletion();
        return;
    }
    throw std::logic_error("Warmup::step: illegal warmup state:" +
                           std::to_string(int(state.getState())));
}

void Warmup::transition(WarmupState::State to, bool force) {
    state.transition(to, force);
    stateTransitionHook(to);
    step();
}

void Warmup::addStats(const AddStatFn& add_stat, const CookieIface* c) const {
    using namespace std::chrono;

    auto addPrefixedStat = [&add_stat, &c](const char* nm, const auto& val) {
        std::string name = "ep_warmup";
        if (nm != nullptr) {
            name.append("_");
            name.append(nm);
        }

        std::stringstream value;
        value << val;
        add_casted_stat(name.data(), value.str().data(), add_stat, c);
    };

    EPStats& stats = store.getEPEngine().getEpStats();
    addPrefixedStat(nullptr, "enabled");
    const char* stateName = state.toString();
    addPrefixedStat("state", stateName);
    addPrefixedStat("thread", getThreadStatState());
    addPrefixedStat("key_count", stats.warmedUpKeys);
    addPrefixedStat("value_count", stats.warmedUpValues);
    addPrefixedStat("dups", stats.warmDups);
    addPrefixedStat("oom", stats.warmOOM);
    addPrefixedStat("min_memory_threshold", stats.warmupMemUsedCap * 100.0);
    addPrefixedStat("min_item_threshold", stats.warmupNumReadCap * 100.0);

    auto md_time = metadata.load();
    if (md_time > md_time.zero()) {
        addPrefixedStat("keys_time",
                        duration_cast<microseconds>(md_time).count());
    }

    auto w_time = warmup.load();
    if (w_time > w_time.zero()) {
        addPrefixedStat("time", duration_cast<microseconds>(w_time).count());
    }

    size_t itemCount = estimatedItemCount.load();
    if (itemCount == std::numeric_limits<size_t>::max()) {
        addPrefixedStat("estimated_key_count", "unknown");
    } else {
        auto e_time = estimateTime.load();
        if (e_time != e_time.zero()) {
            addPrefixedStat("estimate_time",
                            duration_cast<microseconds>(e_time).count());
        }
        addPrefixedStat("estimated_key_count", itemCount);
    }

    if (corruptAccessLog) {
        addPrefixedStat("access_log", "corrupt");
    }

    size_t warmupCount = estimatedWarmupCount.load();
    if (warmupCount == std::numeric_limits<size_t>::max()) {
        addPrefixedStat("estimated_value_count", "unknown");
    } else {
        addPrefixedStat("estimated_value_count", warmupCount);
    }
}

uint16_t Warmup::getNumKVStores() {
    return store.vbMap.getNumShards();
}

void Warmup::populateShardVbStates() {
    uint16_t numKvs = getNumKVStores();

    for (size_t i = 0; i < numKvs; i++) {
        std::vector<vbucket_state*> kvStoreVbStates =
                store.getRWUnderlyingByShard(i)->listPersistedVbuckets();
        for (uint16_t j = 0; j < kvStoreVbStates.size(); j++) {
            if (!kvStoreVbStates[j]) {
                continue;
            }
            auto vb = (j * numKvs) + i;
            std::map<Vbid, vbucket_state>& shardVB =
                    shardVbStates[vb % store.vbMap.getNumShards()];
            shardVB.insert(std::pair<Vbid, vbucket_state>(
                    Vbid(vb), *(kvStoreVbStates[j])));
        }
    }

    for (size_t i = 0; i < store.vbMap.shards.size(); i++) {
        std::vector<Vbid> activeVBs, otherVBs;
        std::map<Vbid, vbucket_state>::const_iterator it;
        for (auto shardIt : shardVbStates[i]) {
            Vbid vbid = shardIt.first;
            vbucket_state vbs = shardIt.second;
            if (vbs.transition.state == vbucket_state_active) {
                activeVBs.push_back(vbid);
            } else {
                otherVBs.push_back(vbid);
            }
        }

        // Push one active VB to the front.
        // When the ratio of RAM to VBucket is poor (big vbuckets) this will
        // ensure we at least bring active data in before replicas eat RAM.
        if (!activeVBs.empty()) {
            shardVbIds[i].push_back(activeVBs.back());
            activeVBs.pop_back();
        }

        // Now the VB lottery can begin.
        // Generate a psudeo random, weighted list of active/replica vbuckets.
        // The random seed is the shard ID so that re-running warmup
        // for the same shard and vbucket set always gives the same output and keeps
        // nodes of the cluster more equal after a warmup.

        std::mt19937 twister(i);
        // Give 'true' (aka active) 60% of the time
        // Give 'false' (aka other) 40% of the time.
        std::bernoulli_distribution distribute(0.6);
        std::array<std::vector<Vbid>*, 2> activeReplicaSource = {
                {&activeVBs, &otherVBs}};

        while (!activeVBs.empty() || !otherVBs.empty()) {
            const bool active = distribute(twister);
            int num = active ? 0 : 1;
            if (!activeReplicaSource[num]->empty()) {
                shardVbIds[i].push_back(activeReplicaSource[num]->back());
                activeReplicaSource[num]->pop_back();
            } else {
                // Once active or replica set is empty, just drain the other one.
                num = num ^ 1;
                while (!activeReplicaSource[num]->empty()) {
                    shardVbIds[i].push_back(activeReplicaSource[num]->back());
                    activeReplicaSource[num]->pop_back();
                }
            }
        }
    }
}
