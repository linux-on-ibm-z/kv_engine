/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2017-Present Couchbase, Inc.
 *
 *   Use of this software is governed by the Business Source License included
 *   in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
 *   in that file, in accordance with the Business Source License, use of this
 *   software will be governed by the Apache License, Version 2.0, included in
 *   the file licenses/APL2.txt.
 */

#pragma once

#include "dcp/backfill.h"
#include "dcp/backfill_by_seqno.h"
#include "ephemeral_vb.h"

/**
 * Concrete class that does backfill from in-memory ordered data structure and
 * informs the DCP stream of the backfill progress.
 *
 * This class creates a range iterator on the in-memory seqList, then
 * during scan() reads items one by one, passing to the given ActiveStream
 * for disk snapshot, backfill items and backfill completion.
 */
class DCPBackfillMemoryBuffered : public DCPBackfill,
                                  public DCPBackfillBySeqno {
public:
    DCPBackfillMemoryBuffered(EphemeralVBucketPtr evb,
                              std::shared_ptr<ActiveStream> s,
                              uint64_t startSeqno,
                              uint64_t endSeqno);

    ~DCPBackfillMemoryBuffered() override;

    backfill_status_t run() override;

    void cancel() override;

private:
    /* The possible states of the DCPBackfillMemoryBuffered */
    enum class BackfillState : uint8_t { Init, Scanning, Done };

    static std::string backfillStateToString(BackfillState state);

    /**
     * Creates a range iterator on Ephemeral VBucket to read items as a snapshot
     * in sequential order. Backfill snapshot range is decided here.
     */
    backfill_status_t create();

    /**
     * Reads the items in the snapshot (iterator) one by one. In case of high
     * memory usage postpones the reading of items, and reading can be resumed
     * later on from that point.
     */
    backfill_status_t scan();

    /**
     * Indicates the completion to the stream.
     *
     * @param cancelled indicates if the backfill finished fully or was
     *                  cancelled in between; for debug
     */
    void complete(bool cancelled);

    /**
     * Makes valid transitions on the backfill state machine
     */
    void transitionState(BackfillState newState);

    /**
     * shared pointer to EphemeralVBucket. Needs to be shared as we cannot
     * delete the underlying VBucket while we have an iterator active on it.
     */
    std::shared_ptr<EphemeralVBucket> evb;

    BackfillState state;

    /**
     * Range iterator (on the vbucket) created for the backfill
     */
    SequenceList::RangeIterator rangeItr;
};
