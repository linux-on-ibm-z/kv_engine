/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2019-Present Couchbase, Inc.
 *
 *   Use of this software is governed by the Business Source License included
 *   in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
 *   in that file, in accordance with the Business Source License, use of this
 *   software will be governed by the Apache License, Version 2.0, included in
 *   the file licenses/APL2.txt.
 */

#pragma once

#include "checkpoint.h"
#include "checkpoint_config.h"
#include "checkpoint_cursor.h"
#include "checkpoint_manager.h"

/*
 * Mock of the CheckpointManager class.
 */
class MockCheckpointManager : public CheckpointManager {
public:
    MockCheckpointManager(
            EPStats& st,
            VBucket& vbucket,
            CheckpointConfig& config,
            int64_t lastSeqno,
            uint64_t lastSnapStart,
            uint64_t lastSnapEnd,
            uint64_t maxVisibleSeqno,
            FlusherCallback cb,
            CheckpointDisposer ckptDisposer = ImmediateCkptDisposer)
        : CheckpointManager(st,
                            vbucket,
                            config,
                            lastSeqno,
                            lastSnapStart,
                            lastSnapEnd,
                            maxVisibleSeqno,
                            cb,
                            [this](CheckpointList&& list, const Vbid& vbid) {
                                testCkptDisposer(std::move(list), vbid);
                            }) {
        // The checkpoint disposer is const in CheckpointManager and should
        // never change in normal operation.
        // To allow it to be overriden in tests, rather than directly passing
        // the checkpoint disposer to CheckpointManager(), instead dispatch to
        // a test function here in MockCheckpointManager, which can be replaced
        // as necessary in tests.
        setCheckpointDisposer(ckptDisposer);
    }

    /**
     * Return the next item to be sent to a given connection.
     * @param cursor  pointer to the clients cursor, can be null
     * @param isLastMutationItem  flag indicating if the item to be returned is
     * the last mutation one in the closed checkpoint.
     * @return the next item to be sent to a given connection.
     */
    queued_item nextItem(CheckpointCursor* cursor, bool& isLastMutationItem) {
        std::lock_guard<std::mutex> lh(queueLock);
        static StoredDocKey emptyKey("", CollectionID::System);
        if (!cursor) {
            queued_item qi(
                    new Item(emptyKey, Vbid(0xffff), queue_op::empty, 0, 0));
            return qi;
        }
        if (getOpenCheckpointId(lh) == 0) {
            queued_item qi(
                    new Item(emptyKey, Vbid(0xffff), queue_op::empty, 0, 0));
            return qi;
        }

        if (incrCursor(*cursor)) {
            isLastMutationItem = isLastMutationItemInCheckpoint(*cursor);
            return *(cursor->currentPos);
        } else {
            isLastMutationItem = false;
            queued_item qi(
                    new Item(emptyKey, Vbid(0xffff), queue_op::empty, 0, 0));
            return qi;
        }
    }

    size_t getNumOfCursors() const {
        std::lock_guard<std::mutex> lh(queueLock);
        return cursors.size();
    }

    size_t getNumCheckpoints() const {
        std::lock_guard<std::mutex> lh(queueLock);
        return checkpointList.size();
    }

    const CheckpointList& getCheckpointList() const {
        return checkpointList;
    }

    queued_item public_createCheckpointItem(uint64_t id,
                                            Vbid vbid,
                                            queue_op checkpoint_op) {
        std::lock_guard<std::mutex> lh(queueLock);
        return createCheckpointItem(id, vbid, checkpoint_op);
    }

    void forceNewCheckpoint() {
        std::lock_guard<std::mutex> lh(queueLock);
        checkOpenCheckpoint(lh, true);
    }

    bool incrCursor(CheckpointCursor& cursor) {
        return CheckpointManager::incrCursor(cursor);
    }

    CheckpointType getOpenCheckpointType() const {
        std::lock_guard<std::mutex> lh(queueLock);
        return getOpenCheckpoint_UNLOCKED(lh).getCheckpointType();
    }

    auto getPersistenceCursorPos() const {
        std::lock_guard<std::mutex> lh(queueLock);
        return getPersistenceCursor()->currentPos;
    }

    /**
     * Set the checkpoint disposer that will be used if eager checkpoint removal
     * is configured.
     */
    void setCheckpointDisposer(CheckpointDisposer disposer) {
        testCkptDisposer = std::move(disposer);
    }

protected:
    CheckpointDisposer testCkptDisposer;
};
