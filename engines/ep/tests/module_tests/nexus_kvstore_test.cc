/*
 *     Copyright 2021-Present Couchbase, Inc.
 *
 *   Use of this software is governed by the Business Source License included
 *   in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
 *   in that file, in accordance with the Business Source License, use of this
 *   software will be governed by the Apache License, Version 2.0, included in
 *   the file licenses/APL2.txt.
 */

#include "evp_store_single_threaded_test.h"

#include "collections/collections_test_helpers.h"

#include "../mock/mock_ep_bucket.h"
#include "../mock/mock_synchronous_ep_engine.h"
#include "collections/collections_test_helpers.h"
#include "item.h"
#include "kv_bucket.h"
#include "kvstore/nexus-kvstore/nexus-kvstore.h"
#include "test_helpers.h"
#include "test_manifest.h"
#include "tests/module_tests/thread_gate.h"
#include "vbucket.h"
#include "vbucket_bgfetch_item.h"
#include "vbucket_state.h"

// GTest warns about uninstantiated tests and currently the only variants of
// Nexus tests are EE only as they require magma. NexusKVStore supports other
// variants though which aren't necessarily EE, but we don't currently test.
// Instead of making this entire test suite EE only, just supress the warning.
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(NexusKVStoreTest);

/**
 * Test fixture for the NexusKVStore test harness
 */
class NexusKVStoreTest : public STParamPersistentBucketTest {
public:
    void SetUp() override {
        config_string += "nexus_concurrent_flush_compaction_enabled=true";
        STParamPersistentBucketTest::SetUp();
    }

    static auto couchstoreMagmaVariants() {
        using namespace std::string_literals;
        return ::testing::Values(
#ifdef EP_USE_MAGMA
                std::make_tuple("persistent_nexus_couchstore_magma"s,
                                "value_only"s),
                std::make_tuple("persistent_nexus_couchstore_magma"s,
                                "full_eviction"s),
                std::make_tuple("persistent_nexus_magma_couchstore"s,
                                "value_only"s),
                std::make_tuple("persistent_nexus_magma_couchstore"s,
                                "full_eviction"s)
#endif
        );
    }

    /**
     * Test helper that runs an implicit compaction test
     *
     * @param storeItemsForTest function storing the item(s) we want to test
     */
    void implicitCompactionTest(
            std::function<void()> storeItemsForTest,
            std::function<void()> postPurgeSeqnoUpdateFn = []() {});

    /**
     * Test helper that runs a test which purges a prepare in an implicit
     * compaction
     *
     * @param purgedKey key to purge
     */
    void implicitCompactionPrepareTest(StoredDocKey purgedKey);

    /**
     * Test helper that runs a test which purges a logical deletion in an
     * implicit compaction
     *
     * @param purgedKey key to purge
     */
    void implicitCompactionLogicalDeleteTest(StoredDocKey purgedKey);

    /**
     * Test helper function that runs various KVStore functions in some manner
     * to determine if NexusKVStore correctly deals with items that may have
     * been purged by one KVStore but not the other
     *
     * @param purgedKey key to check
     * @param purgedKeySeqno seqno fo purged key
     */
    void implicitCompactionTestChecks(DiskDocKey purgedKey,
                                      uint64_t purgedKeySeqno);

    /**
     * Test that we correclty skip comparisons if we are purging collections as
     * the compactions are non-comparable between MagmaKVStore and CouchKVStore
     *
     * @param withItems Should the collection contain items?
     */
    void collectionDropCompactionTest(bool withItems);

    bool isMagmaPrimary() {
        return engine->getConfiguration().getNexusPrimaryBackend() == "magma";
    }
};

void NexusKVStoreTest::implicitCompactionTest(
        std::function<void()> storeItemsForTest,
        std::function<void()> postPurgeSeqnoUpdateFn) {
    // Function to perform 16 writes so that we hit the LSMMaxNumLevel0Tables
    // threshold which will trigger implicit compaction
    auto performWritesForImplicitCompaction = [this]() -> void {
        for (int i = 0; i < 14; i++) {
            store_item(
                    vbid, makeStoredDocKey("key" + std::to_string(i)), "value");
            flushVBucketToDiskIfPersistent(vbid, 1);
        }
    };

    setVBucketStateAndRunPersistTask(
            vbid,
            vbucket_state_active,
            {{"topology", nlohmann::json::array({{"active", "replica"}})}});

    ThreadGate tg(2);
    auto& bucket = dynamic_cast<EPBucket&>(*store);
    bucket.postPurgeSeqnoImplicitCompactionHook =
            [&tg, &postPurgeSeqnoUpdateFn]() -> void { tg.threadUp(); };

    auto dummyKey = makeStoredDocKey("keyA");
    store_item(vbid, dummyKey, "value");
    delete_item(vbid, dummyKey);
    flushVBucketToDiskIfPersistent(vbid, 1);
    TimeTraveller timmy{60 * 60 * 24 * 5};

    performWritesForImplicitCompaction();

    storeItemsForTest();

    // Wait till the purge seqno has been set
    tg.threadUp();

    postPurgeSeqnoUpdateFn();

    // Write and flush another value to cause a Sync in magma to occur which
    // will ensure that firstDeletedKey is no longer visible
    store_item(vbid, makeStoredDocKey("dummy2"), "value");
    flushVBucketToDiskIfPersistent(vbid, 1);
}

TEST_P(NexusKVStoreTest, MagmaImplicitCompactionExpires) {
    auto expiredKey = makeStoredDocKey("keyPrepare");
    implicitCompactionTest(
            [this, &expiredKey]() {
                auto vb = store->getVBucket(vbid);

                store_item(vbid, expiredKey, "value", 1 /*exptime*/);
                flushVBucketToDiskIfPersistent(vbid, 1);

                store_item(vbid,
                           makeStoredDocKey("dummyForImplicitThreshold"),
                           "value");
                flushVBucketToDiskIfPersistent(vbid, 1);
            },
            [this]() {
                if (isMagmaPrimary()) {
                    // Magma primary is allowed to expire things and this test
                    // will attempt to expire something. To prevent a race
                    // causing us to flush more items should we get around to
                    // processing this expiry we need to wait for the flush to
                    // go through before we can continue.
                    const auto time_limit = std::chrono::seconds(10);
                    const auto deadline =
                            std::chrono::steady_clock::now() + time_limit;
                    auto flushed = 0;

                    do {
                        const auto [moreAvailable, wake, numFlushed] =
                                dynamic_cast<EPBucket&>(*store).flushVBucket(
                                        vbid);
                        flushed += numFlushed;
                        std::this_thread::sleep_for(
                                std::chrono::microseconds(100));
                        if (flushed == 1) {
                            break;
                        }

                    } while ((std::chrono::steady_clock::now() < deadline));

                    ASSERT_EQ(1, flushed)
                            << "Hit timeout (" << time_limit.count()
                            << " seconds) waiting for the flush of the "
                               "expiration";
                }
            });

    // Get from memory - jumping straight to the HT for this as a regular get
    // would expire it for us
    auto vb = store->getVBucket(vbid);
    { // Scope for HT lock
        auto htRes = vb->ht.findForRead(expiredKey);
        if (isMagmaPrimary()) {
            EXPECT_FALSE(htRes.storedValue);
        } else {
            ASSERT_TRUE(htRes.storedValue);
            EXPECT_FALSE(htRes.storedValue->isDeleted());
        }
    }
}

void NexusKVStoreTest::implicitCompactionPrepareTest(StoredDocKey purgedKey) {
    uint64_t purgedPrepareSeqno;
    implicitCompactionTest([this, &purgedKey, &purgedPrepareSeqno]() {
        auto vb = store->getVBucket(vbid);

        store_item(vbid,
                   purgedKey,
                   "value",
                   0 /*exptime*/,
                   {cb::engine_errc::sync_write_pending} /*expected*/,
                   PROTOCOL_BINARY_RAW_BYTES,
                   {cb::durability::Requirements()});
        flushVBucketToDiskIfPersistent(vbid, 1);

        purgedPrepareSeqno = vb->getHighSeqno();

        EXPECT_EQ(cb::engine_errc::success,
                  vb->seqnoAcknowledged(
                          folly::SharedMutex::ReadHolder(vb->getStateLock()),
                          "replica",
                          vb->getHighSeqno() /*prepareSeqno*/));
        vb->processResolvedSyncWrites();
        flushVBucketToDiskIfPersistent(vbid, 1);
    });

    auto key = DiskDocKey(purgedKey, true /*prepare*/);

    implicitCompactionTestChecks(key, purgedPrepareSeqno);
}

void NexusKVStoreTest::implicitCompactionTestChecks(DiskDocKey key,
                                                    uint64_t purgedKeySeqno) {
    auto* kvstore = store->getRWUnderlying(vbid);
    // 1) vBucket states.
    // Don't care about the result, just want Nexus checks to run
    kvstore->getCachedVBucketState(vbid);
    kvstore->getPersistedVBucketState(vbid);

    // 2) Gets
    auto gv = kvstore->get(key, vbid);
    gv = kvstore->getWithHeader(*kvstore->makeFileHandle(vbid),
                                key,
                                vbid,
                                ValueFilter::VALUES_DECOMPRESSED);
    kvstore->getRange(Vbid{0},
                      makeDiskDocKey("a", true),
                      makeDiskDocKey("z", true),
                      ValueFilter::KEYS_ONLY,
                      [](GetValue&& cb) {});

    kvstore->getBySeqno(*kvstore->makeFileHandle(vbid),
                        vbid,
                        purgedKeySeqno,
                        ValueFilter::VALUES_DECOMPRESSED);

    // Write an extra item for GetAllKeys because we want something after the
    // prepare
    store_item(vbid,
               makeStoredDocKey("keyZZ"),
               "value2",
               0 /*exptime*/,
               {cb::engine_errc::sync_write_pending} /*expected*/,
               PROTOCOL_BINARY_RAW_BYTES,
               {cb::durability::Requirements()});
    flushVBucketToDiskIfPersistent(vbid, 1);

    class CB : public StatusCallback<const DiskDocKey&> {
        void callback(const DiskDocKey&) override {
        }
    };
    kvstore->getAllKeys(vbid,
                        DiskDocKey(makeStoredDocKey("a")),
                        100,
                        std::make_shared<CB>());

    // 3) BG Fetch - can't hit the typical API as we can't fetch prepares or
    // logically deleted items (the two types of items an implicit compaction
    // might purge that we care about)
    vb_bgfetch_queue_t q;

    vb_bgfetch_item_ctx_t& bgfetch_itm_ctx = q[key];

    bgfetch_itm_ctx.addBgFetch(std::make_unique<FrontEndBGFetchItem>(
            cookie, ValueFilter::VALUES_DECOMPRESSED, 0 /*token*/));
    kvstore->getMulti(vbid, q);

    // 4) Scan
    class CacheLookupCb : public StatusCallback<CacheLookup> {
    public:
        void callback(CacheLookup& lookup) override {
        }
    };

    class ScanCb : public StatusCallback<GetValue> {
    public:
        void callback(GetValue& result) override {
        }
    };

    auto cl = std::make_unique<CacheLookupCb>();
    auto cb = std::make_unique<ScanCb>();
    auto scanCtx =
            kvstore->initBySeqnoScanContext(std::move(cb),
                                            std::move(cl),
                                            vbid,
                                            1,
                                            DocumentFilter::ALL_ITEMS,
                                            ValueFilter::VALUES_COMPRESSED,
                                            SnapshotSource::Head);
    kvstore->scan(*scanCtx);

    // 5) Misc stuff
    kvstore->getItemCount(vbid);
    kvstore->getCollectionsManifest(vbid);
}

TEST_P(NexusKVStoreTest, DropCollectionMidFlush) {
    setVBucketStateAndRunPersistTask(vbid, vbucket_state_active);
    auto vb = store->getVBucket(vbid);

    CollectionsManifest cm;
    vb->updateFromManifest(makeManifest(cm.add(CollectionEntry::meat)));
    flushVBucketToDiskIfPersistent(vbid, 1);
    store_item(vbid,
               makeStoredDocKey("keyA", CollectionEntry::meat.getId()),
               "biggerValues",
               0 /*exptime*/,
               {cb::engine_errc::success} /*expected*/,
               PROTOCOL_BINARY_RAW_BYTES);

    // Previously the post commit checks would fail to find the dropped
    // collection in the manifest but assumed it was there and segfaulted
    auto* kvstore = store->getRWUnderlying(vbid);
    kvstore->setPostFlushHook([this, &vb, &cm]() {
        vb->updateFromManifest(makeManifest(cm.remove(CollectionEntry::meat)));
    });

    flushVBucketToDiskIfPersistent(vbid, 1);
}

TEST_P(NexusKVStoreTest, MagmaImplicitCompactionPurgesPrepareFlush) {
    auto purgedKey = makeStoredDocKey("keyPrepare");
    implicitCompactionPrepareTest(purgedKey);

    // Flushing has a different flush state
    store_item(vbid,
               purgedKey,
               "value2",
               0 /*exptime*/,
               {cb::engine_errc::sync_write_pending} /*expected*/,
               PROTOCOL_BINARY_RAW_BYTES,
               {cb::durability::Requirements()});
    flushVBucketToDiskIfPersistent(vbid, 1);
}

TEST_P(NexusKVStoreTest, MagmaImplicitCompactionPurgesPrepareFlushDelete) {
    auto purgedKey = makeStoredDocKey("keyPrepare");
    implicitCompactionPrepareTest(purgedKey);

    // Flushing has a different flush state - we hit a different function for
    // deletes
    store_item(vbid,
               purgedKey,
               "value2",
               0 /*exptime*/,
               {cb::engine_errc::sync_write_pending} /*expected*/,
               PROTOCOL_BINARY_RAW_BYTES,
               {cb::durability::Requirements()},
               true /*deleted*/);
    flushVBucketToDiskIfPersistent(vbid, 1);
}

TEST_P(NexusKVStoreTest, MagmaImplicitCompactionPurgesPrepareCompaction) {
    auto purgedKey = makeStoredDocKey("keyPrepare");
    implicitCompactionPrepareTest(purgedKey);

    auto* kvstore = store->getRWUnderlying(vbid);

    // Don't really expect this to do anything as we don't invoke any callback
    // when purging a prepare, but testing is always good
    CompactionConfig config;
    auto vb = store->getVBucket(vbid);
    auto ctx = std::make_shared<CompactionContext>(vb, config, 0);
    std::mutex dummyLock;
    std::unique_lock<std::mutex> lh(dummyLock);
    kvstore->compactDB(lh, ctx);
}

TEST_P(NexusKVStoreTest, MagmaImplicitCompactionPurgesPrepareRollback) {
    auto purgedKey = makeStoredDocKey("keyPrepare");
    implicitCompactionPrepareTest(purgedKey);

    class RollbackCallback : public RollbackCB {
        void callback(GetValue& val) override {
        }
    };

    auto* kvstore = store->getRWUnderlying(vbid);
    auto rollbackResult = kvstore->rollback(
            Vbid(0), 16, std::make_unique<RollbackCallback>());
    EXPECT_TRUE(rollbackResult.success);
    EXPECT_EQ(16, rollbackResult.highSeqno);
}

void NexusKVStoreTest::implicitCompactionLogicalDeleteTest(
        StoredDocKey purgedKey) {
    uint64_t purgedKeySeqno;
    implicitCompactionTest([this, &purgedKey, &purgedKeySeqno]() {
        auto vb = store->getVBucket(vbid);
        CollectionsManifest cm;
        cm.add(CollectionEntry::fruit);

        vb->updateFromManifest(makeManifest(cm));

        store_item(vbid, purgedKey, "value");
        purgedKeySeqno = vb->getHighSeqno();

        flushVBucketToDiskIfPersistent(vbid, 2);
        cm.remove(CollectionEntry::fruit);

        vb->updateFromManifest(makeManifest(cm));

        flushVBucketToDiskIfPersistent(vbid, 1);
    });

    implicitCompactionTestChecks(DiskDocKey(purgedKey), purgedKeySeqno);
}

TEST_P(NexusKVStoreTest, MagmaImplicitCompactionPurgesLogicallyDeletedItem) {
    auto purgedKey = makeStoredDocKey("key", CollectionEntry::fruit.getId());
    implicitCompactionLogicalDeleteTest(purgedKey);

    auto* kvstore = store->getRWUnderlying(vbid);

    CompactionConfig config;
    auto vb = store->getVBucket(vbid);
    auto ctx = std::make_shared<CompactionContext>(vb, config, 0);
    std::mutex dummyLock;
    std::unique_lock<std::mutex> lh(dummyLock);
    kvstore->compactDB(lh, ctx);
}

TEST_P(NexusKVStoreTest, SecondaryExpiresFromSameTime) {
    setVBucketStateAndRunPersistTask(vbid, vbucket_state_active);

    auto* kvstore = store->getRWUnderlying(vbid);
    ASSERT_TRUE(kvstore);

    // Expire this item in the future
    auto expiredKey = makeStoredDocKey("key");
    auto timeNow = ep_real_time();
    store_item(vbid, expiredKey, "value", timeNow + 10 /*exptime*/);
    flushVBucketToDiskIfPersistent(vbid, 1);

    auto& nexusKVStore = dynamic_cast<NexusKVStore&>(*kvstore);
    std::unique_ptr<TimeTraveller> t;
    nexusKVStore.preCompactionHook = [this, &expiredKey, &t]() {
        // Jump forwards to check that primary and secondary use the same
        // expiry point
        t = std::make_unique<TimeTraveller>(100);
    };

    runCompaction(vbid);
}

void NexusKVStoreTest::collectionDropCompactionTest(bool withItems) {
    setVBucketStateAndRunPersistTask(
            vbid,
            vbucket_state_active,
            {{"topology", nlohmann::json::array({{"active", "replica"}})}});

    // Expire something in the default collection. When we purge CouchKVStore
    // will visit and attempt to expire this item, MagmaKVStore will not.
    auto expiredKey = makeStoredDocKey("key");
    store_item(vbid, expiredKey, "value", 1 /*exptime*/);
    flushVBucketToDiskIfPersistent(vbid, 1);

    CollectionsManifest cm;
    setCollections(cookie, cm.add(CollectionEntry::fruit));
    flushVBucketToDiskIfPersistent(vbid, 1);

    if (withItems) {
        auto collectionKey = makeStoredDocKey("key", CollectionEntry::fruit);
        store_item(vbid, collectionKey, "value", 1 /*exptime*/);
        flushVBucketToDiskIfPersistent(vbid, 1);
    }

    setCollections(cookie, cm.remove(CollectionEntry::fruit));
    flushVBucketToDiskIfPersistent(vbid, 1);

    runCompaction(vbid);
}

TEST_P(NexusKVStoreTest, CollectionDropCompactionWithItems) {
    collectionDropCompactionTest(true);
}

TEST_P(NexusKVStoreTest, CollectionDropCompactionWithoutItems) {
    collectionDropCompactionTest(false);
}

TEST_P(NexusKVStoreTest, CollectionDropCompactionPurgeItem) {
    setVBucketStateAndRunPersistTask(
            vbid,
            vbucket_state_active,
            {{"topology", nlohmann::json::array({{"active", "replica"}})}});

    auto* kvstore = store->getRWUnderlying(vbid);
    ASSERT_TRUE(kvstore);

    // Drop a key
    auto purgedKey = makeStoredDocKey("key");
    store_item(vbid, purgedKey, "value");
    delete_item(vbid, purgedKey);
    flushVBucketToDiskIfPersistent(vbid, 1);

    CollectionsManifest cm;
    setCollections(cookie, cm.add(CollectionEntry::fruit));
    flushVBucketToDiskIfPersistent(vbid, 1);

    setCollections(cookie, cm.remove(CollectionEntry::fruit));
    flushVBucketToDiskIfPersistent(vbid, 1);

    TimeTraveller timmy{60 * 60 * 24 * 5};
    runCompaction(vbid);

    // We should skip checks when updating the key as it exists in one
    // KVStore but not the other and is below the NexusKVStore purge seqno
    store_item(vbid, purgedKey, "value");
    flushVBucketToDiskIfPersistent(vbid, 1);
}

TEST_P(NexusKVStoreTest,
       ConcurrentCompactionFirstExpiresFlushBeforeCompactingSecond) {
    setVBucketStateAndRunPersistTask(
            vbid,
            vbucket_state_active,
            {{"topology", nlohmann::json::array({{"active", "replica"}})}});

    auto* kvstore = store->getRWUnderlying(vbid);
    ASSERT_TRUE(kvstore);

    // We need something that compaction will operate on that we can change
    // later. For this we'll us an expired item
    auto expiredKey = makeStoredDocKey("key");
    store_item(vbid, expiredKey, "value", 1 /*exptime*/);
    flushVBucketToDiskIfPersistent(vbid, 1);

    auto& nexusKVStore = dynamic_cast<NexusKVStore&>(*kvstore);
    nexusKVStore.midCompactionHook =
            [this, &expiredKey](std::unique_lock<std::mutex>& vbLock) {
                if (vbLock.owns_lock()) {
                    vbLock.unlock();
                }

                // Update the key - make it not expired
                store_item(vbid, expiredKey, "value");
                flushVBucketToDiskIfPersistent(vbid, 1);
            };

    runCompaction(vbid);
}

TEST_P(NexusKVStoreTest, ConcurrentCompactionPurgeFromOneKVStore) {
    setVBucketStateAndRunPersistTask(
            vbid,
            vbucket_state_active,
            {{"topology", nlohmann::json::array({{"active", "replica"}})}});

    auto* kvstore = store->getRWUnderlying(vbid);
    ASSERT_TRUE(kvstore);

    auto purgedKey = makeStoredDocKey("key");
    store_item(vbid, purgedKey, "value");
    delete_item(vbid, purgedKey);
    store_item(vbid, makeStoredDocKey("dummy"), "value");
    flushVBucketToDiskIfPersistent(vbid, 2);

    auto& nexusKVStore = dynamic_cast<NexusKVStore&>(*kvstore);
    nexusKVStore.midCompactionHook =
            [this, &purgedKey](std::unique_lock<std::mutex>& vbLock) {
                if (vbLock.owns_lock()) {
                    vbLock.unlock();
                }

                auto* kvstore = store->getROUnderlying(vbid);
                auto gv = kvstore->get(DiskDocKey(purgedKey), vbid);

                store_item(vbid, purgedKey, "value");
                flushVBucketToDiskIfPersistent(vbid, 1);
            };

    TimeTraveller t(std::numeric_limits<int>::max());
    runCompaction(vbid);

    // Reset and warmup to test that nexus pre-loads purge seqnos correctly.
    // If it does not pre-loads its cache then it will find that the purge
    // seqnos for the backends are different and error.
    resetEngineAndWarmup();
}

TEST_P(NexusKVStoreTest, ConcurrentCompactionLogicalDeletionToOneKVStore) {
    setVBucketStateAndRunPersistTask(
            vbid,
            vbucket_state_active,
            {{"topology", nlohmann::json::array({{"active", "replica"}})}});

    auto* kvstore = store->getRWUnderlying(vbid);
    ASSERT_TRUE(kvstore);

    auto vb = store->getVBucket(vbid);

    CollectionsManifest cm;
    cm.add(CollectionEntry::fruit);
    vb->updateFromManifest(makeManifest(cm));

    auto purgedKey = makeStoredDocKey("key", CollectionEntry::fruit.getId());
    store_item(vbid, purgedKey, "value");
    flushVBucketToDiskIfPersistent(vbid, 2);

    auto& nexusKVStore = dynamic_cast<NexusKVStore&>(*kvstore);
    nexusKVStore.midCompactionHook =
            [this, &purgedKey, &cm, &vb](std::unique_lock<std::mutex>& vbLock) {
                if (vbLock.owns_lock()) {
                    vbLock.unlock();
                }

                cm.remove(CollectionEntry::fruit);
                vb->updateFromManifest(makeManifest(cm));
                flushVBucketToDiskIfPersistent(vbid, 1);
            };

    TimeTraveller t(std::numeric_limits<int>::max());
    runCompaction(vbid);
}

TEST_P(NexusKVStoreTest, ConcurrentCompactionFlushResurrection) {
    setVBucketStateAndRunPersistTask(
            vbid,
            vbucket_state_active,
            {{"topology", nlohmann::json::array({{"active", "replica"}})}});

    auto* kvstore = store->getRWUnderlying(vbid);
    ASSERT_TRUE(kvstore);

    auto vb = store->getVBucket(vbid);

    CollectionsManifest cm;
    cm.add(CollectionEntry::fruit);
    vb->updateFromManifest(makeManifest(cm));

    auto purgedKey = makeStoredDocKey("key", CollectionEntry::fruit.getId());
    store_item(vbid, purgedKey, "value");
    flushVBucketToDiskIfPersistent(vbid, 2);

    cm.remove(CollectionEntry::fruit);
    vb->updateFromManifest(makeManifest(cm));
    flushVBucketToDiskIfPersistent(vbid, 1);

    auto& nexusKVStore = dynamic_cast<NexusKVStore&>(*kvstore);
    nexusKVStore.midCompactionHook =
            [this, &purgedKey, &cm, &vb](std::unique_lock<std::mutex>& vbLock) {
                if (vbLock.owns_lock()) {
                    vbLock.unlock();
                }

                auto* kvstore = store->getROUnderlying(vbid);
                auto gv = kvstore->get(DiskDocKey(purgedKey), vbid);

                cm.add(CollectionEntry::fruit);
                vb->updateFromManifest(makeManifest(cm));
                store_item(vbid, purgedKey, "value");
                flushVBucketToDiskIfPersistent(vbid, 2);
            };

    TimeTraveller t(std::numeric_limits<int>::max());
    runCompaction(vbid);
}

INSTANTIATE_TEST_SUITE_P(Nexus,
                         NexusKVStoreTest,
                         NexusKVStoreTest::couchstoreMagmaVariants(),
                         STParameterizedBucketTest::PrintToStringParamName);
