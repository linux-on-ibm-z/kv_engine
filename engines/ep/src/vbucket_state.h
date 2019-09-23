/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2019 Couchbase, Inc
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
#pragma once

#include "ep_types.h"

#include <memcached/vbucket.h>
#include <nlohmann/json.hpp>
#include <platform/n_byte_integer.h>
#include <cstdint>
#include <string>

class Item;

// Sub structure that stores the data which only changes as part of a state
// transition.
struct vbucket_transition_state {
    bool needsToBePersisted(const vbucket_transition_state& transition);

    /// update the given item with a JSON version of this structure
    void toItem(Item& item) const;

    /// update this from the Item, assumes the Item's value was set by ::toItem
    void fromItem(const Item& item);

    std::string failovers = "";

    /**
     * The replication topology for the vBucket. Can be empty if not yet set,
     * otherwise encoded as a JSON array of chains, each chain is a array of
     * node names - e.g.
     *
     *     [ ["active", "replica_1"], ["active", "replica_1", "replica_2"]]
     *
     * First GA'd in 6.5
     */
    nlohmann::json replicationTopology;

    vbucket_state_t state = vbucket_state_dead;
};

/**
 * Describes the detailed state of a VBucket, including it's high-level 'state'
 * (active, replica, etc), and the various seqnos and other properties it has.
 *
 * This is persisted to disk during flush.
 *
 * Note that over time additional fields have been added to the vBucket state.
 * Given this state is writen to disk, and we support offline upgrade between
 * versions -  newer versions must support reading older versions' disk files
 * (to a limited version range) - when new fields are added the serialization &
 * deserialization methods need to handle fiels not being present.
 *
 * At time of writing the current GA major release is v6, which supports
 * offline upgrade from v5.0 or later. Any earlier releases do not support
 * direct offline upgrade (you'd have to first upgrade to v5.x). As such we
 * only need to support fields which were added in v5.0 or later; earlier fields
 * can be assumed to already exist on disk (v5.0 would have already handled the
 * upgrade).
 */
struct vbucket_state {
    /**
     * Current version of vbucket_state structure.
     * This value is supposed to increase every time we make a
     * change to the structure (ie, adding/removing members) or we make a change
     * in the usage/interpretation of any member.
     * History:
     * v1: Implicit, pre 5.5.4-MP, 6.0.2 and mad-hatter.
     * v2: 5.5.4-MP, 6.0.2 and Mad-Hatter (pre GA), added with MB-34173.
     *     Indicates snapshot start/end are sanitized with respect to
     *     high_seqno.
     * v3: Mad-Hatter. high_completed_seqno and high_prepared_seqno added along
     *     with counter for number of prepares on disk. Checkpoint-ID no longer
     *     stored (and ignored during upgrade)
     */
    static constexpr int CurrentVersion = 3;

    bool needsToBePersisted(const vbucket_state& vbstate);

    void reset();

    vbucket_state_t state = vbucket_state_dead;
    cb::uint48_t maxDeletedSeqno = 0;
    int64_t highSeqno = 0;
    uint64_t purgeSeqno = 0;

    /**
     * Start seqno of the last snapshot persisted.
     * First GA'd in v3.0
     */
    uint64_t lastSnapStart = 0;

    /**
     * End seqno of the last snapshot persisted.
     * First GA'd in v3.0
     */
    uint64_t lastSnapEnd = 0;

    /**
     * Maximum CAS value in this vBucket.
     * First GA'd in v4.0
     */
    uint64_t maxCas = 0;

    /**
     * The seqno at which CAS started to be encoded as a hybrid logical clock.
     * First GA'd in v5.0
     */
    int64_t hlcCasEpochSeqno = HlcCasSeqnoUninitialised;

    /**
     * True if this vBucket _might_ contain documents with eXtended Attributes.
     * first GA'd in v5.0
     */
    bool mightContainXattrs = false;

    /**
     * Does this vBucket file support namespaces (leb128 prefix on keys).
     * First GA'd in v6.5
     */
    bool supportsNamespaces = true;

    /**
     * Version of vbucket_state. See comments against CurrentVersion for
     * details.
     */
    int version = CurrentVersion;

    /**
     * Stores the seqno of the last completed (Committed or Aborted) Prepare.
     * Added for SyncReplication in 6.5.
     */
    uint64_t persistedCompletedSeqno = 0;

    /**
     * Stores the seqno of the last prepare (Pending SyncWrite). Added for
     * SyncReplication in 6.5.
     */
    uint64_t persistedPreparedSeqno = 0;

    /**
     * Number of on disk prepares (Pending SyncWrites). Required to correct the
     * vBucket level on disk document counts (for Full Eviction). Added for
     * SyncReplication in 6.5.
     */
    uint64_t onDiskPrepares = 0;

    /**
     * Data that is changed as part of a vbucket state transition is stored
     * in this member.
     */
    vbucket_transition_state transition;
};

/// Method to allow nlohmann::json to convert vbucket_state to JSON.
void to_json(nlohmann::json& json, const vbucket_state& vbs);

/// Method to allow nlohmann::json to convert from JSON to vbucket_state.
void from_json(const nlohmann::json& j, vbucket_state& vbs);

/// Method to allow nlohmann::json to convert vbucket_transition_state to JSON.
void to_json(nlohmann::json& json, const vbucket_transition_state& vbs);

/// Method to allow nlohmann::json to convert from JSON to
/// vbucket_transition_state.
void from_json(const nlohmann::json& j, vbucket_transition_state& vbs);

std::ostream& operator<<(std::ostream& os, const vbucket_state& vbs);

std::ostream& operator<<(std::ostream& os, const vbucket_transition_state& vbs);
