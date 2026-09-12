// Tests de la plantilla FirstOf TP/SL (PLAN_PRODUCTS.md §4.3, §12 Fase 2): ejemplo literal del
// documento (entry=100, quantity=10, TP+20%/SL-10%, gap) más no-doble-pago y variaciones de
// métrica. Cierra Fase 2 junto con test_trigger_barrier.cpp.

#include <gtest/gtest.h>

#include "engine/payoff/scenario_evaluator.hpp"
#include "engine/payoff/tp_sl.hpp"

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

TpSlSpec return_from_entry_spec() {
    return TpSlSpec{
        ObservableId{"EQ.SPOT.AAPL"}, TpSlMetric::ReturnFromEntry, 100.0, 10.0, 0.20, -0.10,
        {TimePoint{0.10}, TimePoint{0.20}}, Currency{"USD"}, EventId{"TAKE_PROFIT"}, EventId{"STOP_LOSS"}
    };
}

// Ejemplo literal de §4.3: retorno del 8% a t=0.10 (sin hit), salto al 28% a t=0.20 -- el TP
// dispara con el fixing observado (128), no al nivel exacto que implicaria el 20% (120).
TEST(TpSlTemplateTest, TakeProfitFiresOnGapAndPaysRealizedPnlAtObservedFixing) {
    ContractPtr group = first_of_take_profit_stop_loss(return_from_entry_spec());
    MarketPath path;
    path.set_fixing(ObservableId{"EQ.SPOT.AAPL"}, TimePoint{0.10}, 108.0);
    path.set_fixing(ObservableId{"EQ.SPOT.AAPL"}, TimePoint{0.20}, 128.0);

    CashflowLedger ledger = evaluate(group, path);
    ASSERT_EQ(ledger.size(), 1u);
    EXPECT_DOUBLE_EQ(ledger[0].payment_time.year_fraction, 0.20);
    EXPECT_DOUBLE_EQ(ledger[0].amount, 280.0); // 10 * (128 - 100)
    EXPECT_EQ(ledger[0].currency, Currency{"USD"});
    ASSERT_TRUE(ledger[0].source_event.has_value());
    EXPECT_EQ(*ledger[0].source_event, EventId{"TAKE_PROFIT"});
}

TEST(TpSlTemplateTest, StopLossFiresWhenReturnDropsBelowThreshold) {
    ContractPtr group = first_of_take_profit_stop_loss(return_from_entry_spec());
    MarketPath path;
    path.set_fixing(ObservableId{"EQ.SPOT.AAPL"}, TimePoint{0.10}, 95.0);
    path.set_fixing(ObservableId{"EQ.SPOT.AAPL"}, TimePoint{0.20}, 85.0); // retorno -15%

    CashflowLedger ledger = evaluate(group, path);
    ASSERT_EQ(ledger.size(), 1u);
    EXPECT_DOUBLE_EQ(ledger[0].payment_time.year_fraction, 0.20);
    EXPECT_DOUBLE_EQ(ledger[0].amount, -150.0); // 10 * (85 - 100)
    EXPECT_EQ(*ledger[0].source_event, EventId{"STOP_LOSS"});
}

TEST(TpSlTemplateTest, NeitherThresholdHitPaysNothing) {
    ContractPtr group = first_of_take_profit_stop_loss(return_from_entry_spec());
    MarketPath path;
    path.set_fixing(ObservableId{"EQ.SPOT.AAPL"}, TimePoint{0.10}, 102.0);
    path.set_fixing(ObservableId{"EQ.SPOT.AAPL"}, TimePoint{0.20}, 105.0);

    CashflowLedger ledger = evaluate(group, path);
    EXPECT_TRUE(ledger.empty());
}

TEST(TpSlTemplateTest, TakeProfitDoesNotPayTwiceWhenThresholdStaysExceeded) {
    ContractPtr group = first_of_take_profit_stop_loss(return_from_entry_spec());
    MarketPath path;
    path.set_fixing(ObservableId{"EQ.SPOT.AAPL"}, TimePoint{0.10}, 125.0); // ya +25%: primer hit aqui
    path.set_fixing(ObservableId{"EQ.SPOT.AAPL"}, TimePoint{0.20}, 140.0); // sigue por encima

    CashflowLedger ledger = evaluate(group, path);
    ASSERT_EQ(ledger.size(), 1u);
    EXPECT_DOUBLE_EQ(ledger[0].payment_time.year_fraction, 0.10); // primer hit, no el ultimo
    EXPECT_DOUBLE_EQ(ledger[0].amount, 250.0); // 10 * (125 - 100), no el fixing de t=0.20
}

TEST(TpSlTemplateTest, TakeProfitWinsOverStopLossWhenBothConditionsHoldSameDate) {
    // Umbrales deliberadamente laxos para que ambas condiciones "en bruto" sean ciertas el
    // mismo dia: TAKE_PROFIT (priority=10) se comprueba primero y bloquea STOP_LOSS via guarda.
    TpSlSpec spec = return_from_entry_spec();
    spec.take_profit_level = -0.50;
    spec.stop_loss_level = 0.50;
    ContractPtr group = first_of_take_profit_stop_loss(spec);

    MarketPath path;
    path.set_fixing(ObservableId{"EQ.SPOT.AAPL"}, TimePoint{0.10}, 100.0);
    path.set_fixing(ObservableId{"EQ.SPOT.AAPL"}, TimePoint{0.20}, 100.0);

    CashflowLedger ledger = evaluate(group, path);
    ASSERT_EQ(ledger.size(), 1u);
    EXPECT_EQ(*ledger[0].source_event, EventId{"TAKE_PROFIT"});
}

TEST(TpSlTemplateTest, UnderlyingPriceMetricComparesAbsoluteLevel) {
    TpSlSpec spec = return_from_entry_spec();
    spec.metric = TpSlMetric::UnderlyingPrice;
    spec.take_profit_level = 120.0;
    spec.stop_loss_level = 90.0;
    ContractPtr group = first_of_take_profit_stop_loss(spec);

    MarketPath path;
    path.set_fixing(ObservableId{"EQ.SPOT.AAPL"}, TimePoint{0.10}, 110.0);
    path.set_fixing(ObservableId{"EQ.SPOT.AAPL"}, TimePoint{0.20}, 125.0);

    CashflowLedger ledger = evaluate(group, path);
    ASSERT_EQ(ledger.size(), 1u);
    EXPECT_EQ(*ledger[0].source_event, EventId{"TAKE_PROFIT"});
    EXPECT_DOUBLE_EQ(ledger[0].amount, 250.0); // 10 * (125 - 100)
}

TEST(TpSlTemplateTest, MonetaryPnlMetricComparesCurrencyAmount) {
    TpSlSpec spec = return_from_entry_spec();
    spec.metric = TpSlMetric::MonetaryPnl;
    spec.take_profit_level = 200.0; // 10 * (S - 100) >= 200 <=> S >= 120
    spec.stop_loss_level = -100.0;
    ContractPtr group = first_of_take_profit_stop_loss(spec);

    MarketPath path;
    path.set_fixing(ObservableId{"EQ.SPOT.AAPL"}, TimePoint{0.10}, 110.0);
    path.set_fixing(ObservableId{"EQ.SPOT.AAPL"}, TimePoint{0.20}, 121.0);

    CashflowLedger ledger = evaluate(group, path);
    ASSERT_EQ(ledger.size(), 1u);
    EXPECT_EQ(*ledger[0].source_event, EventId{"TAKE_PROFIT"});
}

} // namespace
