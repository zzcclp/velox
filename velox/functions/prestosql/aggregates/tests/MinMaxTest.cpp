/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "velox/common/base/tests/GTestUtils.h"
#include "velox/functions/lib/aggregates/tests/utils/AggregationTestBase.h"
#include "velox/functions/prestosql/types/TimestampWithTimeZoneType.h"
#include "velox/vector/fuzzer/VectorFuzzer.h"

using namespace facebook::velox;
using namespace facebook::velox::exec::test;

namespace {

std::string min(const std::string& column) {
  return fmt::format("min({})", column);
}

std::string max(const std::string& column) {
  return fmt::format("max({})", column);
}

class MinMaxTest : public functions::aggregate::test::AggregationTestBase {
 protected:
  void SetUp() override {
    AggregationTestBase::SetUp();
  }

  std::vector<RowVectorPtr> fuzzData(const RowTypePtr& rowType) {
    VectorFuzzer::Options options;
    options.vectorSize = 1'000;
    options.nullRatio = 0.1;
    VectorFuzzer fuzzer(options, pool());
    std::vector<RowVectorPtr> vectors(10);
    for (auto i = 0; i < 10; ++i) {
      vectors[i] = fuzzer.fuzzInputRow(rowType);
    }
    return vectors;
  }

  template <typename TAgg>
  void
  doTest(TAgg agg, const TypePtr& inputType, bool testWithTableScan = true) {
    auto rowType = ROW({"c0", "c1", "mask"}, {BIGINT(), inputType, BOOLEAN()});
    auto vectors = fuzzData(rowType);
    createDuckDbTable(vectors);

    static const std::string c0 = "c0";
    static const std::string c1 = "c1";
    static const std::string a0 = "a0";

    // Global aggregation.
    testAggregations(
        vectors, {}, {agg(c1)}, fmt::format("SELECT {} FROM tmp", agg(c1)));

    // Group by aggregation.
    testAggregations(
        [&](auto& builder) {
          builder.values(vectors).project({"c0 % 10", "c1"});
        },
        {"p0"},
        {agg(c1)},
        fmt::format("SELECT c0 % 10, {} FROM tmp GROUP BY 1", agg(c1)));

    // Masked aggregations.
    auto maskedAgg = agg(c1) + " filter (where mask)";
    testAggregations(
        vectors, {}, {maskedAgg}, fmt::format("SELECT {} FROM tmp", maskedAgg));

    testAggregations(
        [&](auto& builder) {
          builder.values(vectors).project({"c0 % 10", "c1", "mask"});
        },
        {"p0"},
        {maskedAgg},
        fmt::format("SELECT c0 % 10, {} FROM tmp GROUP BY 1", maskedAgg));

    // Encodings: use filter to wrap aggregation inputs in a dictionary.
    testAggregations(
        [&](auto& builder) {
          builder.values(vectors)
              .filter("c0 % 2 = 0")
              .project({"c0 % 11", "c1"});
        },
        {"p0"},
        {agg(c1)},
        fmt::format(
            "SELECT c0 % 11, {} FROM tmp WHERE c0 % 2 = 0 GROUP BY 1",
            agg(c1)));

    testAggregations(
        [&](auto& builder) { builder.values(vectors).filter("c0 % 2 = 0"); },
        {},
        {agg(c1)},
        fmt::format("SELECT {} FROM tmp WHERE c0 % 2 = 0", agg(c1)));
  }

  template <typename T>
  void testExtremeFloatValues() {
    // Tests to ensure that extreme floating point values are handled correctly,
    // including, INF, -INF, NaN. This validates that the groups have initial
    // value set correctly, (-INF for max() and NaN for min()) and NaN is
    // considered greater than INF. Also tests for when floating points are
    // nested inside complex types. Finally, this also tests for when
    // aggregation is pushed down to the scan operator which can only happen if
    // the column is a primitive type and not used anywhere execpt a single
    // aggregate.
    static const T kNaN = std::numeric_limits<T>::quiet_NaN();
    static const T kSNaN = std::numeric_limits<T>::signaling_NaN();
    static const T kInf = std::numeric_limits<T>::infinity();

    auto data = makeRowVector({
        // regular ordering
        makeFlatVector<T>({2.0, kNaN, 1.1, kInf, -1.1}),
        // with nulls
        makeNullableFlatVector<T>({2.0, kNaN, std::nullopt, 1.1, -1.1}),
        // only nans (use a different binary representation for NaN to verify
        // that they are considered equal)
        makeFlatVector<T>({kSNaN, kSNaN, kSNaN, kSNaN, kSNaN}),
        // only Inf
        makeFlatVector<T>({kInf, kInf, kInf, kInf, kInf}),
        // only -Inf
        makeFlatVector<T>({-kInf, -kInf, -kInf, -kInf, -kInf}),
        // group by column
        makeFlatVector<int32_t>({1, 1, 1, 2, 2}),
    });

    // Global aggregation.
    {
      // Verify max pushed down to scan operator.
      std::vector<VectorPtr> expectedMaxValues = {
          makeFlatVector<T>(std::vector<T>({kNaN})),
          makeFlatVector<T>(std::vector<T>({kNaN})),
          makeFlatVector<T>(std::vector<T>({kNaN})),
          makeFlatVector<T>(std::vector<T>({kInf})),
          makeFlatVector<T>(std::vector<T>({-kInf}))};

      testAggregations(
          {data},
          {},
          {"max(c0)", "max(c1)", "max(c2)", "max(c3)", "max(c4)"},
          {makeRowVector(expectedMaxValues)});

      // Verify max pushed down to scan operator.
      std::vector<VectorPtr> expectedMinValues = {
          makeFlatVector<T>(std::vector<T>({-1.1})),
          makeFlatVector<T>(std::vector<T>({-1.1})),
          makeFlatVector<T>(std::vector<T>({kNaN})),
          makeFlatVector<T>(std::vector<T>({kInf})),
          makeFlatVector<T>(std::vector<T>({-kInf})),
      };
      testAggregations(
          {data},
          {},
          {"min(c0)", "min(c1)", "min(c2)", "min(c3)", "min(c4)"},
          {makeRowVector(expectedMinValues)});

      // Verify max and min evaluated in aggregation operator.
      std::vector<VectorPtr> allExpectedValues = expectedMaxValues;
      allExpectedValues.insert(
          allExpectedValues.end(),
          expectedMinValues.begin(),
          expectedMinValues.end());

      testAggregations(
          {data},
          {},
          {"max(c0)",
           "max(c1)",
           "max(c2)",
           "max(c3)",
           "max(c4)",
           "min(c0)",
           "min(c1)",
           "min(c2)",
           "min(c3)",
           "min(c4)"},
          {makeRowVector(allExpectedValues)});
    }

    // group-by aggregation.
    {
      // Verify max pushed down to scan operator.
      std::vector<VectorPtr> expectedMaxValues = {
          makeFlatVector<int32_t>({1, 2}), // grouping key
          makeFlatVector<T>({kNaN, kInf}),
          makeFlatVector<T>({kNaN, 1.1}),
          makeFlatVector<T>({kNaN, kNaN}),
          makeFlatVector<T>({kInf, kInf}),
          makeFlatVector<T>({-kInf, -kInf})};

      testAggregations(
          {data},
          {"c5"},
          {"max(c0)", "max(c1)", "max(c2)", "max(c3)", "max(c4)"},
          {makeRowVector(expectedMaxValues)});

      // Verify min pushed down to scan operator.
      std::vector<VectorPtr> expectedMinValues = {
          makeFlatVector<int32_t>({1, 2}), // grouping key
          makeFlatVector<T>({1.1, -1.1}),
          makeFlatVector<T>({2.0, -1.1}),
          makeFlatVector<T>({kNaN, kNaN}),
          makeFlatVector<T>({kInf, kInf}),
          makeFlatVector<T>({-kInf, -kInf})};

      testAggregations(
          {data},
          {"c5"},
          {"min(c0)", "min(c1)", "min(c2)", "min(c3)", "min(c4)"},
          {makeRowVector(expectedMinValues)});

      // Verify max and min evaluated in aggregation operator.
      std::vector<VectorPtr> allExpectedValues = expectedMaxValues;
      allExpectedValues.insert(
          allExpectedValues.end(),
          expectedMinValues.begin() + 1, // skip the grouping key column
          expectedMinValues.end());

      testAggregations(
          {data},
          {"c5"},
          {"max(c0)",
           "max(c1)",
           "max(c2)",
           "max(c3)",
           "max(c4)",
           "min(c0)",
           "min(c1)",
           "min(c2)",
           "min(c3)",
           "min(c4)"},
          {makeRowVector(allExpectedValues)});
    }

    // Test for float point values nested inside complex type.
    data = makeRowVector({
        makeRowVector({
            makeFlatVector<T>({2, kNaN, 1, kInf, -1, kNaN}),
            makeFlatVector<int32_t>({1, 1, 1, 2, 2, 2}),
        }),
        makeFlatVector<int32_t>({1, 1, 1, 2, 2, 2}),
    });

    // Global aggregation.
    {
      auto expected = makeRowVector(
          {makeRowVector({
               makeFlatVector<T>(std::vector<T>({-1})),
               makeFlatVector<int32_t>(std::vector<int32_t>({2})),
           }),
           makeRowVector({
               makeFlatVector<T>(std::vector<T>({kNaN})),
               makeFlatVector<int32_t>(std::vector<int32_t>({2})),
           })});

      testAggregations({data}, {}, {"min(c0)", "max(c0)"}, {expected});
    }

    // group-by aggregation.
    {
      auto expected = makeRowVector(
          {makeFlatVector<int32_t>({1, 2}),
           makeRowVector({
               makeFlatVector<T>(std::vector<T>({1, -1})),
               makeFlatVector<int32_t>(std::vector<int32_t>({1, 2})),
           }),
           makeRowVector({
               makeFlatVector<T>(std::vector<T>({kNaN, kNaN})),
               makeFlatVector<int32_t>(std::vector<int32_t>({1, 2})),
           })});

      testAggregations({data}, {"c1"}, {"min(c0)", "max(c0)"}, {expected});
    }
  }
};

TEST_F(MinMaxTest, maxTinyint) {
  doTest(max, TINYINT());
}

TEST_F(MinMaxTest, maxSmallint) {
  doTest(max, SMALLINT());
}

TEST_F(MinMaxTest, maxInteger) {
  doTest(max, INTEGER());
}

TEST_F(MinMaxTest, maxBigint) {
  doTest(max, BIGINT());
}

TEST_F(MinMaxTest, maxReal) {
  doTest(max, REAL());
  testExtremeFloatValues<float>();
}

TEST_F(MinMaxTest, maxDouble) {
  doTest(max, DOUBLE());
  testExtremeFloatValues<double>();
}

TEST_F(MinMaxTest, maxVarchar) {
  doTest(max, VARCHAR());
}

TEST_F(MinMaxTest, maxBoolean) {
  doTest(max, BOOLEAN());
}

TEST_F(MinMaxTest, maxInterval) {
  doTest(max, INTERVAL_DAY_TIME());
}

TEST_F(MinMaxTest, minTinyint) {
  doTest(min, TINYINT());
}

TEST_F(MinMaxTest, minSmallint) {
  doTest(min, SMALLINT());
}

TEST_F(MinMaxTest, minInteger) {
  doTest(min, INTEGER());
}

TEST_F(MinMaxTest, minBigint) {
  doTest(min, BIGINT());
}

TEST_F(MinMaxTest, minReal) {
  doTest(min, REAL());
}

TEST_F(MinMaxTest, minDouble) {
  doTest(min, DOUBLE());
}

TEST_F(MinMaxTest, minInterval) {
  doTest(min, INTERVAL_DAY_TIME());
}

TEST_F(MinMaxTest, minVarchar) {
  doTest(min, VARCHAR());
}

TEST_F(MinMaxTest, minBoolean) {
  doTest(min, BOOLEAN());
}

TEST_F(MinMaxTest, constVarchar) {
  // Create two batches of the source data for the aggregation:
  // Column c0 with 1K of "apple" and 1K of "banana".
  // Column c1 with 1K of nulls and 1K of nulls.
  auto constVectors = {
      makeRowVector(
          {makeConstant("apple", 1'000),
           makeNullConstant(TypeKind::VARCHAR, 1'000)}),
      makeRowVector({
          makeConstant("banana", 1'000),
          makeNullConstant(TypeKind::VARCHAR, 1'000),
      })};

  testAggregations(
      {constVectors},
      {},
      {"min(c0)", "max(c0)", "min(c1)", "max(c1)"},
      "SELECT 'apple', 'banana', null, null");
}

TEST_F(MinMaxTest, minMaxTimestamp) {
  auto rowType = ROW({"c0", "c1"}, {SMALLINT(), TIMESTAMP()});
  auto vectors = makeVectors(rowType, 1'000, 10);
  createDuckDbTable(vectors);

  testAggregations(
      vectors,
      {},
      {"min(c1)", "max(c1)"},
      "SELECT date_trunc('millisecond', min(c1)), "
      "date_trunc('millisecond', max(c1)) FROM tmp");

  testAggregations(
      [&](auto& builder) {
        builder.values(vectors).project({"c0 % 17 as k", "c1"});
      },
      {"k"},
      {"min(c1)", "max(c1)"},
      "SELECT c0 % 17, date_trunc('millisecond', min(c1)), "
      "date_trunc('millisecond', max(c1)) FROM tmp GROUP BY 1");
}

TEST_F(MinMaxTest, largeValuesDate) {
  auto vectors = {makeRowVector(
      {makeConstant(60577, 100, DATE()), makeConstant(-57604, 100, DATE())})};
  createDuckDbTable(vectors);

  testAggregations(
      vectors,
      {},
      {"min(c0)", "max(c0)", "min(c1)", "max(c1)"},
      "SELECT min(c0), max(c0), min(c1), max(c1) FROM tmp");
}

TEST_F(MinMaxTest, minMaxDate) {
  auto rowType = ROW({"c0", "c1"}, {SMALLINT(), DATE()});
  auto vectors = makeVectors(rowType, 1'000, 10);
  createDuckDbTable(vectors);

  testAggregations(
      vectors, {}, {"min(c1)", "max(c1)"}, "SELECT min(c1), max(c1) FROM tmp");

  testAggregations(
      [&](auto& builder) {
        builder.values(vectors).project({"c0 % 17 as k", "c1"});
      },
      {"k"},
      {"min(c1)", "max(c1)"},
      "SELECT c0 % 17, min(c1), max(c1) FROM tmp GROUP BY 1");
}

TEST_F(MinMaxTest, minMaxUnknown) {
  auto data = makeRowVector({
      makeFlatVector<int64_t>({1, 2, 1, 2, 1, 2}),
      makeAllNullFlatVector<UnknownValue>(6),
  });

  auto expected = makeRowVector({
      makeAllNullFlatVector<UnknownValue>(1),
      makeAllNullFlatVector<UnknownValue>(1),
  });

  testAggregations({data}, {}, {"min(c1)", "max(c1)"}, {expected});

  expected = makeRowVector({
      makeFlatVector<int64_t>({1, 2}),
      makeAllNullFlatVector<UnknownValue>(2),
      makeAllNullFlatVector<UnknownValue>(2),
  });

  testAggregations({data}, {"c0"}, {"min(c1)", "max(c1)"}, {expected});
}

TEST_F(MinMaxTest, initialValue) {
  // Ensures that no groups are default initialized (to 0) in
  // aggregate::SimpleNumericAggregate.
  auto row = makeRowVector({
      makeFlatVector<int8_t>({1, 1, 1, 1}),
      makeFlatVector<int8_t>({-1, -1, -1, -1}),
      makeFlatVector<double>({1, 2, 3, 4}),
      makeFlatVector<double>({-1, -2, -3, -4}),
  });
  testAggregations(
      {row},
      {},
      {"min(c0)", "max(c1)", "min(c2)", "max(c3)"},
      "SELECT 1, -1, 1, -1");
}

TEST_F(MinMaxTest, maxShortDecimal) {
  doTest(max, DECIMAL(18, 3), false);
}

TEST_F(MinMaxTest, minShortDecimal) {
  doTest(min, DECIMAL(3, 1), false);
}

TEST_F(MinMaxTest, maxLongDecimal) {
  doTest(max, DECIMAL(20, 3), false);
}

TEST_F(MinMaxTest, minLongDecimal) {
  doTest(min, DECIMAL(38, 19), false);
}

TEST_F(MinMaxTest, array) {
  auto data = makeRowVector({
      makeNullableArrayVector<int64_t>({
          {1, 2, 3},
          {2, std::nullopt},
          {6, 7, 8},
      }),
  });

  auto expected = makeRowVector({
      makeArrayVector<int64_t>({
          {1, 2, 3},
      }),
      makeArrayVector<int64_t>({
          {6, 7, 8},
      }),
  });

  VELOX_ASSERT_THROW(
      testAggregations({data}, {}, {"min(c0)", "max(c0)"}, {expected}),
      "ARRAY comparison not supported for values that contain nulls");

  data = makeRowVector({
      makeNullableArrayVector<int64_t>({
          {1, 2, 3},
          {3, 2},
          {6, 7, 8},
      }),
  });
  testAggregations({data}, {}, {"min(c0)", "max(c0)"}, {expected});
}

TEST_F(MinMaxTest, row) {
  auto data = makeRowVector({
      makeRowVector({
          makeFlatVector<StringView>({
              "a"_sv,
              "b"_sv,
              "c"_sv,
          }),
          makeNullableFlatVector<StringView>({
              std::nullopt,
              "efg"_sv,
              "hij"_sv,
          }),
      }),
  });

  auto expected = makeRowVector({
      makeRowVector(
          {makeFlatVector<StringView>({"a"_sv}),
           makeFlatVector<StringView>({"abc"_sv})}),
      makeRowVector(
          {makeFlatVector<StringView>({"c"_sv}),
           makeFlatVector<StringView>({"hij"_sv})}),
  });

  VELOX_ASSERT_THROW(
      testAggregations({data}, {}, {"min(c0)", "max(c0)"}, {expected}),
      "ROW comparison not supported for values that contain nulls");

  data = makeRowVector({
      makeRowVector({
          makeFlatVector<StringView>({
              "a"_sv,
              "b"_sv,
              "c"_sv,
          }),
          makeNullableFlatVector<StringView>({
              "abc"_sv,
              "efg"_sv,
              "hij"_sv,
          }),
      }),
  });
  testAggregations({data}, {}, {"min(c0)", "max(c0)"}, {expected});
}

TEST_F(MinMaxTest, arrayCheckNulls) {
  auto batch = makeRowVector({
      makeArrayVectorFromJson<int32_t>({
          "[1, 2]",
          "[6, 7]",
          "[2, 3]",
      }),
      makeFlatVector<int32_t>({
          1,
          2,
          3,
      }),
  });

  auto batchWithNull = makeRowVector({
      makeArrayVectorFromJson<int32_t>({
          "[1, 2]",
          "[6, 7]",
          "[3, null]",
      }),
      makeFlatVector<int32_t>({
          1,
          2,
          3,
      }),
  });

  for (const auto& expr : {"min(c0)", "max(c0)"}) {
    testFailingAggregations(
        {batch, batchWithNull},
        {},
        {expr},
        "ARRAY comparison not supported for values that contain nulls");
    testFailingAggregations(
        {batch, batchWithNull},
        {"c1"},
        {expr},
        "ARRAY comparison not supported for values that contain nulls");
  }
}

TEST_F(MinMaxTest, rowCheckNull) {
  auto batch = makeRowVector({
      makeRowVector({
          makeFlatVector<StringView>({
              "a"_sv,
              "b"_sv,
              "c"_sv,
          }),
          makeNullableFlatVector<StringView>({
              "aa"_sv,
              "bb"_sv,
              "cc"_sv,
          }),
      }),
      makeFlatVector<int8_t>({1, 2, 3}),
  });

  auto batchWithNull = makeRowVector({
      makeRowVector({
          makeFlatVector<StringView>({
              "a"_sv,
              "b"_sv,
              "c"_sv,
          }),
          makeNullableFlatVector<StringView>({
              "aa"_sv,
              std::nullopt,
              "cc"_sv,
          }),
      }),
      makeFlatVector<int8_t>({1, 2, 3}),
  });

  for (const auto& expr : {"min(c0)", "max(c0)"}) {
    testFailingAggregations(
        {batch, batchWithNull},
        {},
        {expr},
        "ROW comparison not supported for values that contain nulls");
    testFailingAggregations(
        {batch, batchWithNull},
        {"c1"},
        {expr},
        "ROW comparison not supported for values that contain nulls");
  }
}

TEST_F(MinMaxTest, failOnUnorderableType) {
  auto data = makeRowVector({
      makeAllNullMapVector(5, VARCHAR(), BIGINT()),
      makeFlatVector<int32_t>({1, 2, 3, 4, 5}),
  });

  static const std::string kErrorMessage =
      "Aggregate function signature is not supported";
  for (const auto& expr : {"min(c0)", "max(c0)"}) {
    {
      auto builder = PlanBuilder().values({data});
      VELOX_ASSERT_THROW(builder.singleAggregation({}, {expr}), kErrorMessage);
    }

    {
      auto builder = PlanBuilder().values({data});
      VELOX_ASSERT_THROW(
          builder.singleAggregation({"c1"}, {expr}), kErrorMessage);
    }
  }
}

TEST_F(MinMaxTest, TimestampWithTimezone) {
  auto data = makeRowVector({
      makeFlatVector<int64_t>(
          {pack(-1, 2),
           pack(-3, 1),
           pack(0, 4),
           pack(2, 4),
           pack(3, 1),
           pack(-4, 5),
           pack(1, 3),
           pack(4, 0)},
          TIMESTAMP_WITH_TIME_ZONE()),
      // group by column
      makeFlatVector<int32_t>({1, 2, 2, 1, 1, 1, 2, 2}),
  });

  // Global aggregation.
  {
    auto expected = makeRowVector(
        {makeFlatVector<int64_t>(
             std::vector<int64_t>{pack(-4, 5)}, TIMESTAMP_WITH_TIME_ZONE()),
         makeFlatVector<int64_t>(
             std::vector<int64_t>{pack(4, 0)}, TIMESTAMP_WITH_TIME_ZONE())});

    testAggregations(
        {data},
        {},
        {
            "min(c0)",
            "max(c0)",
        },
        {expected});
  }

  // group-by aggregation.
  {
    auto expected = makeRowVector(
        {makeFlatVector<int32_t>({1, 2}),
         makeFlatVector<int64_t>(
             {pack(-4, 5), pack(-3, 1)}, TIMESTAMP_WITH_TIME_ZONE()),
         makeFlatVector<int64_t>(
             {pack(3, 1), pack(4, 0)}, TIMESTAMP_WITH_TIME_ZONE())});

    testAggregations({data}, {"c1"}, {"min(c0)", "max(c0)"}, {expected});
  }
}

class MinMaxNTest : public functions::aggregate::test::AggregationTestBase {
 protected:
  void SetUp() override {
    AggregationTestBase::SetUp();
  }

  template <typename T>
  void testNumericGlobal() {
    auto data =
        makeRowVector({makeFlatVector<T>({1, 10, 2, 9, 3, 8, 4, 7, 6, 5})});

    // DuckDB doesn't support min(x, n) or max(x, n) functions.

    auto expected = makeRowVector({
        makeArrayVector<T>({
            {1, 2},
        }),
        makeArrayVector<T>({
            {1, 2, 3, 4, 5},
        }),
        makeArrayVector<T>({
            {10, 9, 8},
        }),
        makeArrayVector<T>({
            {10, 9, 8, 7, 6, 5, 4},
        }),
    });

    testAggregations(
        {data},
        {},
        {"min(c0, 2)", "min(c0, 5)", "max(c0, 3)", "max(c0, 7)"},
        {expected});

    // Add some nulls. Expect these to be ignored.
    data = makeRowVector({
        makeNullableFlatVector<T>(
            {1,
             std::nullopt,
             10,
             2,
             9,
             std::nullopt,
             3,
             8,
             4,
             7,
             6,
             5,
             std::nullopt}),
    });

    testAggregations(
        {data},
        {},
        {"min(c0, 2)", "min(c0, 5)", "max(c0, 3)", "max(c0, 7)"},
        {expected});

    // Test all null input.
    data = makeRowVector({
        makeAllNullFlatVector<T>(100),
    });

    expected = makeRowVector({
        makeAllNullArrayVector(1, data->childAt(0)->type()),
        makeAllNullArrayVector(1, data->childAt(0)->type()),
        makeAllNullArrayVector(1, data->childAt(0)->type()),
        makeAllNullArrayVector(1, data->childAt(0)->type()),
    });

    testAggregations(
        {data},
        {},
        {"min(c0, 2)", "min(c0, 5)", "max(c0, 3)", "max(c0, 7)"},
        {expected});

    // Test the NULL handling in `N` param.
    data = makeRowVector({
        makeFlatVector<T>({1, 10, 2, 9, 3, 8, 4, 7, 6, 5}),
        // c1, used as the N of minN, with NULL in it.
        makeNullableFlatVector<int64_t>(
            {2, 2, std::nullopt, 2, 2, 2, 2, 2, 2, 2}),
        // c2, used as the N of maxN, with NULL in it.
        makeNullableFlatVector<int64_t>(
            {3, 3, 3, 3, 3, std::nullopt, 3, 3, 3, 3}),
        // c3, used as the N of minN/maxN, all NULL.
        makeAllNullFlatVector<int64_t>(10),
    });

    expected = makeRowVector(
        // min(c0, c1): Because of NULL N, 2 is ignored.
        {makeArrayVector<T>({
             {1, 3},
         }),
         // min(c0, c3): Since all N are NULL, the result is NULL.
         makeNullableArrayVector<T>({std::nullopt}),
         // max(c0, c2): Because of NULL N, 8 is ignored.
         makeArrayVector<T>({
             {10, 9, 7},
         }),
         // max(c0, c3): Since all N are NULL, the result is NULL.
         makeNullableArrayVector<T>({std::nullopt})});

    testAggregations(
        {data},
        {},
        {"min(c0, c1)", "min(c0, c3)", "max(c0, c2)", "max(c0, c3)"},
        {expected});

    // Second argument of max_n/min_n must be less than or equal to 10000.
    VELOX_ASSERT_THROW(
        testAggregations({data}, {}, {"min(c0, 10001)"}, {expected}),
        "second argument of max/min must be less than or equal to 10000");
    VELOX_ASSERT_THROW(
        testAggregations({data}, {}, {"max(c0, 10001)"}, {expected}),
        "second argument of max/min must be less than or equal to 10000");
  }

  template <typename T>
  void testNumericGlobalDecimal() {
    TypePtr type;
    if (std::is_same<T, int64_t>::value) {
      type = DECIMAL(6, 2);
    } else {
      type = DECIMAL(20, 2);
    }
    auto data = makeRowVector({
        makeFlatVector<T>(
            {100000,
             131011,
             223454,
             111911,
             111300,
             800000,
             104000,
             712452,
             161213,
             135243},
            type),
    });
    auto expected = makeRowVector({
        makeArrayVector<T>(
            {
                {100000, 104000},
            },
            type),
        makeArrayVector<T>(
            {
                {100000, 104000, 111300, 111911, 131011},
            },
            type),
        makeArrayVector<T>(
            {
                {800000, 712452, 223454},
            },
            type),
        makeArrayVector<T>(
            {
                {800000, 712452, 223454, 161213, 135243, 131011, 111911},
            },
            type),
    });

    testAggregations(
        {data},
        {},
        {"min(c0, 2)", "min(c0, 5)", "max(c0, 3)", "max(c0, 7)"},
        {expected});

    // Add some nulls. Expect these to be ignored.
    data = makeRowVector({
        makeNullableFlatVector<T>(
            {100000,
             std::nullopt,
             131011,
             223454,
             111911,
             std::nullopt,
             111300,
             800000,
             104000,
             712452,
             161213,
             135243,
             std::nullopt},
            type),
    });

    testAggregations(
        {data},
        {},
        {"min(c0, 2)", "min(c0, 5)", "max(c0, 3)", "max(c0, 7)"},
        {expected});

    // Test all null input.
    data = makeRowVector({
        makeNullableFlatVector<T>(
            {std::nullopt, std::nullopt, std::nullopt, std::nullopt}, type),
    });

    expected = makeRowVector({
        makeAllNullArrayVector(1, data->childAt(0)->type()),
        makeAllNullArrayVector(1, data->childAt(0)->type()),
        makeAllNullArrayVector(1, data->childAt(0)->type()),
        makeAllNullArrayVector(1, data->childAt(0)->type()),
    });

    testAggregations(
        {data},
        {},
        {"min(c0, 2)", "min(c0, 5)", "max(c0, 3)", "max(c0, 7)"},
        {expected});
  }

  template <typename T>
  void testNumericGroupBy() {
    auto data = makeRowVector({
        makeFlatVector<int16_t>({1, 2, 1, 1, 2, 2, 1, 2}),
        makeFlatVector<T>({1, 2, 4, 3, 6, 5, 7, 8}),
    });

    auto expected = makeRowVector({
        makeFlatVector<int16_t>({1, 2}),
        makeArrayVector<T>({
            {1, 3},
            {2, 5},
        }),
        makeArrayVector<T>({
            {1, 3, 4, 7},
            {2, 5, 6, 8},
        }),
        makeArrayVector<T>({
            {7, 4, 3},
            {8, 6, 5},
        }),
        makeArrayVector<T>({
            {7, 4, 3, 1},
            {8, 6, 5, 2},
        }),
    });

    testAggregations(
        {data},
        {"c0"},
        {"min(c1, 2)", "min(c1, 5)", "max(c1, 3)", "max(c1, 7)"},
        {expected});

    // Add some nulls. Expect these to be ignored.
    data = makeRowVector({
        makeFlatVector<int16_t>({1, 2, 1, 1, 1, 2, 2, 2, 1, 2}),
        makeNullableFlatVector<T>(
            {1, 2, std::nullopt, 4, 3, 6, std::nullopt, 5, 7, 8}),
    });

    testAggregations(
        {data},
        {"c0"},
        {"min(c1, 2)", "min(c1, 5)", "max(c1, 3)", "max(c1, 7)"},
        {expected});

    // Test all null input.
    data = makeRowVector({
        makeFlatVector<int16_t>({1, 2, 1, 1, 1, 2, 2, 2, 1, 2}),
        makeNullableFlatVector<T>(
            {std::nullopt,
             2,
             std::nullopt,
             std::nullopt,
             std::nullopt,
             6,
             std::nullopt,
             5,
             std::nullopt,
             8}),
    });

    expected = makeRowVector({
        makeFlatVector<int16_t>({1, 2}),
        makeNullableArrayVector<T>({
            std::nullopt,
            {{{2, 5}}},
        }),
        makeNullableArrayVector<T>({
            std::nullopt,
            {{{2, 5, 6, 8}}},
        }),
        makeNullableArrayVector<T>({
            std::nullopt,
            {{{8, 6, 5}}},
        }),
        makeNullableArrayVector<T>({
            std::nullopt,
            {{{8, 6, 5, 2}}},
        }),
    });

    testAggregations(
        {data},
        {"c0"},
        {"min(c1, 2)", "min(c1, 5)", "max(c1, 3)", "max(c1, 7)"},
        {expected});

    // Test the NULL handling in `N` param.
    data = makeRowVector({
        // Group by column.
        makeFlatVector<int16_t>({1, 2, 1, 1, 2, 2, 1, 2}),
        // Values.
        makeFlatVector<T>({1, 2, 4, 3, 6, 5, 7, 8}),
        // c2: used as the N of min, with NULL in it.
        makeNullableFlatVector<int64_t>(
            {2, 2, 2, std::nullopt, 2, std::nullopt, 2, 2}),
        // c3: used as the N of max, with NULL in it.
        makeNullableFlatVector<int64_t>(
            {3, 3, 3, 3, 3, 3, std::nullopt, std::nullopt}),
        // c4: used as the N of minN/maxN, all NULL.
        makeAllNullFlatVector<int64_t>(8),
    });

    expected = makeRowVector({
        makeFlatVector<int16_t>({1, 2}),
        // min(c1, c2): 3, 5 are ignored because of NULL N.
        makeArrayVector<T>({
            {1, 4},
            {2, 6},
        }),
        // min(c1, c4): Since all N are NULL, the result is NULL.
        makeNullableArrayVector<T>({std::nullopt, std::nullopt}),
        // max(c1, c3): 7, 8 are ignored because of NULL N.
        makeArrayVector<T>({
            {4, 3, 1},
            {6, 5, 2},
        }),
        // max(c1, c4): Since all N are NULL, the result is NULL.
        makeNullableArrayVector<T>({std::nullopt, std::nullopt}),
    });

    testAggregations(
        {data},
        {"c0"},
        {"min(c1, c2)", "min(c1, c4)", "max(c1, c3)", "max(c1, c4)"},
        {expected});
  }

  template <typename T>
  void testNumericGroupByDecimal() {
    TypePtr type;
    if (std::is_same<T, int64_t>::value) {
      type = DECIMAL(6, 2);
    } else {
      type = DECIMAL(20, 2);
    }

    auto data = makeRowVector({
        makeFlatVector<int16_t>({1, 2, 1, 1, 2, 2, 1, 2}),
        makeFlatVector<T>(
            {100000, 131011, 223454, 111911, 111300, 104000, 161213, 135243},
            type),
    });

    auto expected = makeRowVector({
        makeFlatVector<int16_t>({1, 2}),
        makeArrayVector<T>(
            {
                {100000, 111911},
                {104000, 111300},
            },
            type),
        makeArrayVector<T>(
            {
                {100000, 111911, 161213, 223454},
                {104000, 111300, 131011, 135243},
            },
            type),
        makeArrayVector<T>(
            {
                {223454, 161213, 111911},
                {135243, 131011, 111300},
            },
            type),
        makeArrayVector<T>(
            {
                {223454, 161213, 111911, 100000},
                {135243, 131011, 111300, 104000},
            },
            type),
    });

    testAggregations(
        {data},
        {"c0"},
        {"min(c1, 2)", "min(c1, 5)", "max(c1, 3)", "max(c1, 7)"},
        {expected});

    // Add some nulls. Expect these to be ignored.
    data = makeRowVector({
        makeFlatVector<int16_t>({1, 2, 1, 1, 1, 2, 2, 2, 1, 2}),
        makeNullableFlatVector<T>(
            {100000,
             131011,
             std::nullopt,
             223454,
             111911,
             111300,
             std::nullopt,
             104000,
             161213,
             135243},
            type),
    });

    testAggregations(
        {data},
        {"c0"},
        {"min(c1, 2)", "min(c1, 5)", "max(c1, 3)", "max(c1, 7)"},
        {expected});

    // Test all null input.
    data = makeRowVector({
        makeFlatVector<int16_t>({1, 2, 1, 1, 1, 2, 2, 2, 1, 2}),
        makeNullableFlatVector<T>(
            {std::nullopt,
             131011,
             std::nullopt,
             std::nullopt,
             std::nullopt,
             111300,
             std::nullopt,
             104000,
             std::nullopt,
             135243},
            type),
    });

    expected = makeRowVector({
        makeFlatVector<int16_t>({1, 2}),
        makeNullableArrayVector<T>(
            {
                std::nullopt,
                {{{104000, 111300}}},
            },
            ARRAY(type)),
        makeNullableArrayVector<T>(
            {
                std::nullopt,
                {{{104000, 111300, 131011, 135243}}},
            },
            ARRAY(type)),
        makeNullableArrayVector<T>(
            {
                std::nullopt,
                {{{135243, 131011, 111300}}},
            },
            ARRAY(type)),
        makeNullableArrayVector<T>(
            {
                std::nullopt,
                {{{135243, 131011, 111300, 104000}}},
            },
            ARRAY(type)),
    });

    testAggregations(
        {data},
        {"c0"},
        {"min(c1, 2)", "min(c1, 5)", "max(c1, 3)", "max(c1, 7)"},
        {expected});
  }

  template <typename T>
  void testNaNFloatValues() {
    // Tests to ensure NaN is correctly handled and considered greater than
    // Infinity.
    static const T kNaN = std::numeric_limits<T>::quiet_NaN();
    static const T kInf = std::numeric_limits<T>::infinity();

    auto data = makeRowVector({
        // regular ordering
        makeFlatVector<T>({2.0, kNaN, kInf, kNaN, -1.1, 0.0}),
        // with nulls (null is ignored)
        makeNullableFlatVector<T>({2.0, kNaN, std::nullopt, 1.1, -1.1, 0.0}),
        // group by column
        makeFlatVector<int32_t>({1, 1, 1, 2, 2, 2}),
    });

    // Global aggregation.
    {
      auto expected = makeRowVector(
          {makeArrayVector<T>({{-1.1, 0.0, 2.0, kInf, kNaN, kNaN}}),
           makeArrayVector<T>({{kNaN, kNaN, kInf, 2.0, 0.0, -1.1}}),
           makeArrayVector<T>({{-1.1, 0.0, 1.1, 2.0, kNaN}}),
           makeArrayVector<T>({{kNaN, 2.0, 1.1, 0.0, -1.1}})});

      testAggregations(
          {data},
          {},
          {
              "min(c0, 6)",
              "max(c0, 6)",
              "min(c1, 6)",
              "max(c1, 6)",
          },
          {expected});
    }

    // group-by aggregation.
    {
      auto expected = makeRowVector(
          {makeFlatVector<int32_t>({1, 2}),
           makeArrayVector<T>({{2.0, kInf, kNaN}, {-1.1, 0.0, kNaN}}),
           makeArrayVector<T>({{kNaN, kInf, 2.0}, {kNaN, 0.0, -1.1}}),
           makeArrayVector<T>({{2.0, kNaN}, {-1.1, 0.0, 1.1}}),
           makeArrayVector<T>({{kNaN, 2.0}, {1.1, 0.0, -1.1}})});

      testAggregations(
          {data},
          {"c2"},
          {"min(c0, 3)", "max(c0, 3)", "min(c1, 3)", "max(c1, 3)"},
          {expected});
    }
  }
};

TEST_F(MinMaxNTest, tinyint) {
  testNumericGlobal<int8_t>();
  testNumericGroupBy<int8_t>();
}

TEST_F(MinMaxNTest, smallint) {
  testNumericGlobal<int16_t>();
  testNumericGroupBy<int16_t>();
}

TEST_F(MinMaxNTest, integer) {
  testNumericGlobal<int32_t>();
  testNumericGroupBy<int32_t>();
}

TEST_F(MinMaxNTest, bigint) {
  testNumericGlobal<int64_t>();
  testNumericGroupBy<int64_t>();
}

TEST_F(MinMaxNTest, real) {
  testNumericGlobal<float>();
  testNumericGroupBy<float>();
  testNaNFloatValues<float>();
}

TEST_F(MinMaxNTest, double) {
  testNumericGlobal<double>();
  testNumericGroupBy<double>();
  testNaNFloatValues<double>();
}

TEST_F(MinMaxNTest, shortdecimal) {
  testNumericGlobalDecimal<int64_t>();
  testNumericGroupByDecimal<int64_t>();
}

TEST_F(MinMaxNTest, longdecimal) {
  testNumericGlobalDecimal<int128_t>();
  testNumericGroupByDecimal<int128_t>();
}

TEST_F(MinMaxNTest, string) {
  auto data = makeRowVector(
      {makeFlatVector<std::string>({"1", "2", "3", "4", "abc", "xyz"})});
  auto expected = makeRowVector({
      makeArrayVector<std::string>({
          {"1", "2"},
      }),
      makeArrayVector<std::string>({
          {"1", "2", "3", "4", "abc"},
      }),
      makeArrayVector<std::string>({
          {"xyz", "abc", "4"},
      }),
      makeArrayVector<std::string>({
          {"xyz", "abc", "4", "3", "2", "1"},
      }),
  });

  testAggregations(
      {data},
      {},
      {"min(c0, 2)", "min(c0, 5)", "max(c0, 3)", "max(c0, 7)"},
      {expected});

  // Add some nulls. Expect these to be ignored.
  data = makeRowVector({makeNullableFlatVector<std::string>(
      {"1",
       std::nullopt,
       "2",
       "3",
       "4",
       "abc",
       std::nullopt,
       "xyz",
       std::nullopt})});

  testAggregations(
      {data},
      {},
      {"min(c0, 2)", "min(c0, 5)", "max(c0, 3)", "max(c0, 7)"},
      {expected});

  // Test all null input.
  data = makeRowVector({makeNullableFlatVector<std::string>(
      {std::nullopt,
       std::nullopt,
       std::nullopt,
       std::nullopt,
       std::nullopt,
       std::nullopt,
       std::nullopt,
       std::nullopt,
       std::nullopt})});

  expected = makeRowVector({
      makeAllNullArrayVector(1, data->childAt(0)->type()),
      makeAllNullArrayVector(1, data->childAt(0)->type()),
      makeAllNullArrayVector(1, data->childAt(0)->type()),
      makeAllNullArrayVector(1, data->childAt(0)->type()),
  });

  testAggregations(
      {data},
      {},
      {"min(c0, 2)", "min(c0, 5)", "max(c0, 3)", "max(c0, 7)"},
      {expected});

  // Test long string
  data = makeRowVector({makeFlatVector<std::string>(
      {"hello long string",
       "hello long string2",
       "hello long string3",
       "hello long string a",
       "this is a very long string",
       "min max test",
       "max min test"})});
  expected = makeRowVector({
      makeArrayVector<std::string>({
          {"hello long string", "hello long string a"},
      }),
      makeArrayVector<std::string>({
          {"hello long string",
           "hello long string a",
           "hello long string2",
           "hello long string3",
           "max min test"},
      }),
      makeArrayVector<std::string>({
          {"this is a very long string", "min max test", "max min test"},
      }),
      makeArrayVector<std::string>({
          {"this is a very long string",
           "min max test",
           "max min test",
           "hello long string3",
           "hello long string2",
           "hello long string a",
           "hello long string"},
      }),
  });

  testAggregations(
      {data},
      {},
      {"min(c0, 2)", "min(c0, 5)", "max(c0, 3)", "max(c0, 7)"},
      {expected});
}

TEST_F(MinMaxNTest, incrementalWindow) {
  // SELECT
  //  c0, c1, c2, c3,
  //  max(c0, c1) over (partition by c2 order by c3 asc)
  // FROM (
  //  VALUES
  //      (1, 10, false, 0),
  //      (2, 10, false, 1)
  // ) AS t(c0, c1, c2, c3)
  auto data = makeRowVector({
      makeFlatVector<int64_t>({1, 2}),
      makeFlatVector<int64_t>({10, 10}),
      makeFlatVector<bool>({false, false}),
      makeFlatVector<int64_t>({0, 1}),
  });

  auto plan =
      PlanBuilder()
          .values({data})
          .window({"max(c0, c1) over (partition by c2 order by c3 asc)"})
          .planNode();

  // Expected result: {1, 10, false, 0, [1]}, {2, 10, false, 1, [2, 1]}.
  auto expected = makeRowVector({
      makeFlatVector<int64_t>({1, 2}),
      makeFlatVector<int64_t>({10, 10}),
      makeFlatVector<bool>({false, false}),
      makeFlatVector<int64_t>({0, 1}),
      makeArrayVector<int64_t>({{1}, {2, 1}}),
  });
  AssertQueryBuilder(plan).assertResults(expected);
}

} // namespace
