/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2020-Present Couchbase, Inc.
 *
 *   Use of this software is governed by the Business Source License included
 *   in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
 *   in that file, in accordance with the Business Source License, use of this
 *   software will be governed by the Apache License, Version 2.0, included in
 *   the file licenses/APL2.txt.
 */

#pragma once
#include "callbacks.h"
#include "dcp/backfill.h"

#include <chrono>
#include <mutex>

class ActiveStream;
class KVBucket;
class VBucket;
enum class ValueFilter;

/* The possible states of the DCPBackfillDisk */
enum backfill_state_t {
    backfill_state_init,
    backfill_state_scanning,
    backfill_state_completing,
    backfill_state_done
};

/* Callback to get the items that are found to be in the cache */
class CacheCallback : public StatusCallback<CacheLookup> {
public:
    CacheCallback(KVBucket& bucket, std::shared_ptr<ActiveStream> s);

    void callback(CacheLookup& lookup) override;

private:
    /**
     * Attempt to perform the get of lookup
     *
     * @return return a GetValue by performing a vb::get with lookup::getKey.
     */
    GetValue get(VBucket& vb, CacheLookup& lookup, ActiveStream& stream);

    KVBucket& bucket;
    std::weak_ptr<ActiveStream> streamPtr;
};

/* Callback to get the items that are found to be in the disk */
class DiskCallback : public StatusCallback<GetValue> {
public:
    explicit DiskCallback(std::shared_ptr<ActiveStream> s);

    void callback(GetValue& val) override;

private:
    std::weak_ptr<ActiveStream> streamPtr;
};

class DCPBackfillDisk : public virtual DCPBackfill {
public:
    explicit DCPBackfillDisk(KVBucket& bucket);

    ~DCPBackfillDisk() override;

protected:
    backfill_status_t run() override;
    void cancel() override;
    void transitionState(backfill_state_t newState);

    /**
     * Create the scan, intialising scanCtx from KVStore initScanContext
     */
    virtual backfill_status_t create() = 0;

    /**
     * Run the scan which will return items to the owning stream
     */
    virtual backfill_status_t scan() = 0;

    /**
     * Handles the completion of the backfill, e.g. notify completion status to
     * the stream.
     *
     * @param cancelled indicates the if backfill finished fully or was
     *                  cancelled in between; for debug
     */
    virtual void complete(bool cancelled) = 0;

    std::mutex lock;
    backfill_state_t state = backfill_state_init;

    KVBucket& bucket;

    std::unique_ptr<ScanContext> scanCtx;
};
