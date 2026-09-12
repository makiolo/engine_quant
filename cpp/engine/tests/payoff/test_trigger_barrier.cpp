// Tests de las plantillas de barrera (PLAN_PRODUCTS.md §4.2, §12 Fase 2): up-and-in,
// down-and-out, double knock-out y window barrier, construidas sobre el mecanismo crudo de
// Trigger ya probado en test_trigger_state.cpp.

#include <gtest/gtest.h>

#include "engine/payoff/barrier_templates.hpp"
#include "engine/payoff/scenario_evaluator.hpp"

namespace {

using namespace engine::payoff;
using namespace engine::payoff::templates;

CashflowLedger evaluate(const ContractPtr& contract, MarketPath& path) {
    FixingStore historical;
    RuntimeState state;
    EvaluationContext context{path, historical, state};
    ScenarioEvaluator evaluator;
    return evaluator.evaluate(contract, context);
}

ContractPtr call_underlying() {
    return when(
        TimePoint{1.0},
        cashflow(
            Currency{"USD"},
            maximum(sub(fixing(ObservableId{"EQ.SPOT.AAPL"}, TimePoint{1.0}), constant(100.0)), constant(0.0))
        )
    );
}

std::vector<TimePoint> quarterly_schedule() {
    return {TimePoint{0.25}, TimePoint{0.5}, TimePoint{0.75}, TimePoint{1.0}};
}

// El spot salta de 118 (t=0.5, aun por debajo) a 122 (t=0.75, ya por encima de 120): nunca se
// observa exactamente en 120 -- confirma que el hit se registra con el fixing observado en el
// primer instante de monitorizacion donde la condicion es cierta, no al nivel exacto (§4.3).
MarketPath gapped_up_path() {
    MarketPath path;
    path.set_fixing(ObservableId{"EQ.SPOT.AAPL"}, TimePoint{0.25}, 105.0);
    path.set_fixing(ObservableId{"EQ.SPOT.AAPL"}, TimePoint{0.5}, 118.0);
    path.set_fixing(ObservableId{"EQ.SPOT.AAPL"}, TimePoint{0.75}, 122.0);
    path.set_fixing(ObservableId{"EQ.SPOT.AAPL"}, TimePoint{1.0}, 130.0);
    return path;
}

TEST(BarrierTemplateTest, UpAndInPaysUnderlyingWhenBarrierIsHitDespiteGap) {
    ContractPtr barrier =
        up_and_in(EventId{"UI"}, ObservableId{"EQ.SPOT.AAPL"}, 120.0, quarterly_schedule(), call_underlying());
    MarketPath path = gapped_up_path();
    CashflowLedger ledger = evaluate(barrier, path);
    ASSERT_EQ(ledger.size(), 1u);
    EXPECT_DOUBLE_EQ(ledger[0].payment_time.year_fraction, 1.0);
    EXPECT_DOUBLE_EQ(ledger[0].amount, 30.0);
}

TEST(BarrierTemplateTest, UpAndInNeverHitPaysZero) {
    ContractPtr barrier =
        up_and_in(EventId{"UI"}, ObservableId{"EQ.SPOT.AAPL"}, 120.0, quarterly_schedule(), call_underlying());
    MarketPath path;
    for (TimePoint t : quarterly_schedule()) path.set_fixing(ObservableId{"EQ.SPOT.AAPL"}, t, 90.0);
    CashflowLedger ledger = evaluate(barrier, path);
    EXPECT_TRUE(ledger.empty());
}

TEST(BarrierTemplateTest, DownAndOutKnockedOutPaysRebateInsteadOfUnderlying) {
    ContractPtr barrier = down_and_out(
        EventId{"DO"}, ObservableId{"EQ.SPOT.AAPL"}, 80.0, quarterly_schedule(), call_underlying(),
        cashflow(Currency{"USD"}, constant(5.0))
    );
    MarketPath path;
    path.set_fixing(ObservableId{"EQ.SPOT.AAPL"}, TimePoint{0.25}, 110.0);
    path.set_fixing(ObservableId{"EQ.SPOT.AAPL"}, TimePoint{0.5}, 75.0); // cruza hacia abajo
    path.set_fixing(ObservableId{"EQ.SPOT.AAPL"}, TimePoint{0.75}, 130.0);
    path.set_fixing(ObservableId{"EQ.SPOT.AAPL"}, TimePoint{1.0}, 130.0);
    CashflowLedger ledger = evaluate(barrier, path);
    ASSERT_EQ(ledger.size(), 1u);
    EXPECT_DOUBLE_EQ(ledger[0].amount, 5.0);
    EXPECT_DOUBLE_EQ(ledger[0].payment_time.year_fraction, 0.5); // rebate paga en el instante del hit
}

TEST(BarrierTemplateTest, DownAndOutNeverKnockedPaysUnderlying) {
    ContractPtr barrier = down_and_out(
        EventId{"DO"}, ObservableId{"EQ.SPOT.AAPL"}, 80.0, quarterly_schedule(), call_underlying(),
        cashflow(Currency{"USD"}, constant(5.0))
    );
    MarketPath path;
    for (TimePoint t : quarterly_schedule()) path.set_fixing(ObservableId{"EQ.SPOT.AAPL"}, t, 130.0);
    CashflowLedger ledger = evaluate(barrier, path);
    ASSERT_EQ(ledger.size(), 1u);
    EXPECT_DOUBLE_EQ(ledger[0].amount, 30.0);
    EXPECT_DOUBLE_EQ(ledger[0].payment_time.year_fraction, 1.0);
}

TEST(BarrierTemplateTest, DoubleKnockOutTriggersOnUpperBound) {
    ContractPtr barrier = double_knock_out(
        EventId{"DKO"}, ObservableId{"EQ.SPOT.AAPL"}, 80.0, 140.0, quarterly_schedule(), call_underlying(),
        cashflow(Currency{"USD"}, constant(1.0))
    );
    MarketPath path;
    path.set_fixing(ObservableId{"EQ.SPOT.AAPL"}, TimePoint{0.25}, 110.0);
    path.set_fixing(ObservableId{"EQ.SPOT.AAPL"}, TimePoint{0.5}, 150.0); // cruza el limite superior
    path.set_fixing(ObservableId{"EQ.SPOT.AAPL"}, TimePoint{0.75}, 110.0);
    path.set_fixing(ObservableId{"EQ.SPOT.AAPL"}, TimePoint{1.0}, 110.0);
    CashflowLedger ledger = evaluate(barrier, path);
    ASSERT_EQ(ledger.size(), 1u);
    EXPECT_DOUBLE_EQ(ledger[0].amount, 1.0);
}

TEST(BarrierTemplateTest, DoubleKnockOutTriggersOnLowerBound) {
    ContractPtr barrier = double_knock_out(
        EventId{"DKO"}, ObservableId{"EQ.SPOT.AAPL"}, 80.0, 140.0, quarterly_schedule(), call_underlying(),
        cashflow(Currency{"USD"}, constant(1.0))
    );
    MarketPath path;
    path.set_fixing(ObservableId{"EQ.SPOT.AAPL"}, TimePoint{0.25}, 110.0);
    path.set_fixing(ObservableId{"EQ.SPOT.AAPL"}, TimePoint{0.5}, 70.0); // cruza el limite inferior
    path.set_fixing(ObservableId{"EQ.SPOT.AAPL"}, TimePoint{0.75}, 110.0);
    path.set_fixing(ObservableId{"EQ.SPOT.AAPL"}, TimePoint{1.0}, 110.0);
    CashflowLedger ledger = evaluate(barrier, path);
    ASSERT_EQ(ledger.size(), 1u);
    EXPECT_DOUBLE_EQ(ledger[0].amount, 1.0);
}

TEST(BarrierTemplateTest, DoubleKnockOutStaysInCorridorPaysUnderlying) {
    ContractPtr barrier = double_knock_out(
        EventId{"DKO"}, ObservableId{"EQ.SPOT.AAPL"}, 80.0, 140.0, quarterly_schedule(), call_underlying(),
        cashflow(Currency{"USD"}, constant(1.0))
    );
    MarketPath path;
    for (TimePoint t : quarterly_schedule()) path.set_fixing(ObservableId{"EQ.SPOT.AAPL"}, t, 130.0);
    CashflowLedger ledger = evaluate(barrier, path);
    ASSERT_EQ(ledger.size(), 1u);
    EXPECT_DOUBLE_EQ(ledger[0].amount, 30.0);
}

TEST(WindowScheduleTest, FiltersToInclusiveRangePreservingOrder) {
    std::vector<TimePoint> full = {TimePoint{0.1}, TimePoint{0.3}, TimePoint{0.5}, TimePoint{0.7}, TimePoint{0.9}};
    std::vector<TimePoint> windowed = window_schedule(full, TimePoint{0.3}, TimePoint{0.7});
    ASSERT_EQ(windowed.size(), 3u);
    EXPECT_DOUBLE_EQ(windowed[0].year_fraction, 0.3);
    EXPECT_DOUBLE_EQ(windowed[1].year_fraction, 0.5);
    EXPECT_DOUBLE_EQ(windowed[2].year_fraction, 0.7);
}

TEST(BarrierTemplateTest, WindowBarrierIgnoresCrossingOutsideTheWindow) {
    // La barrera solo se monitoriza en [0.5, 0.75]; el cruce en t=0.25 queda fuera de ventana.
    std::vector<TimePoint> windowed = window_schedule(quarterly_schedule(), TimePoint{0.5}, TimePoint{0.75});
    ContractPtr barrier = up_and_in(EventId{"UI"}, ObservableId{"EQ.SPOT.AAPL"}, 120.0, windowed, call_underlying());

    MarketPath path;
    path.set_fixing(ObservableId{"EQ.SPOT.AAPL"}, TimePoint{0.25}, 130.0); // cruce fuera de ventana: ignorado
    path.set_fixing(ObservableId{"EQ.SPOT.AAPL"}, TimePoint{0.5}, 90.0);
    path.set_fixing(ObservableId{"EQ.SPOT.AAPL"}, TimePoint{0.75}, 90.0);
    path.set_fixing(ObservableId{"EQ.SPOT.AAPL"}, TimePoint{1.0}, 90.0);
    CashflowLedger ledger = evaluate(barrier, path);
    EXPECT_TRUE(ledger.empty());
}

} // namespace
