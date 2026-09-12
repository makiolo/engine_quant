// Tests del mecanismo crudo de `Trigger`/`EventState` en `ScenarioEvaluator` (PLAN_PRODUCTS.md
// §4.1, §12 Fase 2, ADR-P0-03/ADR-P0-08), usando `trigger()` directamente -- antes de construir
// las plantillas de barrera/TP-SL (test_trigger_barrier.cpp/test_tp_sl.cpp) encima.

#include <gtest/gtest.h>

#include "engine/payoff/scenario_evaluator.hpp"

namespace {

using namespace engine::payoff;

CashflowLedger evaluate(const ContractPtr& contract, MarketPath& path) {
    FixingStore historical;
    RuntimeState state;
    EvaluationContext context{path, historical, state};
    ScenarioEvaluator evaluator;
    return evaluator.evaluate(contract, context);
}

TEST(TriggerStateTest, FirstCrossingAboveBarrierPaysAtScheduledPayment) {
    MarketPath path;
    path.set_fixing(ObservableId{"EQ.SPOT.AAPL"}, TimePoint{0.25}, 105.0);
    path.set_fixing(ObservableId{"EQ.SPOT.AAPL"}, TimePoint{0.5}, 118.0);
    path.set_fixing(ObservableId{"EQ.SPOT.AAPL"}, TimePoint{0.75}, 122.0);
    path.set_fixing(ObservableId{"EQ.SPOT.AAPL"}, TimePoint{1.0}, 130.0);

    TriggerSpec spec{
        EventId{"UI"}, {TimePoint{0.25}, TimePoint{0.5}, TimePoint{0.75}, TimePoint{1.0}},
        greater_equal(current(ObservableId{"EQ.SPOT.AAPL"}), constant(120.0)), Monitoring::Discrete,
        Settlement::AtScheduledPayment, 0, true
    };
    ContractPtr on_hit = when(
        TimePoint{1.0},
        cashflow(
            Currency{"USD"},
            maximum(sub(fixing(ObservableId{"EQ.SPOT.AAPL"}, TimePoint{1.0}), constant(100.0)), constant(0.0))
        )
    );
    ContractPtr barrier = trigger(spec, on_hit, zero());

    CashflowLedger ledger = evaluate(barrier, path);
    ASSERT_EQ(ledger.size(), 1u);
    EXPECT_DOUBLE_EQ(ledger[0].payment_time.year_fraction, 1.0);
    EXPECT_DOUBLE_EQ(ledger[0].amount, 30.0);
    ASSERT_TRUE(ledger[0].source_event.has_value());
    EXPECT_EQ(*ledger[0].source_event, EventId{"UI"});
}

TEST(TriggerStateTest, NoCrossingTakesOnMissBranch) {
    MarketPath path;
    path.set_fixing(ObservableId{"EQ.SPOT.AAPL"}, TimePoint{1.0}, 90.0);
    TriggerSpec spec{
        EventId{"UI"}, {TimePoint{1.0}}, greater_equal(current(ObservableId{"EQ.SPOT.AAPL"}), constant(120.0)),
        Monitoring::Discrete, Settlement::AtScheduledPayment, 0, true
    };
    ContractPtr barrier = trigger(spec, cashflow(Currency{"USD"}, constant(999.0)), zero());
    CashflowLedger ledger = evaluate(barrier, path);
    EXPECT_TRUE(ledger.empty());
}

TEST(TriggerStateTest, LatchTrueKeepsFirstHitEvenIfConditionLaterFalse) {
    MarketPath path;
    path.set_fixing(ObservableId{"EQ.SPOT.AAPL"}, TimePoint{0.5}, 125.0); // cruza
    path.set_fixing(ObservableId{"EQ.SPOT.AAPL"}, TimePoint{1.0}, 90.0);  // ya no cruza
    TriggerSpec spec{
        EventId{"UI"}, {TimePoint{0.5}, TimePoint{1.0}}, greater_equal(current(ObservableId{"EQ.SPOT.AAPL"}), constant(120.0)),
        Monitoring::Discrete, Settlement::AtHit, 0, true
    };
    ContractPtr on_hit = cashflow(Currency{"USD"}, event_value(EventId{"UI"}, ObservableId{"EQ.SPOT.AAPL"}));
    ContractPtr barrier = trigger(spec, on_hit, zero());

    CashflowLedger ledger = evaluate(barrier, path);
    ASSERT_EQ(ledger.size(), 1u);
    EXPECT_DOUBLE_EQ(ledger[0].payment_time.year_fraction, 0.5);
    EXPECT_DOUBLE_EQ(ledger[0].amount, 125.0);
}

TEST(TriggerStateTest, LatchFalseUnfiresWhenConditionNoLongerHolds) {
    MarketPath path;
    path.set_fixing(ObservableId{"EQ.SPOT.AAPL"}, TimePoint{0.5}, 125.0);
    path.set_fixing(ObservableId{"EQ.SPOT.AAPL"}, TimePoint{1.0}, 90.0);
    TriggerSpec spec{
        EventId{"UI"}, {TimePoint{0.5}, TimePoint{1.0}}, greater_equal(current(ObservableId{"EQ.SPOT.AAPL"}), constant(120.0)),
        Monitoring::Discrete, Settlement::AtScheduledPayment, 0, false
    };
    ContractPtr barrier = trigger(spec, when(TimePoint{1.0}, cashflow(Currency{"USD"}, constant(1.0))), zero());
    CashflowLedger ledger = evaluate(barrier, path);
    EXPECT_TRUE(ledger.empty());
}

TEST(TriggerStateTest, SettlementAtHitFixesCursorToFirstHitTime) {
    MarketPath path;
    path.set_fixing(ObservableId{"EQ.SPOT.AAPL"}, TimePoint{0.5}, 121.0);
    path.set_fixing(ObservableId{"EQ.SPOT.AAPL"}, TimePoint{0.75}, 128.0);
    TriggerSpec spec{
        EventId{"UI"}, {TimePoint{0.5}, TimePoint{0.75}}, greater_equal(current(ObservableId{"EQ.SPOT.AAPL"}), constant(120.0)),
        Monitoring::Discrete, Settlement::AtHit, 0, true
    };
    // Cashflow "desnudo" (sin When propio): valido porque settlement=AtHit fija el cursor.
    ContractPtr on_hit = cashflow(Currency{"USD"}, constant(50.0));
    ContractPtr barrier = trigger(spec, on_hit, zero());

    CashflowLedger ledger = evaluate(barrier, path);
    ASSERT_EQ(ledger.size(), 1u);
    EXPECT_DOUBLE_EQ(ledger[0].payment_time.year_fraction, 0.5); // primer hit, no el ultimo
}

TEST(TriggerStateTest, HigherPriorityTriggerWinsOnSameDateViaMutualExclusionGuard) {
    // Replica el patron de FirstOf TP/SL (ADR-P0-08): dos triggers con el mismo id de
    // monitorizacion, cada uno con guarda Not(EventOccurred(el_otro)).
    MarketPath path;
    path.set_fixing(ObservableId{"EQ.SPOT.AAPL"}, TimePoint{0.2}, 128.0); // dispara ambas condiciones "en bruto"

    auto metric = sub(div(current(ObservableId{"EQ.SPOT.AAPL"}), constant(100.0)), constant(1.0));

    TriggerSpec tp_spec{
        EventId{"TAKE_PROFIT"}, {TimePoint{0.2}},
        all_of({greater_equal(metric, constant(0.20)), negate(event_occurred(EventId{"STOP_LOSS"}))}),
        Monitoring::Discrete, Settlement::AtHit, 10, true
    };
    // STOP_LOSS "dispararia en bruto" tambien si su condicion fuese amplia; aqui usamos una
    // condicion que en este fixture es igualmente cierta (>= -1.0) para probar la exclusion
    // mutua por prioridad, no la metrica real de un SL.
    TriggerSpec sl_spec{
        EventId{"STOP_LOSS"}, {TimePoint{0.2}},
        all_of({greater_equal(metric, constant(-1.0)), negate(event_occurred(EventId{"TAKE_PROFIT"}))}),
        Monitoring::Discrete, Settlement::AtHit, 20, true
    };

    ContractPtr tp = trigger(tp_spec, cashflow(Currency{"USD"}, constant(1.0)), zero());
    ContractPtr sl = trigger(sl_spec, cashflow(Currency{"USD"}, constant(2.0)), zero());
    ContractPtr tree = both({tp, sl});

    CashflowLedger ledger = evaluate(tree, path);
    ASSERT_EQ(ledger.size(), 1u);
    EXPECT_DOUBLE_EQ(ledger[0].amount, 1.0); // TAKE_PROFIT (priority 10) gana, STOP_LOSS queda bloqueado
    EXPECT_EQ(*ledger[0].source_event, EventId{"TAKE_PROFIT"});
}

TEST(TriggerStateTest, EventTimeReflectsFirstHit) {
    MarketPath path;
    path.set_fixing(ObservableId{"EQ.SPOT.AAPL"}, TimePoint{0.5}, 125.0);
    TriggerSpec spec{
        EventId{"UI"}, {TimePoint{0.5}}, greater_equal(current(ObservableId{"EQ.SPOT.AAPL"}), constant(120.0)),
        Monitoring::Discrete, Settlement::AtScheduledPayment, 0, true
    };
    ContractPtr on_hit = when(TimePoint{1.0}, cashflow(Currency{"USD"}, event_time(EventId{"UI"})));
    ContractPtr barrier = trigger(spec, on_hit, zero());

    CashflowLedger ledger = evaluate(barrier, path);
    ASSERT_EQ(ledger.size(), 1u);
    EXPECT_DOUBLE_EQ(ledger[0].amount, 0.5);
}

} // namespace
