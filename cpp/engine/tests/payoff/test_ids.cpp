// Tests del esqueleto del motor de payoff (PLAN_PRODUCTS.md §12 Fase 0-1, ADR-P0-01):
// tolerancia temporal centralizada y construcción de `NodePath` para diagnósticos.

#include <gtest/gtest.h>

#include "engine/payoff/errors.hpp"
#include "engine/payoff/ids.hpp"

namespace {

using engine::payoff::EventId;
using engine::payoff::NodePath;
using engine::payoff::ObservableId;
using engine::payoff::TimePoint;
using engine::payoff::time_equal;
using engine::payoff::time_less;

TEST(PayoffIds, TimeEqualWithinToleranceIsEqual) {
    EXPECT_TRUE(time_equal(TimePoint{1.0}, TimePoint{1.0 + 1e-10}));
    EXPECT_TRUE(time_equal(TimePoint{1.0}, TimePoint{1.0 - 1e-10}));
}

TEST(PayoffIds, TimeEqualBeyondToleranceIsNotEqual) {
    EXPECT_FALSE(time_equal(TimePoint{1.0}, TimePoint{1.0 + 1e-6}));
}

TEST(PayoffIds, TimeLessOrdersBeyondTolerance) {
    EXPECT_TRUE(time_less(TimePoint{1.0}, TimePoint{2.0}));
    EXPECT_FALSE(time_less(TimePoint{2.0}, TimePoint{1.0}));
    EXPECT_FALSE(time_less(TimePoint{1.0}, TimePoint{1.0 + 1e-10}));
}

TEST(PayoffIds, ObservableIdEqualityIsValueBased) {
    EXPECT_EQ(ObservableId{"EQ.SPOT.AAPL"}, ObservableId{"EQ.SPOT.AAPL"});
    EXPECT_NE(ObservableId{"EQ.SPOT.AAPL"}, ObservableId{"EQ.SPOT.MSFT"});
}

TEST(PayoffIds, EventIdCanonicalOrderIsLexicographic) {
    EXPECT_TRUE(EventId{"STOP_LOSS"} < EventId{"TAKE_PROFIT"});
}

TEST(NodePathTest, RootToStringIsRoot) {
    EXPECT_EQ(NodePath::root().to_string(), "root");
}

TEST(NodePathTest, ChildBuildsDottedPath) {
    NodePath path = NodePath::root().child(1, "children").child("condition").child("left");
    EXPECT_EQ(path.to_string(), "root.children[1].condition.left");
}

TEST(ValidationErrorTest, WhatIncludesPathAndMessage) {
    engine::payoff::ValidationError error("moneda vacia", NodePath::root().child("currency"));
    EXPECT_EQ(error.path().to_string(), "root.currency");
    EXPECT_STREQ(error.what(), "root.currency: moneda vacia");
}

} // namespace
