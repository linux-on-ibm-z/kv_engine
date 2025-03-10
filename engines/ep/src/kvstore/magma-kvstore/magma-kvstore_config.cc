/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2018-Present Couchbase, Inc.
 *
 *   Use of this software is governed by the Business Source License included
 *   in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
 *   in that file, in accordance with the Business Source License, use of this
 *   software will be governed by the Apache License, Version 2.0, included in
 *   the file licenses/APL2.txt.
 */

#include "magma-kvstore_config.h"

#include "configuration.h"
#include "magma-kvstore.h"

#include <memcached/server_core_iface.h>

/// A listener class to update MagmaKVSTore related configs at runtime.
class MagmaKVStoreConfig::ConfigChangeListener : public ValueChangedListener {
public:
    ConfigChangeListener(MagmaKVStoreConfig& c) : config(c) {
    }

    void sizeValueChanged(const std::string& key, size_t value) override {
        if (key == "magma_fragmentation_percentage") {
            config.setMagmaFragmentationPercentage(value);
        } else if (key == "magma_flusher_thread_percentage") {
            config.setMagmaFlusherThreadPercentage(value);
        } else if (key == "persistent_metadata_purge_age") {
            config.setMetadataPurgeAge(value);
        }
    }

    void floatValueChanged(const std::string& key, float value) override {
        if (key == "magma_mem_quota_ratio") {
            config.setMagmaMemQuotaRatio(value);
        }
    }

    void stringValueChanged(const std::string& key,
                            const char* value) override {
        if (key == "vbucket_mapping_sanity_checking_error_mode") {
            config.setVBucketMappingErrorHandlingMethod(
                    cb::getErrorHandlingMethod(value));
        }
    }

    void booleanValueChanged(const std::string& key, bool b) override {
        if (key == "vbucket_mapping_sanity_checking") {
            config.setSanityCheckVBucketMapping(b);
        }
    }

private:
    MagmaKVStoreConfig& config;
};

MagmaKVStoreConfig::MagmaKVStoreConfig(Configuration& config,
                                       std::string_view backend,
                                       uint16_t numShards,
                                       uint16_t shardid)
    : KVStoreConfig(config, backend, numShards, shardid) {
    bucketQuota = config.getMaxSize();
    magmaDeleteMemtableWritecache = config.getMagmaDeleteMemtableWritecache();
    magmaDeleteFragRatio = config.getMagmaDeleteFragRatio();
    magmaMaxCheckpoints = config.getMagmaMaxCheckpoints();
    magmaCheckpointInterval =
            std::chrono::milliseconds(1s * config.getMagmaCheckpointInterval());
    magmaMinCheckpointInterval = std::chrono::milliseconds(
            1s * config.getMagmaMinCheckpointInterval());
    magmaCheckpointThreshold = config.getMagmaCheckpointThreshold();
    magmaHeartbeatInterval =
            std::chrono::milliseconds(1s * config.getMagmaHeartbeatInterval());
    magmaValueSeparationSize = config.getMagmaValueSeparationSize();
    magmaMemQuotaRatio = config.getMagmaMemQuotaRatio();
    magmaWriteCacheRatio = config.getMagmaWriteCacheRatio();
    magmaMaxWriteCache = config.getMagmaMaxWriteCache();
    magmaEnableDirectIo = config.isMagmaEnableDirectIo();
    magmaInitialWalBufferSize = config.getMagmaInitialWalBufferSize();
    magmaSyncEveryBatch = config.isMagmaSyncEveryBatch();
    magmaEnableUpsert = config.isMagmaEnableUpsert();
    magmaExpiryFragThreshold = config.getMagmaExpiryFragThreshold();
    magmaExpiryPurgerInterval =
            std::chrono::seconds(config.getMagmaExpiryPurgerInterval());
    magmaEnableBlockCache = config.isMagmaEnableBlockCache();
    magmaFragmentationPercentage = config.getMagmaFragmentationPercentage();
    magmaFlusherPercentage = config.getMagmaFlusherThreadPercentage();
    magmaMaxRecoveryBytes = config.getMagmaMaxRecoveryBytes();
    magmaMaxLevel0TTL =
            std::chrono::seconds(1s * config.getMagmaMaxLevel0Ttl());
    magmaMaxDefaultStorageThreads = config.getMagmaMaxDefaultStorageThreads();
    metadataPurgeAge = config.getPersistentMetadataPurgeAge();
    magmaBloomFilterAccuracy = config.getMagmaBloomFilterAccuracy();
    magmaBloomFilterAccuracyForBottomLevel =
            config.getMagmaBloomFilterAccuracyForBottomLevel();
    magmaEnableWAL = config.isMagmaEnableWal();
    magmaEnableGroupCommit = config.isMagmaEnableGroupCommit();
    magmaGroupCommitMaxSyncWaitDuration = std::chrono::milliseconds(
            config.getMagmaGroupCommitMaxSyncWaitDurationMs());
    magmaGroupCommitMaxTransactionCount =
            config.getMagmaGroupCommitMaxTransactionCount();

    config.addValueChangedListener(
            "magma_fragmentation_percentage",
            std::make_unique<ConfigChangeListener>(*this));
    config.addValueChangedListener(
            "num_writer_threads",
            std::make_unique<ConfigChangeListener>(*this));
    config.addValueChangedListener(
            "magma_flusher_thread_percentage",
            std::make_unique<ConfigChangeListener>(*this));
    config.addValueChangedListener(
            "persistent_metadata_purge_age",
            std::make_unique<ConfigChangeListener>(*this));
    config.addValueChangedListener(
            "magma_mem_quota_ratio",
            std::make_unique<ConfigChangeListener>(*this));

    sanityCheckVBucketMapping = config.isVbucketMappingSanityChecking();
    vBucketMappingErrorHandlingMethod = cb::getErrorHandlingMethod(
            config.getVbucketMappingSanityCheckingErrorMode());

    config.addValueChangedListener(
            "vbucket_mapping_sanity_checking",
            std::make_unique<ConfigChangeListener>(*this));
    config.addValueChangedListener(
            "vbucket_mapping_sanity_checking_error_mode",
            std::make_unique<ConfigChangeListener>(*this));
}

void MagmaKVStoreConfig::setStore(MagmaKVStore* store) {
    this->store = store;
}

void MagmaKVStoreConfig::setMagmaFragmentationPercentage(size_t value) {
    magmaFragmentationPercentage.store(value);
    if (store) {
        store->setMagmaFragmentationPercentage(value);
    }
}

void MagmaKVStoreConfig::setStorageThreads(
        ThreadPoolConfig::StorageThreadCount value) {
    storageThreads.store(value);
    if (store) {
        store->calculateAndSetMagmaThreads();
    }
}

void MagmaKVStoreConfig::setMagmaFlusherThreadPercentage(size_t value) {
    magmaFlusherPercentage.store(value);
    if (store) {
        store->calculateAndSetMagmaThreads();
    }
}

void MagmaKVStoreConfig::setBucketQuota(size_t value) {
    // Just update the cached value, the update to magma is driven via the
    // engine.
    bucketQuota.store(value);
}

void MagmaKVStoreConfig::setMagmaMemQuotaRatio(float value) {
    magmaMemQuotaRatio.store(value);
    if (store) {
        store->setMaxDataSize(bucketQuota);
    }
}