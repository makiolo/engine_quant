// Cruza el AST generico de payoff contra la implementacion legacy de IRS (PLAN_PRODUCTS.md
// SS9.3, SS13.4): replica el swap fijo-flotante como un unico Cashflow a t=0 usando
// DiscountFactor (SS3.2, "para el caso single-curve actual, replica equivalente documentada")
// y compara la PV resultante -- via la medida generica `present_value` de Fase 4 (SS12) -- contra
// engine::price()/Registry<IMeasure> real. Solo lectura de engine/bootstrap.hpp y
// engine/market.hpp -- ninguna fuente de produccion nueva en este commit.

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <vector>

#include "engine/bootstrap.hpp"
#include "engine/payoff/measures.hpp"
#include "engine/price.hpp"

namespace {

using engine::ExecutionContext;
using engine::MarketSnapshot;
using engine::Params;
using engine::PricingContext;
using engine::Registries;
using engine::register_builtins;

namespace pf = engine::payoff;

TEST(MeasureCrossCheckTest, FixedForFloatingSwapAstMatchesLegacyPv) {
    const double notional = 1'000'000.0;
    const double fixed_rate = 0.03;
    const double start = 0.0;
    const std::vector<double> payment_times{1.0, 2.0, 3.0, 4.0, 5.0};
    const std::vector<double> accruals{1.0, 1.0, 1.0, 1.0, 1.0};

    // ---- Legacy: IrSwapProduct + Registry<IMeasure> (measure.cpp real, sin tocarlo) ----
    Registries registries;
    register_builtins(registries);
    auto model = registries.models.create(
        "HullWhite1F", Params{{"a", 0.1}, {"b", 0.03}, {"sigma", 0.01}, {"r0", 0.02}}
    );
    auto product = registries.products.create(
        "IRSwap", Params{
                      {"notional", notional},
                      {"fixed_rate", fixed_rate},
                      {"start", start},
                      {"payment_times", payment_times},
                      {"accruals", accruals},
                  }
    );
    auto measure = registries.measures.create("PV");
    MarketSnapshot market({1.0}, {0.02});
    PricingContext pricing(Params{{"pricing_date", 0.0}, {"n_paths", 1.0}, {"n_steps", 1.0}, {"seed", 1.0}});
    ExecutionContext execution(Params{{"backend", std::string("cpu")}, {"precision", std::string("fp64")}});

    engine::MeasureResult legacy = measure->evaluate(*model, *product, market, pricing, execution);
    ASSERT_TRUE(legacy.has_scalar);

    // ---- AST generico: mismo swap como Both(pata flotante, Give(pata fija)), replicado con
    // DiscountFactor y pagado como un unico Cashflow a t=0 (ADR-P0-02, SS9.3). ----
    pf::CurveId curve{"IR.DF.OIS"};
    pf::MarketPath path;
    path.set_discount_factor(curve, pf::TimePoint{0.0}, pf::TimePoint{start}, market.discount_factor(start));
    for (double t : payment_times) {
        path.set_discount_factor(curve, pf::TimePoint{0.0}, pf::TimePoint{t}, market.discount_factor(t));
    }

    pf::ScalarExprPtr floating_leg = pf::mul(
        pf::constant(notional),
        pf::sub(
            pf::discount_factor(curve, pf::TimePoint{0.0}, pf::TimePoint{start}),
            pf::discount_factor(curve, pf::TimePoint{0.0}, pf::TimePoint{payment_times.back()})
        )
    );

    pf::ScalarExprPtr fixed_leg_sum = pf::constant(0.0);
    for (std::size_t i = 0; i < payment_times.size(); ++i) {
        pf::ScalarExprPtr term = pf::mul(
            pf::constant(fixed_rate * accruals[i]), pf::discount_factor(curve, pf::TimePoint{0.0}, pf::TimePoint{payment_times[i]})
        );
        fixed_leg_sum = pf::add(fixed_leg_sum, term);
    }
    pf::ScalarExprPtr fixed_leg = pf::mul(pf::constant(notional), fixed_leg_sum);

    pf::ContractPtr swap_ast = pf::when(
        pf::TimePoint{0.0},
        pf::both({
            pf::cashflow(pf::Currency{"USD"}, floating_leg),
            pf::give(pf::cashflow(pf::Currency{"USD"}, fixed_leg)),
        })
    );

    // El swap ya paga un unico Cashflow en t=0 (el descuento va incorporado via DiscountFactor
    // dentro del propio AST) -- discount_and_convert() solo necesita un factor identidad en
    // (0,0) para esa moneda/curva (PLAN_PRODUCTS.md §12 Fase 4).
    path.set_discount_factor(curve, pf::TimePoint{0.0}, pf::TimePoint{0.0}, 1.0);

    pf::FixingStore historical;
    pf::RuntimeState state;
    pf::EvaluationContext context{path, historical, state};

    pf::DiscountingPolicy discounting(pf::Currency{"USD"}, curve);
    pf::ValuationResult ast_result = pf::present_value(swap_ast, context, discounting, pf::Currency{"USD"});

    EXPECT_NEAR(ast_result.present_value, legacy.scalar, 1e-6);
    EXPECT_EQ(ast_result.measure, pf::ProbabilityMeasure::DeterministicScenario);
}

} // namespace
