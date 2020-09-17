/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2017 Couchbase, Inc
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */

#include "ephemeral_bucket_test.h"

#include "checkpoint.h"
#include "ephemeral_bucket.h"
#include "ephemeral_tombstone_purger.h"
#include "ephemeral_vb.h"
#include "test_helpers.h"

#include "../mock/mock_dcp_consumer.h"
#include "dcp/dcpconnmap.h"
/*
 * Test statistics related to an individual VBucket's sequence list.
 */

void EphemeralBucketStatTest::addDocumentsForSeqListTesting(uint16_t vb) {
    // Add some documents to the vBucket to use to test the stats.
    store_item(vb, makeStoredDocKey("deleted"), "value");
    delete_item(vb, makeStoredDocKey("deleted"));
    store_item(vb, makeStoredDocKey("doc"), "value");
    store_item(vb, makeStoredDocKey("doc"), "value 2");
}

TEST_F(EphemeralBucketStatTest, VBSeqlistStats) {
    // Check preconditions.
    auto stats = get_stat("vbucket-details 0");
    ASSERT_EQ("0", stats.at("vb_0:seqlist_high_seqno"));

    // Add some documents to the vBucket to use to test the stats.
    addDocumentsForSeqListTesting(vbid);

    stats = get_stat("vbucket-details 0");

    EXPECT_EQ("0", stats.at("vb_0:auto_delete_count"));
    EXPECT_EQ("2", stats.at("vb_0:seqlist_count"))
        << "Expected both current and deleted documents";
    EXPECT_EQ("1", stats.at("vb_0:seqlist_deleted_count"));
    EXPECT_EQ("4", stats.at("vb_0:seqlist_high_seqno"));
    EXPECT_EQ("4", stats.at("vb_0:seqlist_highest_deduped_seqno"));
    EXPECT_EQ("0", stats.at("vb_0:seqlist_range_read_begin"));
    EXPECT_EQ("0", stats.at("vb_0:seqlist_range_read_end"));
    EXPECT_EQ("0", stats.at("vb_0:seqlist_range_read_count"));
    EXPECT_EQ("0", stats.at("vb_0:seqlist_stale_count"));
    EXPECT_EQ("0", stats.at("vb_0:seqlist_stale_value_bytes"));
    EXPECT_EQ("0", stats.at("vb_0:seqlist_stale_metadata_bytes"));

    // Trigger the "automatic" deletion of an item by paging it out.
    auto vb = store->getVBucket(vbid);
    auto key = makeStoredDocKey("doc");
    auto lock = vb->ht.getLockedBucket(key);
    auto* value = vb->fetchValidValue(
            lock, key, WantsDeleted::No, TrackReference::Yes, QueueExpired::No);
    ASSERT_TRUE(vb->pageOut(lock, value));

    stats = get_stat("vbucket-details 0");
    EXPECT_EQ("1", stats.at("vb_0:auto_delete_count"));
    EXPECT_EQ("2", stats.at("vb_0:seqlist_deleted_count"));
    EXPECT_EQ("5", stats.at("vb_0:seqlist_high_seqno"));
}

TEST_F(EphemeralBucketStatTest, ReplicaMemoryTracking) {
    // test that replicaHTMemory is correctly updated for
    // inserts/updates/deletes/tombstone removal.
    auto replicaVB = 0;
    setVBucketStateAndRunPersistTask(replicaVB, vbucket_state_replica);

    auto cookie = create_mock_cookie();

    auto& stats = engine->getEpStats();
    EXPECT_EQ(0, stats.replicaHTMemory);

    auto key = makeStoredDocKey("item2");

    std::string value = "value";
    auto item = make_item(replicaVB, key, value);

    // Store an item in a replica VB and confirm replicaHTMemory increases
    item.setCas(1);
    uint64_t seqno;
    ASSERT_EQ(ENGINE_SUCCESS,
              store->setWithMeta(std::ref(item),
                                 0,
                                 &seqno,
                                 cookie,
                                 {vbucket_state_replica},
                                 CheckConflicts::No,
                                 /*allowExisting*/ true));

    // avoids checking exact values to be resilient to changes (e.g.) in stored
    // value size.
    auto smallItemMem = stats.replicaHTMemory;
    EXPECT_GT(smallItemMem, 80);

    // Replace the existing item with a _larger_ item and confirm
    // replicaHTMemory increases further
    std::string largerValue = "valuevaluevaluevaluevaluevalue";
    auto largerItem = make_item(replicaVB, key, largerValue);
    largerItem.setCas(1);
    ASSERT_EQ(ENGINE_SUCCESS,
              store->setWithMeta(std::ref(largerItem),
                                 0,
                                 &seqno,
                                 cookie,
                                 {vbucket_state_replica},
                                 CheckConflicts::No,
                                 /*allowExisting*/ true));

    auto largerItemMem = smallItemMem + largerValue.size() - value.size();
    EXPECT_EQ(largerItemMem, stats.replicaHTMemory);

    // Delete the item, confirm replicaHTMemory decreases (tombstone
    // remains).
    ItemMetaData meta;
    uint64_t cas = 1;
    meta.cas = cas;
    ASSERT_EQ(ENGINE_SUCCESS,
              store->deleteWithMeta(
                      key,
                      cas,
                      nullptr,
                      replicaVB,
                      cookie,
                      {vbucket_state_replica},
                      CheckConflicts::No,
                      meta,
                      false /* is backfill phase */,
                      GenerateBySeqno::Yes,
                      GenerateCas::No,
                      store->getVBucket(replicaVB)->getHighSeqno() + 1,
                      nullptr /* extended metadata */,
                      true));

    EXPECT_LT(stats.replicaHTMemory, largerItemMem);
    EXPECT_GT(stats.replicaHTMemory, 0);

    // now remove the tombstone and confirm the replicaHTMemory is now 0
    auto& replica = *store->getVBucket(replicaVB);

    EphemeralVBucket::HTTombstonePurger purger(
            0 /* remove tombstones of any age */);
    purger.setCurrentVBucket(replica);
    replica.ht.visit(purger);

    EXPECT_EQ(0, stats.replicaHTMemory);

    destroy_mock_cookie(cookie);
}

TEST_F(EphemeralBucketStatTest, ReplicaMemoryTrackingNotUpdatedForActive) {
    // replicaHTMemory should not be updated by storing items in active
    // vbuckets
    auto activeVB = 0;
    setVBucketStateAndRunPersistTask(activeVB, vbucket_state_active);

    auto& stats = engine->getEpStats();
    EXPECT_EQ(0, stats.replicaHTMemory);
    EXPECT_EQ(0, stats.replicaCheckpointOverhead);

    // Confirm replicaHTMemory is _not_ affected by storing an item to an
    // active vb.
    store_item(activeVB, makeStoredDocKey("item"), "value");
    EXPECT_EQ(0, stats.replicaHTMemory);
    EXPECT_EQ(0, stats.replicaCheckpointOverhead);
}

TEST_F(EphemeralBucketStatTest, ReplicaMemoryTrackingStateChange) {
    // Check that replicaHTMemory is increased/decreased as vbuckets change
    // state to/from replica
    setVBucketStateAndRunPersistTask(vbid, vbucket_state_active);

    auto key = makeStoredDocKey("item");

    auto& stats = engine->getEpStats();
    EXPECT_EQ(0, stats.replicaHTMemory);
    EXPECT_EQ(0, stats.replicaCheckpointOverhead);

    store_item(vbid, key, "value");

    EXPECT_EQ(0, stats.replicaHTMemory);
    EXPECT_EQ(0, stats.replicaCheckpointOverhead);

    setVBucketStateAndRunPersistTask(vbid, vbucket_state_replica);

    // check that the mem usage has gone up by some amount - not
    // checking it is an exact value to avoid a brittle test
    EXPECT_GT(stats.replicaHTMemory, 80);
    EXPECT_GT(stats.replicaCheckpointOverhead, 80);

    // changing back to active should return replicaHTMemory to 0
    setVBucketStateAndRunPersistTask(vbid, vbucket_state_active);

    EXPECT_EQ(0, stats.replicaHTMemory);
    EXPECT_EQ(0, stats.replicaCheckpointOverhead);
}

TEST_F(EphemeralBucketStatTest, ReplicaCheckpointMemoryTracking) {
    // test that replicaCheckpointOverhead is correctly updated
    auto replicaVB = 0;
    setVBucketStateAndRunPersistTask(replicaVB, vbucket_state_replica);

    auto cookie = create_mock_cookie();

    auto& replica = *store->getVBucket(replicaVB);
    auto& cpm = *replica.checkpointManager;

    // remove the checkpoint containing the set vbstate to get a clean
    // baseline memory usage
    cpm.createNewCheckpoint(true /*force*/);
    bool newCkptCreated = false;
    cpm.removeClosedUnrefCheckpoints(replica, newCkptCreated);

    auto& stats = engine->getEpStats();
    const auto initialMem = stats.replicaCheckpointOverhead;

    const auto keyA = makeStoredDocKey("itemA");
    const auto keyB = makeStoredDocKey("itemB");

    const std::string value = "value";
    auto item1 = make_item(replicaVB, keyA, value);

    // Store an item in a replica VB and confirm replicaCheckpointOverhead
    // increases
    item1.setCas(1);
    uint64_t seqno;
    ASSERT_EQ(ENGINE_SUCCESS,
              store->setWithMeta(std::ref(item1),
                                 0,
                                 &seqno,
                                 cookie,
                                 {vbucket_state_replica},
                                 CheckConflicts::No,
                                 /*allowExisting*/ true));

    // avoids checking exact values to be resilient to changes (e.g.) in stored
    // value size.
    const auto item1Mem = stats.replicaCheckpointOverhead;
    EXPECT_GT(item1Mem, initialMem + 20);

    // Store the item again and confirm replicaCheckpointOverhead
    // _does not increase_. This matches existing checkpoint memory tracking;
    // in the event of an existing item, checkpoint memory usage is _not_
    // adjusted, even though the old and new item could be of different sizes
    const std::string largerValue = "valuevaluevaluevaluevaluevaluevaluevalue";
    auto item2 = make_item(replicaVB, keyA, value);
    item2.setCas(1);
    ASSERT_EQ(ENGINE_SUCCESS,
              store->setWithMeta(std::ref(item2),
                                 0,
                                 &seqno,
                                 cookie,
                                 {vbucket_state_replica},
                                 CheckConflicts::No,
                                 /*allowExisting*/ true));
    // tracked memory unchanged
    EXPECT_EQ(item1Mem, stats.replicaCheckpointOverhead);

    // Store an item with a different key, confirm checkpoint mem increases
    auto item3 = make_item(replicaVB, keyB, value);
    item3.setCas(1);
    ASSERT_EQ(ENGINE_SUCCESS,
              store->setWithMeta(std::ref(item3),
                                 0,
                                 &seqno,
                                 cookie,
                                 {vbucket_state_replica},
                                 CheckConflicts::No,
                                 /*allowExisting*/ true));

    const auto item3Mem = stats.replicaCheckpointOverhead;
    EXPECT_GT(item3Mem, item1Mem);

    // now remove the checkpoint and confirm the replicaCheckpointOverhead is
    // now back to the initial value.
    cpm.createNewCheckpoint();
    cpm.removeClosedUnrefCheckpoints(replica, newCkptCreated);

    EXPECT_EQ(initialMem, stats.replicaCheckpointOverhead);

    destroy_mock_cookie(cookie);
}

TEST_F(EphemeralBucketStatTest,
       ReplicaCheckpointMemoryTracking_CheckpointCollapse) {
    // Confirm that checkpoint collapsing does not lead to misaccounting of
    // replica checkpoint memory
    auto replicaVB = 0;
    setVBucketStateAndRunPersistTask(replicaVB, vbucket_state_replica);

    auto cookie = create_mock_cookie();

    auto& replica = *store->getVBucket(replicaVB);
    auto& cpm = *replica.checkpointManager;

    // remove the checkpoint containing the set vbstate to get a clean
    // baseline memory usage
    cpm.createNewCheckpoint(true /*force*/);
    bool newCkptCreated = false;
    cpm.removeClosedUnrefCheckpoints(replica, newCkptCreated);

    auto& stats = engine->getEpStats();
    auto initialMem = stats.replicaCheckpointOverhead;
    auto currentMem = initialMem;

    // Now, enable checkpoint merging, and create a cursor to prevent
    // checkpoints being dropped
    engine->getConfiguration().setEnableChkMerge(true);
    engine->getConfiguration().setKeepClosedChks(false);
    engine->updateCheckpointConfig();

    std::string cursorName = "test_cursor";
    cpm.registerCursorBySeqno(cursorName, 0, MustSendCheckpointEnd::NO);

    std::string value = "value";
    for (int i = 0; i < 10; i++) {
        auto key = makeStoredDocKey("item" + std::to_string(i));
        auto item = make_item(replicaVB, key, value);

        // Store an item in a replica VB and confirm replicaCheckpointOverhead
        // increases
        item.setCas(1);
        uint64_t seqno;
        ASSERT_EQ(ENGINE_SUCCESS,
                  store->setWithMeta(std::ref(item),
                                     0,
                                     &seqno,
                                     cookie,
                                     {vbucket_state_replica},
                                     CheckConflicts::No,
                                     /*allowExisting*/ true));

        cpm.createNewCheckpoint(true /*force*/);

        auto preCollapseMem = stats.replicaCheckpointOverhead;
        // _attempt_ to remove closed checkpoints, to trigger checkpoint
        // collapsing
        cpm.removeClosedUnrefCheckpoints(replica, newCkptCreated);

        auto newMem = stats.replicaCheckpointOverhead;

        // after the first iteration, confirm that mem usage has dropped
        // (nothing will be collapsed on the first iteration, there are too
        // few checkpoints)
        if (i > 0) {
            EXPECT_LT(newMem, preCollapseMem);
        }

        // but is still higher than last iteration
        EXPECT_GT(newMem, currentMem);

        // and finally confirm that even though a new close checkpoint was
        // created, collapsing ran and merged the closed checkpoints together.
        EXPECT_LE(cpm.getNumCheckpoints(), 2);
    }

    // now remove the checkpoints and confirm the replicaHTMemory is now back
    // to the initial value.
    cpm.removeCursor(cursorName);
    cpm.removeClosedUnrefCheckpoints(replica, newCkptCreated);

    EXPECT_EQ(initialMem, stats.replicaCheckpointOverhead);

    destroy_mock_cookie(cookie);
}

TEST_F(SingleThreadedEphemeralBackfillTest, RangeIteratorVBDeleteRaceTest) {
    /* The destructor of RangeIterator attempts to release locks in the
     * seqList, which is owned by the Ephemeral VB. If the evb is
     * destructed before the iterator, unexepected behaviour will arise.
     * In MB-24631 the destructor spun trying to acquire a lock which
     * was now garbage data after the memory was reused.
     *
     * Due to the variable results of this, the test alone does not
     * confirm the absence of this issue, but AddressSanitizer should
     * report heap-use-after-free.
     */

    // Make vbucket active.
    setVBucketStateAndRunPersistTask(vbid, vbucket_state_active);

    auto vb = store->getVBuckets().getBucket(vbid);
    ASSERT_NE(nullptr, vb.get());

    // prep data
    store_item(vbid, makeStoredDocKey("key1"), "value");
    store_item(vbid, makeStoredDocKey("key2"), "value");

    auto& ckpt_mgr = *vb->checkpointManager;
    ASSERT_EQ(1, ckpt_mgr.getNumCheckpoints());

    // make checkpoint to cause backfill later rather than straight to in-memory
    ckpt_mgr.createNewCheckpoint();
    bool new_ckpt_created;
    ASSERT_EQ(2, ckpt_mgr.removeClosedUnrefCheckpoints(*vb, new_ckpt_created));

    // Create a Mock Dcp producer
    const std::string testName("test_producer");
    auto producer = std::make_shared<MockDcpProducer>(
            *engine,
            cookie,
            testName,
            /*flags*/ 0,
            cb::const_byte_buffer() /*no json*/);

    // Since we are creating a mock active stream outside of
    // DcpProducer::streamRequest(), and we want the checkpt processor task,
    // create it explicitly here
    producer->createCheckpointProcessorTask();
    producer->scheduleCheckpointProcessorTask();

    // Create a Mock Active Stream
    auto mock_stream = std::make_shared<MockActiveStream>(
            static_cast<EventuallyPersistentEngine*>(engine.get()),
            producer,
            /*flags*/ 0,
            /*opaque*/ 0,
            *vb,
            /*st_seqno*/ 0,
            /*en_seqno*/ ~0,
            /*vb_uuid*/ 0xabcd,
            /*snap_start_seqno*/ 0,
            /*snap_end_seqno*/ ~0,
            IncludeValue::Yes,
            IncludeXattrs::Yes);

    ASSERT_TRUE(mock_stream->isPending()) << "stream state should be Pending";

    mock_stream->transitionStateToBackfilling();

    ASSERT_TRUE(mock_stream->isBackfilling())
            << "stream state should have transitioned to Backfilling";

    size_t byteLimit = engine->getConfiguration().getDcpScanByteLimit();

    auto& manager = producer->getBFM();

    /* Hack to make DCPBackfillMemoryBuffered::create construct the range
     * iterator, but DCPBackfillMemoryBuffered::scan /not/ complete the
     * backfill immediately - we pretend the buffer is full. This is
     * reset in manager->backfill() */
    manager.bytesCheckAndRead(byteLimit + 1);

    // Directly run backfill once, to create the range iterator
    manager.backfill();

    const char* vbDeleteTaskName = "Removing (dead) vb:0 from memory";
    ASSERT_FALSE(
            task_executor->isTaskScheduled(NONIO_TASK_IDX, vbDeleteTaskName));

    /* Bin the vbucket. This will eventually lead to the destruction of
     * the seqList. If the vb were to be destroyed *now*,
     * AddressSanitizer would report heap-use-after-free when the
     * DCPBackfillMemoryBuffered is destroyed (it owns a range iterator)
     * This should no longer happen, as the backfill now hold a
     * shared_ptr to the evb.
     */
    store->deleteVBucket(vbid, nullptr);
    vb.reset();

    // vb can't yet be deleted, there is a range iterator over it still!
    EXPECT_FALSE(
            task_executor->isTaskScheduled(NONIO_TASK_IDX, vbDeleteTaskName));

    auto& lpAuxioQ = *task_executor->getLpTaskQ()[AUXIO_TASK_IDX];
    // Now bin the producer
    producer->cancelCheckpointCreatorTask();
    /* Checkpoint processor task finishes up and releases its producer
       reference */
    runNextTask(lpAuxioQ, "Process checkpoint(s) for DCP producer " + testName);

    engine->getDcpConnMap().shutdownAllConnections();
    mock_stream.reset();
    producer.reset();

    // run the backfill task so the backfill can reach state
    // backfill_finished and be destroyed destroying the range iterator
    // in the process
    runNextTask(lpAuxioQ, "Backfilling items for a DCP Connection");

    // Now the backfill is gone, the evb can be deleted
    EXPECT_TRUE(
            task_executor->isTaskScheduled(NONIO_TASK_IDX, vbDeleteTaskName));
}

class SingleThreadedEphemeralPurgerTest : public SingleThreadedKVBucketTest {
protected:
    void SetUp() override {
        config_string +=
                "bucket_type=ephemeral;"
                "max_vbuckets=" + std::to_string(numVbs) + ";"
                "ephemeral_metadata_purge_age=0;"
                "ephemeral_metadata_purge_stale_chunk_duration=0";
        SingleThreadedKVBucketTest::SetUp();

        /* Set up 4 vbuckets */
        for (int vbid = 0; vbid < numVbs; ++vbid) {
            setVBucketStateAndRunPersistTask(vbid, vbucket_state_active);
        }
    }

    bool checkAllPurged(uint64_t expPurgeUpto) {
        for (int vbid = 0; vbid < numVbs; ++vbid) {
            if (store->getVBucket(vbid)->getPurgeSeqno() < expPurgeUpto) {
                return false;
            }
        }
        return true;
    }
    const int numVbs = 4;
};

TEST_F(SingleThreadedEphemeralPurgerTest, PurgeAcrossAllVbuckets) {
    /* Set 100 item in all vbuckets. We need hundred items atleast because
       our ProgressTracker checks whether to pause only after
       INITIAL_VISIT_COUNT_CHECK = 100 */
    const int numItems = 100;
    for (int vbid = 0; vbid < numVbs; ++vbid) {
        for (int i = 0; i < numItems; ++i) {
            const std::string key("key" + std::to_string(vbid) +
                                  std::to_string(i));
            store_item(vbid, makeStoredDocKey(key), "value");
        }
    }

    /* Add and delete an item in every vbucket */
    for (int vbid = 0; vbid < numVbs; ++vbid) {
        const std::string key("keydelete" + std::to_string(vbid));
        storeAndDeleteItem(vbid, makeStoredDocKey(key), "value");
    }

    /* We have added an item at seqno 100 and deleted it immediately */
    const uint64_t expPurgeUpto = numItems + 2;

    /* Add another item as we do not purge last element in the list */
    for (int vbid = 0; vbid < numVbs; ++vbid) {
        const std::string key("afterdelete" + std::to_string(vbid));
        store_item(vbid, makeStoredDocKey(key), "value");
    }

    /* Run the HTCleaner task, so that we can wake up the stale item deleter */
    EphemeralBucket* bucket = dynamic_cast<EphemeralBucket*>(store);
    bucket->enableTombstonePurgerTask();
    bucket->attemptToFreeMemory(); // this wakes up the HTCleaner task

    auto& lpAuxioQ = *task_executor->getLpTaskQ()[NONIO_TASK_IDX];
    /* Run the HTCleaner and EphTombstoneStaleItemDeleter tasks. We expect
       pause and resume of EphTombstoneStaleItemDeleter atleast once and we run
       until all the deleted items across all the vbuckets are purged */
    int numPaused = 0;
    while (!checkAllPurged(expPurgeUpto)) {
        runNextTask(lpAuxioQ);
        ++numPaused;
    }
    EXPECT_GT(numPaused, 2 /* 1 run of 'HTCleaner' and more than 1 run of
                              'EphTombstoneStaleItemDeleter' */);
}
