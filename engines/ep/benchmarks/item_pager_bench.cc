/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2021 Couchbase, Inc
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

#include "engine_fixture.h"

#include <fakes/fake_executorpool.h>
#include <kv_bucket.h>
#include <paging_visitor.h>

#include <benchmark/benchmark.h>
#include <folly/portability/GTest.h>

#include <random>

/**
 * Fixture for item pager benchmarks
 */
class ItemPagerBench : public EngineFixture {
protected:
    void SetUp(const benchmark::State& state) override {
        varConfig = "backend=couchdb;max_vbuckets=1024";
        EngineFixture::SetUp(state);
        if (state.thread_index == 0) {
            const auto vbCount = state.range(0);
            vbids.reserve(vbCount);

            for (int i = 0; i < vbCount; i++) {
                vbids.emplace_back(i);
            }

            auto* store = engine->getKVBucket();

            // set all vbs to active initially
            for (auto vbid : vbids) {
                ASSERT_EQ(cb::engine_errc::success,
                          store->setVBucketState(vbid, vbucket_state_active))
                        << "Couldn't create vbid:" << vbid.get();
            }

            // populate
            pseudoRandomPopulate(vbids);

            // flip 1/2 to replica now they have been populated
            for (auto vbid : vbids) {
                if (vbid.get() % 2) {
                    ASSERT_EQ(
                            cb::engine_errc::success,
                            store->setVBucketState(vbid, vbucket_state_active))
                            << "Couldn't set to replica vbid:" << vbid.get();
                }
            }
        }
    }

    void TearDown(const benchmark::State& state) override {
        if (state.thread_index == 0) {
            const auto vbCount = state.range(0);
            for (int i = 0; i < vbCount; i++) {
                ASSERT_EQ(
                        cb::engine_errc::success,
                        engine->getKVBucket()->deleteVBucket(Vbid(i), nullptr))
                        << "Couldn't delete vbid:" << i;
                executorPool->runNextTask(
                        AUXIO_TASK_IDX,
                        "Removing (dead) vb:" + std::to_string(i) +
                                " from memory and disk");
            }
        }
        EngineFixture::TearDown(state);
    }

    void pseudoRandomPopulate(const std::vector<Vbid>& vbs,
                              size_t maxItemCount = 100) {
        // initialize engine with default (fixed) seed
        std::mt19937 mt;

        // get a uniform distribution over the possibe range of items to add to
        // each vbucket
        std::uniform_int_distribution dist(size_t(0), maxItemCount);

        std::string value = "foobarvalue";
        for (const auto& vbid : vbs) {
            // get a (pseudo)random number of items within [0, maxItemCount]
            auto itemCount = dist(mt);

            // store that many items into this vb
            for (size_t i = 0; i < itemCount; ++i) {
                auto item = make_item(
                        vbid, std::string("key") + std::to_string(i), value);
                ASSERT_EQ(cb::engine_errc::success,
                          engine->getKVBucket()->set(item, cookie));
            }
        }
    }

protected:
    std::vector<Vbid> vbids;
};

BENCHMARK_DEFINE_F(ItemPagerBench, VBCBAdaptorCreation)
(benchmark::State& state) {
    // Benchmark - measure how long it takes to create a VBCBAdaptor for a
    // PagingVisitor. This involves visiting each vb and checking the memory
    // usage and state.

    std::shared_ptr<std::atomic<bool>> available;
    Configuration& cfg = engine->getConfiguration();

    while (state.KeepRunning()) {
        state.PauseTiming();
        std::unique_ptr<PagingVisitor> pv = std::make_unique<PagingVisitor>(
                *engine->getKVBucket(),
                engine->getEpStats(),
                EvictionRatios{
                        1 /* active&pending */,
                        1 /* replica */}, // evict everything (but this will
                // not be run)
                available,
                EXPIRY_PAGER,
                false,
                VBucketFilter(vbids),
                cfg.getItemEvictionAgePercentage(),
                cfg.getItemEvictionFreqCounterAgeThreshold());
        state.ResumeTiming();

        auto task = std::make_shared<VBCBAdaptor>(engine->getKVBucket(),
                                                  TaskId::ItemPagerVisitor,
                                                  std::move(pv),
                                                  "paging visitor adaptor",
                                                  /*shutdown*/ false);
    }
}

BENCHMARK_REGISTER_F(ItemPagerBench, VBCBAdaptorCreation)->Range(1, 1024);
