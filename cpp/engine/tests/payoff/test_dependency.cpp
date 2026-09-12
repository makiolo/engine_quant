// Tests de `DependencyVisitor` (PLAN_PRODUCTS.md §5.1, §12 Fase 1): reportes exactos de
// observables/monedas/fechas/eventos para las fixtures de Fase 0 (call/forward/swap/barrier).

#include <gtest/gtest.h>

#include "engine/payoff/dependency_visitor.hpp"

namespace {

using namespace engine::payoff;

TEST(DependencyVisitorTest, CallReportsObservableCurrencyAndFixingDate) {
    ContractPtr call = when(
        TimePoint{1.0},
        cashflow(
            Currency{"USD"},
            maximum(sub(fixing(ObservableId{"EQ.SPOT.AAPL"}, TimePoint{1.0}), constant(100.0)), constant(0.0))
        )
    );
    DependencyVisitor visitor;
    DependencyReport report = visitor.analyze(call);

    EXPECT_EQ(report.observables, (std::set<ObservableId>{ObservableId{"EQ.SPOT.AAPL"}}));
    EXPECT_EQ(report.currencies, (std::set<Currency>{Currency{"USD"}}));
    ASSERT_EQ(report.fixing_dates.size(), 1u);
    EXPECT_DOUBLE_EQ(report.fixing_dates.begin()->year_fraction, 1.0);
    EXPECT_TRUE(report.curves.empty());
    EXPECT_TRUE(report.events.empty());
}

TEST(DependencyVisitorTest, ForwardReportsSameDependenciesAsCall) {
    ContractPtr forward = when(
        TimePoint{1.0},
        cashflow(Currency{"USD"}, sub(fixing(ObservableId{"EQ.SPOT.AAPL"}, TimePoint{1.0}), constant(100.0)))
    );
    DependencyVisitor visitor;
    DependencyReport report = visitor.analyze(forward);

    EXPECT_EQ(report.observables, (std::set<ObservableId>{ObservableId{"EQ.SPOT.AAPL"}}));
    EXPECT_EQ(report.currencies, (std::set<Currency>{Currency{"USD"}}));
}

TEST(DependencyVisitorTest, FixedLegSwapReportsOnlyCurrencyNoObservables) {
    ContractPtr swap = both({
        when(TimePoint{1.0}, cashflow(Currency{"USD"}, constant(30000.0))),
        when(TimePoint{2.0}, cashflow(Currency{"USD"}, constant(30000.0))),
    });
    DependencyVisitor visitor;
    DependencyReport report = visitor.analyze(swap);

    EXPECT_TRUE(report.observables.empty());
    EXPECT_EQ(report.currencies, (std::set<Currency>{Currency{"USD"}}));
    EXPECT_TRUE(report.fixing_dates.empty());
}

TEST(DependencyVisitorTest, BarrierReportsObservableEventAndMonitoringTimesAsFixingDates) {
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

    DependencyVisitor visitor;
    DependencyReport report = visitor.analyze(barrier);

    EXPECT_EQ(report.observables, (std::set<ObservableId>{ObservableId{"EQ.SPOT.AAPL"}}));
    EXPECT_EQ(report.events, (std::set<EventId>{EventId{"UI"}}));
    EXPECT_EQ(report.fixing_dates.size(), 4u);
}

TEST(DependencyVisitorTest, FxConversionReportsBothCurrencies) {
    ContractPtr node = when(
        TimePoint{1.0},
        cashflow(Currency{"USD"}, mul(constant(100.0), fx_conversion(Currency{"EUR"}, Currency{"USD"}, TimePoint{1.0})))
    );
    DependencyVisitor visitor;
    DependencyReport report = visitor.analyze(node);
    EXPECT_EQ(report.currencies, (std::set<Currency>{Currency{"EUR"}, Currency{"USD"}}));
}

TEST(DependencyVisitorTest, DiscountFactorReportsCurve) {
    ContractPtr node = when(
        TimePoint{1.0},
        cashflow(Currency{"USD"}, mul(constant(100.0), discount_factor(CurveId{"IR.DF.EUR.OIS"}, TimePoint{0.0}, TimePoint{1.0})))
    );
    DependencyVisitor visitor;
    DependencyReport report = visitor.analyze(node);
    EXPECT_EQ(report.curves, (std::set<CurveId>{CurveId{"IR.DF.EUR.OIS"}}));
}

} // namespace
