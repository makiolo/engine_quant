// Tests de `ValidationVisitor` (PLAN_PRODUCTS.md §3.1, §7.1, §12 Fase 1, ADR-P0-05,
// ADR-P0-08): agrega todos los errores con su NodePath, nunca se detiene en el primero.

#include <gtest/gtest.h>

#include <limits>
#include <string>
#include <vector>

#include "engine/payoff/validation_visitor.hpp"

namespace {

using namespace engine::payoff;

bool has_error_containing(const std::vector<ValidationError>& errors, const std::string& substr) {
    for (const auto& e : errors) {
        if (std::string(e.what()).find(substr) != std::string::npos) return true;
    }
    return false;
}

TEST(ValidationVisitorTest, ValidCallProducesNoErrors) {
    // when(1.0, cashflow(USD, max(fixing(AAPL,1.0) - 100, 0)))
    ContractPtr call = when(
        TimePoint{1.0},
        cashflow(
            Currency{"USD"},
            maximum(sub(fixing(ObservableId{"EQ.SPOT.AAPL"}, TimePoint{1.0}), constant(100.0)), constant(0.0))
        )
    );
    ValidationVisitor validator;
    EXPECT_TRUE(validator.validate(call).empty());
}

TEST(ValidationVisitorTest, CashflowWithoutActiveCursorIsRejected) {
    ContractPtr naked = cashflow(Currency{"USD"}, constant(1.0));
    ValidationVisitor validator;
    auto errors = validator.validate(naked);
    ASSERT_FALSE(errors.empty());
    EXPECT_TRUE(has_error_containing(errors, "instante activo"));
    EXPECT_EQ(errors.front().path().to_string(), "root");
}

TEST(ValidationVisitorTest, CurrentWithoutActiveCursorIsRejected) {
    ContractPtr naked = cashflow(Currency{"USD"}, current(ObservableId{"EQ.SPOT.AAPL"}));
    // Envuelto en When para que el propio Cashflow sea valido, pero Current hereda el mismo
    // cursor activo -- aqui forzamos el caso sin cursor via on_miss de un Trigger.
    TriggerSpec spec{
        EventId{"T"}, {TimePoint{1.0}}, greater_equal(constant(1.0), constant(0.0)),
        Monitoring::Discrete, Settlement::AtScheduledPayment, 0, true
    };
    ContractPtr tree = trigger(spec, zero(), naked);
    ValidationVisitor validator;
    auto errors = validator.validate(tree);
    EXPECT_TRUE(has_error_containing(errors, "Current sin instante activo"));
}

TEST(ValidationVisitorTest, TriggerOnHitAtHitHasActiveCursor) {
    TriggerSpec spec{
        EventId{"UI"}, {TimePoint{0.5}, TimePoint{1.0}}, greater_equal(current(ObservableId{"EQ.SPOT.AAPL"}), constant(120.0)),
        Monitoring::Discrete, Settlement::AtHit, 0, true
    };
    ContractPtr on_hit = cashflow(Currency{"USD"}, constant(1.0));
    ContractPtr tree = trigger(spec, on_hit, zero());
    ValidationVisitor validator;
    EXPECT_TRUE(validator.validate(tree).empty());
}

TEST(ValidationVisitorTest, TriggerOnHitAtScheduledPaymentWithoutWhenIsRejected) {
    TriggerSpec spec{
        EventId{"UI"}, {TimePoint{1.0}}, greater_equal(constant(1.0), constant(0.0)),
        Monitoring::Discrete, Settlement::AtScheduledPayment, 0, true
    };
    ContractPtr naked_on_hit = cashflow(Currency{"USD"}, constant(1.0));
    ContractPtr tree = trigger(spec, naked_on_hit, zero());
    ValidationVisitor validator;
    auto errors = validator.validate(tree);
    EXPECT_TRUE(has_error_containing(errors, "instante activo"));
}

TEST(ValidationVisitorTest, EmptyCurrencyIsRejected) {
    ContractPtr tree = when(TimePoint{1.0}, cashflow(Currency{""}, constant(1.0)));
    ValidationVisitor validator;
    auto errors = validator.validate(tree);
    EXPECT_TRUE(has_error_containing(errors, "moneda vacia"));
}

TEST(ValidationVisitorTest, NonFiniteTimeIsRejected) {
    ContractPtr tree = when(
        TimePoint{std::numeric_limits<double>::infinity()}, cashflow(Currency{"USD"}, constant(1.0))
    );
    ValidationVisitor validator;
    auto errors = validator.validate(tree);
    EXPECT_TRUE(has_error_containing(errors, "no finita"));
}

TEST(ValidationVisitorTest, UnorderedMonitoringTimesIsRejected) {
    TriggerSpec spec{
        EventId{"UI"}, {TimePoint{1.0}, TimePoint{0.5}}, greater_equal(constant(1.0), constant(0.0)),
        Monitoring::Discrete, Settlement::AtHit, 0, true
    };
    ContractPtr tree = trigger(spec, cashflow(Currency{"USD"}, constant(1.0)), zero());
    ValidationVisitor validator;
    auto errors = validator.validate(tree);
    EXPECT_TRUE(has_error_containing(errors, "ordenado ascendente"));
}

TEST(ValidationVisitorTest, DuplicateEventIdIsRejected) {
    TriggerSpec spec1{
        EventId{"SAME"}, {TimePoint{1.0}}, greater_equal(constant(1.0), constant(0.0)),
        Monitoring::Discrete, Settlement::AtHit, 0, true
    };
    TriggerSpec spec2 = spec1;
    ContractPtr t1 = trigger(spec1, cashflow(Currency{"USD"}, constant(1.0)), zero());
    ContractPtr t2 = trigger(spec2, cashflow(Currency{"USD"}, constant(1.0)), zero());
    ContractPtr tree = both({t1, t2});
    ValidationVisitor validator;
    auto errors = validator.validate(tree);
    EXPECT_TRUE(has_error_containing(errors, "duplicado"));
}

TEST(ValidationVisitorTest, DanglingEventReferenceIsRejected) {
    ContractPtr tree = when(
        TimePoint{1.0}, cashflow(Currency{"USD"}, event_value(EventId{"NUNCA_DEFINIDO"}, ObservableId{"EQ.SPOT.AAPL"}))
    );
    ValidationVisitor validator;
    auto errors = validator.validate(tree);
    EXPECT_TRUE(has_error_containing(errors, "sin Trigger/Exercise que lo defina"));
}

TEST(ValidationVisitorTest, ContinuousApproximationIsRejectedInFases1And2) {
    TriggerSpec spec{
        EventId{"UI"}, {TimePoint{1.0}}, greater_equal(constant(1.0), constant(0.0)),
        Monitoring::ContinuousApproximation, Settlement::AtHit, 0, true
    };
    ContractPtr tree = trigger(spec, cashflow(Currency{"USD"}, constant(1.0)), zero());
    ValidationVisitor validator;
    auto errors = validator.validate(tree);
    EXPECT_TRUE(has_error_containing(errors, "Brownian bridge"));
}

TEST(ValidationVisitorTest, AllErrorsAreAggregatedNotJustFirst) {
    ContractPtr tree = both({
        cashflow(Currency{""}, constant(1.0)),                  // sin cursor + moneda vacia
        when(TimePoint{1.0}, cashflow(Currency{""}, constant(1.0))) // moneda vacia
    });
    ValidationVisitor validator;
    auto errors = validator.validate(tree);
    EXPECT_GE(errors.size(), 3u);
}

TEST(ValidationVisitorTest, ExceedingMaxDepthIsRejected) {
    ScalarExprPtr expr = constant(1.0);
    for (int i = 0; i < 2000; ++i) {
        expr = neg(expr);
    }
    ContractPtr tree = when(TimePoint{1.0}, cashflow(Currency{"USD"}, expr));
    ValidationVisitor validator;
    auto errors = validator.validate(tree);
    EXPECT_TRUE(has_error_containing(errors, "profundidad maxima"));
}

} // namespace
