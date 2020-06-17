/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2018 Couchbase, Inc.
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

#include <folly/portability/GTest.h>
#include <mcbp/protocol/unsigned_leb128.h>

#include <limits>
#include <random>

template <class T>
class UnsignedLeb128 : public ::testing::Test {};

using MyTypes = ::testing::Types<uint8_t, uint16_t, uint32_t, uint64_t>;
TYPED_TEST_SUITE(UnsignedLeb128, MyTypes);

TEST(UnsignedLeb128, MaxSize) {
    EXPECT_EQ(2, cb::mcbp::unsigned_leb128<uint8_t>::getMaxSize());
    EXPECT_EQ(3, cb::mcbp::unsigned_leb128<uint16_t>::getMaxSize());
    EXPECT_EQ(5, cb::mcbp::unsigned_leb128<uint32_t>::getMaxSize());
    EXPECT_EQ(10, cb::mcbp::unsigned_leb128<uint64_t>::getMaxSize());
}

TYPED_TEST(UnsignedLeb128, EncodeDecode0) {
    cb::mcbp::unsigned_leb128<TypeParam> zero(0);
    EXPECT_EQ(1, zero.get().size());
    EXPECT_EQ(0, zero.get().data()[0]);
    auto rv = cb::mcbp::decode_unsigned_leb128<TypeParam>(zero.get());
    EXPECT_EQ(0, rv.first);
    EXPECT_EQ(0, rv.second.size()); // All input consumed
    EXPECT_EQ(0, *cb::mcbp::unsigned_leb128_get_stop_byte_index(zero.get()));
}

TYPED_TEST(UnsignedLeb128, EncodeDecodeMax) {
    cb::mcbp::unsigned_leb128<TypeParam> max(
            std::numeric_limits<TypeParam>::max());
    auto rv = cb::mcbp::decode_unsigned_leb128<TypeParam>(max.get());
    EXPECT_EQ(std::numeric_limits<TypeParam>::max(), rv.first);
    EXPECT_EQ(0, rv.second.size());
}

// Input has the MSbit set for every byte
TYPED_TEST(UnsignedLeb128, EncodeDecode0x80) {
    TypeParam value = 0;
    for (size_t i = 0; i < sizeof(TypeParam); i++) {
        value |= 0x80ull << (i * 8);
    }
    cb::mcbp::unsigned_leb128<TypeParam> leb(value);
    auto rv = cb::mcbp::decode_unsigned_leb128<TypeParam>(leb.get());
    EXPECT_EQ(value, rv.first);
    EXPECT_EQ(0, rv.second.size());
    EXPECT_EQ(leb.get().size() - 1,
              *cb::mcbp::unsigned_leb128_get_stop_byte_index(leb.get()));
}

TYPED_TEST(UnsignedLeb128, EncodeDecodeRandomValue) {
    std::mt19937_64 twister(sizeof(TypeParam));
    auto value = gsl::narrow_cast<TypeParam>(twister());
    cb::mcbp::unsigned_leb128<TypeParam> leb(value);
    auto rv = cb::mcbp::decode_unsigned_leb128<TypeParam>(leb.get());
    EXPECT_EQ(value, rv.first);
    EXPECT_EQ(0, rv.second.size());
    EXPECT_EQ(leb.get().size() - 1,
              *cb::mcbp::unsigned_leb128_get_stop_byte_index(leb.get()));
}

TYPED_TEST(UnsignedLeb128, EncodeDecodeValues) {
    std::vector<uint64_t> values = {1,
                                    10,
                                    100,
                                    255,
                                    256,
                                    1000,
                                    10000,
                                    65535,
                                    65536,
                                    100000,
                                    1000000,
                                    100000000,
                                    4294967295,
                                    4294967296,
                                    1000000000000};

    for (auto v : values) {
        if (v <= std::numeric_limits<TypeParam>::max()) {
            cb::mcbp::unsigned_leb128<TypeParam> leb(
                    gsl::narrow_cast<TypeParam>(v));
            auto rv = cb::mcbp::decode_unsigned_leb128<TypeParam>(leb.get());
            EXPECT_EQ(v, rv.first);
            EXPECT_EQ(0, rv.second.size());
            EXPECT_EQ(
                    leb.get().size() - 1,
                    *cb::mcbp::unsigned_leb128_get_stop_byte_index(leb.get()));
        }
    }
}

TYPED_TEST(UnsignedLeb128, EncodeDecodeMultipleValues) {
    std::mt19937_64 twister(sizeof(TypeParam));
    std::vector<uint8_t> data;
    std::vector<TypeParam> values;
    const int iterations = 10;

    // Encode
    for (int n = 0; n < iterations; n++) {
        values.push_back(gsl::narrow_cast<TypeParam>(twister()));
        cb::mcbp::unsigned_leb128<TypeParam> leb(values.back());
        for (auto c : leb.get()) {
            data.push_back(c);
        }
    }

    std::pair<TypeParam, cb::const_byte_buffer> decoded = {0, {data}};
    int index = 0;

    // Decode
    do {
        decoded = cb::mcbp::decode_unsigned_leb128<TypeParam>(decoded.second);
        EXPECT_EQ(values[index], decoded.first);
        index++;
    } while (!decoded.second.empty());
    EXPECT_EQ(iterations, index);
}

TYPED_TEST(UnsignedLeb128, DecodeInvalidInput) {
    // Encode a value and then break the value by removing the stop-byte
    std::mt19937_64 twister(sizeof(TypeParam));
    auto value = gsl::narrow_cast<TypeParam>(twister());
    cb::mcbp::unsigned_leb128<TypeParam> leb(value);

    // Take a copy of the const encoded value for modification
    std::vector<uint8_t> data;
    for (auto c : leb.get()) {
        data.push_back(c);
    }

    // Set the MSbit of the MSB so it's no longer a stop-byte
    data.back() |= 0x80ull;

    EXPECT_FALSE(cb::mcbp::unsigned_leb128_get_stop_byte_index({data}));
    try {
        cb::mcbp::decode_unsigned_leb128<TypeParam>({data});
        FAIL() << "Decode didn't throw";
    } catch (const std::invalid_argument&) {
    }
}

// Encode a value and expect the iterators to iterate the encoded bytes
TYPED_TEST(UnsignedLeb128, iterators) {
    TypeParam value = 1; // Upto 127 and it's 1 byte
    cb::mcbp::unsigned_leb128<TypeParam> leb(value);
    int loopCounter = 0;
    for (const auto c : leb) {
        (void)c;
        loopCounter++;
    }
    EXPECT_EQ(1, loopCounter);
    loopCounter = 0;

    for (auto itr = leb.begin(); itr != leb.end(); itr++) {
        loopCounter++;
    }
    EXPECT_EQ(1, loopCounter);
}

// Set some expectations around the get/data/size API
TYPED_TEST(UnsignedLeb128, basic_api_checks) {
    auto value = gsl::narrow_cast<TypeParam>(5555);
    cb::mcbp::unsigned_leb128<TypeParam> leb(value);
    EXPECT_EQ(leb.get().size(), leb.size());
    EXPECT_EQ(leb.get().data(), leb.data());
}

// Test a few non-canonical encodings decode as expected.
TYPED_TEST(UnsignedLeb128, non_canonical) {
    std::vector<std::pair<TypeParam, std::vector<std::vector<uint8_t>>>>
            testData = {
                    {0, {{0}, {0x80, 0}, {0x80, 0x80, 0}}},
                    {1, {{1}, {0x81, 0}, {0x81, 0x80, 0}}},
            };

    for (const auto& test : testData) {
        for (const auto& data : test.second) {
            // Ignore test inputs which are invalid for TypeParam (too long)
            if (data.size() <=
                cb::mcbp::unsigned_leb128<TypeParam>::getMaxSize()) {
                auto value =
                        cb::mcbp::decode_unsigned_leb128<TypeParam>({data});
                EXPECT_EQ(test.first, value.first);
            }
        }
    }
}

TYPED_TEST(UnsignedLeb128, long_input) {
    std::vector<uint8_t> data;
    // Generate data that used to decode ok, but was invalid. leb128 decoder
    // detects input which is too long and fails
    for (size_t ii = 0; ii < cb::mcbp::unsigned_leb128<TypeParam>::getMaxSize();
         ii++) {
        data.push_back(0x81);
    }
    data.push_back(0x01);

    try {
        cb::mcbp::decode_unsigned_leb128<TypeParam>({data});
        FAIL() << "Decode didn't throw";
    } catch (const std::invalid_argument&) {
    }

    auto rv = cb::mcbp::decode_unsigned_leb128<TypeParam>(
            {data}, cb::mcbp::Leb128NoThrow{});
    EXPECT_EQ(nullptr, rv.second.data());
    EXPECT_EQ(0, rv.second.size());
    EXPECT_EQ(0, rv.first);
}

TEST(UnsignedLeb128, collection_ID_encode) {
    struct TestData {
        uint32_t value;
        std::vector<uint8_t> encoded;
    };
    // These values we will add to protocol documentation so that clients can
    // test their leb128 encoders
    std::vector<TestData> tests = {
            {0x00, {0x00}},
            {0x01, {0x01}},
            {0x7F, {0x7F}},
            {0x80, {0x80, 0x01}},
            {0x555, {0xD5, 0x0A}},
            {0x7FFF, {0xFF, 0xFF, 0x01}},
            {0xBFFF, {0xFF, 0xFF, 0x02}},
            {0xFFFF, {0XFF, 0xFF, 0x03}},
            {0x8000, {0x80, 0x80, 0x02}},
            {0x5555, {0xD5, 0xAA, 0x01}},
            {0xcafef00, {0x80, 0xDE, 0xBF, 0x65}},
            {0xcafef00d, {0x8D, 0xE0, 0xFB, 0xD7, 0x0C}},
            {0xffffffff, {0xFF, 0xFF, 0xFF, 0xFF, 0x0F}}};

    for (size_t index = 0; index < tests.size(); index++) {
        const auto& test = tests[index];
        // Encode the value
        cb::mcbp::unsigned_leb128<uint32_t> encoded(test.value);
        ASSERT_EQ(test.encoded.size(), encoded.size())
                << "size failure for test:" << index;
        EXPECT_EQ(test.value,
                  cb::mcbp::decode_unsigned_leb128<uint32_t>(encoded.get())
                          .first);

        int offset = 0;
        for (const auto byte : encoded) {
            // cast away from uint8_t so we get more readable failures
            EXPECT_EQ(uint32_t(test.encoded[offset]), uint32_t(byte))
                    << "Mismatch byte-offset:" << offset
                    << ", test.value:" << test.value << ", test:" << index;
            offset++;
        }
    }
}
