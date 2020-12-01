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

#include "bucket.h"
#include "cluster.h"
#include "dcp_replicator.h"
#include "node.h"

#include <platform/uuid.h>
#include <protocol/connection/client_connection.h>
#include <protocol/connection/client_mcbp_commands.h>
#include <utility>

namespace cb::test {

Bucket::Bucket(const Cluster& cluster,
               std::string nm,
               size_t vbuckets,
               size_t replicas,
               DcpPacketFilter packet_filter)
    : cluster(cluster),
      name(std::move(nm)),
      uuid(::to_string(cb::uuid::random())),
      packet_filter(std::move(packet_filter)) {
    auto nodes = cluster.size();
    vbucketmap.resize(vbuckets);
    int ii = 0;
    for (size_t vb = 0; vb < vbuckets; ++vb) {
        vbucketmap[vb].resize(replicas + 1);
        for (size_t n = 0; n < (replicas + 1); ++n) {
            vbucketmap[vb][n] = ii++ % nodes;
        }
    }

    // Let's build up the bucket clustermap
    // Start by setting the default values
    manifest = {
            {"rev", 1},
            {"name", name},
            {"uuid", uuid},
            {"uri", "/pools/default/buckets/" + name + "?bucket_uuid=" + uuid},
            {"streamingUri",
             "/pools/default/bucketsStreaming/" + name +
                     "?bucket_uuid=" + uuid},
            {"nodeLocator", "vbucket"},
            {"bucketCapabilitiesVer", ""},
            {"bucketCapabilities",
             {"collections",
              "durableWrite",
              "tombstonedUserXAttrs",
              "couchapi",
              "dcp",
              "cbhello",
              "touch",
              "cccp",
              "nodesExt",
              "xattr"}},
            {"collectionsManifestUid", "0"},
            {"ddocs", {"uri", "/pools/default/buckets/" + name + "/ddocs"}},
            {"clusterCapabilitiesVer", {1, 0}},
            {"clusterCapabilities", nlohmann::json::object()},
            {"vBucketServerMap",
             {{"numReplicas", vbucketmap[0].size() - 1},
              {"hashAlgorithm", "CRC"},
              {"vBucketMap", vbucketmap}}}

    };

    auto [ipv4, ipv6] = cb::net::getIpAddresses(true);
    (void)ipv6; // currently not used
    const auto& hostname = ipv4.front();
    cluster.iterateNodes([this, &hostname](const cb::test::Node& node) {
        node.getConnectionMap().iterate([this, &hostname](
                                                const MemcachedConnection& c) {
            if (c.getFamily() == AF_INET) {
                manifest["vBucketServerMap"]["serverList"].emplace_back(
                        hostname + ":" + std::to_string(c.getPort()));

                nlohmann::json json = {{"couchApiBase",
                                        "http://" + hostname + ":6666/" +
                                                Bucket::name + "%2B" + uuid},
                                       {"hostname", hostname + ":6666"},
                                       {"ports", {{"direct", c.getPort()}}}};
                manifest["nodes"].emplace_back(std::move(json));

                json = {{"services",
                         {{"mgmt", 6666}, {"capi", 6666}, {"kv", c.getPort()}}},
                        {"hostname", hostname}};
                manifest["nodesExt"].emplace_back(std::move(json));
            }
        });
    });
}

Bucket::~Bucket() = default;

void Bucket::setupReplication() {
    setupReplication({});
}

void Bucket::setupReplication(const std::vector<ReplicationConfig>& specifics) {
    replicators =
            DcpReplicator::create(cluster, *this, packet_filter, specifics);
}

void Bucket::shutdownReplication() {
    replicators.reset();
}

std::unique_ptr<MemcachedConnection> Bucket::getConnection(
        Vbid vbucket, vbucket_state_t state, size_t replica_number) {
    if (vbucket.get() > vbucketmap.size()) {
        throw std::invalid_argument("Bucket::getConnection: Invalid vbucket");
    }

    if (state == vbucket_state_active) {
        return cluster.getConnection(vbucketmap[vbucket.get()][0]);
    }

    if (state != vbucket_state_replica) {
        throw std::invalid_argument(
                "Bucket::getConnection: Unsupported vbucket state");
    }

    if ((replica_number + 1) > vbucketmap[0].size()) {
        throw std::invalid_argument(
                "Bucket::getConnection: Invalid replica number");
    }

    return cluster.getConnection(vbucketmap[vbucket.get()][replica_number + 1]);
}

void Bucket::setCollectionManifest(nlohmann::json next) {
    const auto payload = next.dump(2);
    for (size_t idx = 0; idx < cluster.size(); ++idx) {
        auto conn = cluster.getConnection(idx);
        conn->authenticate("@admin", "password", "PLAIN");
        conn->selectBucket(name);
        auto rsp = conn->execute(BinprotGenericCommand{
                cb::mcbp::ClientOpcode::CollectionsSetManifest, {}, payload});
        if (!rsp.isSuccess()) {
            throw ConnectionError(
                    "Bucket::setCollectionManifest: Failed to set Collection "
                    "manifest on n_" +
                            std::to_string(idx),
                    rsp);
        }
    }

    collectionManifest = std::move(next);
}

} // namespace cb::test