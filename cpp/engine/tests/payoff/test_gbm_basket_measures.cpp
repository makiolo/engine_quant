// Precio bajo Q via Monte Carlo (GbmBasket) de un PayoffProgram multi-activo correlacionado
// (PLAN_IMPROVE_NOTEBOOK.md Fase 3): engine::payoff::risk_neutral_price_gbm(..., const
// GbmBasketModel&, ...), preflight de ModelCapabilities generalizado, y el mismo registry wiring
// de "PayoffPriceQ" (measure.cpp) que ya cubre GbmModel para N=1, ahora tambien para N=arbitrario
// (mismo nombre de medida, dispatch por dynamic_cast -- decision de diseno documentada en el
// doc-comment de PayoffPriceQMeasure::evaluate).

#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>

#include "engine/bootstrap.hpp"
#include "engine/model.hpp"
#include "engine/payoff/errors.hpp"
#include "engine/payoff/expression.hpp"
#include "engine/payoff/measures.hpp"
#include "engine/payoff/payoff_product.hpp"
#include "engine/price.hpp"

namespace {

namespace pf = engine::payoff;

using engine::ExecutionContext;
using engine::MarketSnapshot;
using engine::PricingContext;
using engine::Registries;
using engine::register_builtins;

pf::TimePoint tp(double t) { return pf::TimePoint{t}; }

PricingContext pricing_context(std::uint64_t n_paths, std::uint64_t seed) {
    return PricingContext(engine::Params{
        {"pricing_date", 0.0}, {"n_paths", static_cast<double>(n_paths)}, {"n_steps", 1.0},
        {"seed", static_cast<double>(seed)}
    });
}

ExecutionContext cpu_execution() {
    return ExecutionContext(engine::Params{{"backend", std::string("cpu")}, {"precision", std::string("fp64")}});
}

MarketSnapshot flat_market() { return MarketSnapshot({1.0}, {0.02}); }

// Basket call sobre la SUMA de dos activos: max(S_A(T) + S_B(T) - K, 0).
pf::ContractPtr basket_call(const pf::ObservableId& a, const pf::ObservableId& b, double strike, double maturity) {
    return pf::when(
        tp(maturity),
        pf::cashflow(
            pf::Currency{"USD"},
            pf::maximum(
                pf::sub(pf::add(pf::fixing(a, tp(maturity)), pf::fixing(b, tp(maturity))), pf::constant(strike)),
                pf::constant(0.0)
            )
        )
    );
}

engine::Params basket_params(
    const std::string& observables, std::vector<double> s0, std::vector<double> r, std::vector<double> q,
    std::vector<double> sigma, std::vector<double> correlation
) {
    return engine::Params{
        {"observables", observables}, {"s0", std::move(s0)}, {"r", std::move(r)}, {"q", std::move(q)},
        {"sigma", std::move(sigma)}, {"correlation", std::move(correlation)}
    };
}

TEST(GbmBasketModelTest, ConstructsAndRoundTripsToParams) {
    engine::GbmBasketModel model(
        basket_params("EQ.SPOT.A,EQ.SPOT.B", {100.0, 50.0}, {0.03, 0.03}, {0.0, 0.0}, {0.2, 0.35}, {1.0, 0.5, 0.5, 1.0})
    );
    EXPECT_EQ(model.n_assets(), 2u);
    EXPECT_EQ(model.observables()[0].value, "EQ.SPOT.A");
    EXPECT_EQ(model.observables()[1].value, "EQ.SPOT.B");

    engine::Params round_tripped = model.to_params();
    engine::GbmBasketModel reconstructed(round_tripped);
    EXPECT_EQ(reconstructed.n_assets(), model.n_assets());
    EXPECT_DOUBLE_EQ(reconstructed.s0()[1], model.s0()[1]);
    EXPECT_DOUBLE_EQ(reconstructed.correlation()[1], model.correlation()[1]);
}

TEST(GbmBasketModelTest, RejectsMismatchedVectorLengths) {
    EXPECT_THROW(
        engine::GbmBasketModel(
            basket_params("EQ.SPOT.A,EQ.SPOT.B", {100.0, 50.0}, {0.03}, {0.0, 0.0}, {0.2, 0.35}, {1.0, 0.5, 0.5, 1.0})
        ),
        std::invalid_argument
    );
}

TEST(GbmBasketModelTest, RejectsNonSquareCorrelation) {
    EXPECT_THROW(
        engine::GbmBasketModel(
            basket_params("EQ.SPOT.A,EQ.SPOT.B", {100.0, 50.0}, {0.03, 0.03}, {0.0, 0.0}, {0.2, 0.35}, {1.0, 0.5})
        ),
        std::invalid_argument
    );
}

TEST(RiskNeutralGbmBasketMeasureTest, PricesABasketCallAndRejectsAnUnknownObservable) {
    const pf::ObservableId a{"EQ.SPOT.A"}, b{"EQ.SPOT.B"}, other{"EQ.SPOT.OTHER"};
    const double s0 = 100.0, r = 0.03, q = 0.0, sigma = 0.2, maturity = 1.0;

    engine::GbmBasketModel model(
        basket_params("EQ.SPOT.A,EQ.SPOT.B", {s0, s0}, {r, r}, {q, q}, {sigma, sigma}, {1.0, 0.4, 0.4, 1.0})
    );

    pf::PayoffProduct product("BASKET_CALL", basket_call(a, b, 180.0, maturity));
    pf::QValuationResult result = pf::risk_neutral_price_gbm(*product.payoff_program(), model, 100'000, 11);
    EXPECT_EQ(result.measure, pf::ProbabilityMeasure::RiskNeutralQ);
    EXPECT_GT(result.mean, 0.0);
    EXPECT_GT(result.std_error, 0.0);

    // El contrato referencia un observable ('EQ.SPOT.OTHER') que el basket no declara -- rechazo
    // explicito en preflight, misma disciplina que la sobrecarga de GbmModel.
    pf::PayoffProduct bad_product("BAD", basket_call(a, other, 180.0, maturity));
    EXPECT_THROW(pf::risk_neutral_price_gbm(*bad_product.payoff_program(), model, 1'000, 11), pf::ValidationError);
}

TEST(RiskNeutralGbmBasketMeasureTest, BasketCallPriceIncreasesWithCorrelation) {
    // Criterio de aceptacion explicito de PLAN_IMPROVE_NOTEBOOK.md Fase 3: subir la correlacion
    // entre dos activos con la misma vol sube el precio de una basket CALL sobre la suma (mas
    // correlacion => mas varianza de la suma => call mas cara).
    const pf::ObservableId a{"EQ.SPOT.A"}, b{"EQ.SPOT.B"};
    const double s0 = 100.0, r = 0.03, q = 0.0, sigma = 0.25, maturity = 1.0;
    pf::PayoffProduct product("BASKET_CALL", basket_call(a, b, 200.0, maturity));
    const std::uint64_t n_paths = 150'000, seed = 5;

    engine::GbmBasketModel low_corr(
        basket_params("EQ.SPOT.A,EQ.SPOT.B", {s0, s0}, {r, r}, {q, q}, {sigma, sigma}, {1.0, 0.0, 0.0, 1.0})
    );
    engine::GbmBasketModel high_corr(
        basket_params("EQ.SPOT.A,EQ.SPOT.B", {s0, s0}, {r, r}, {q, q}, {sigma, sigma}, {1.0, 0.9, 0.9, 1.0})
    );

    pf::QValuationResult low_result = pf::risk_neutral_price_gbm(*product.payoff_program(), low_corr, n_paths, seed);
    pf::QValuationResult high_result =
        pf::risk_neutral_price_gbm(*product.payoff_program(), high_corr, n_paths, seed);

    EXPECT_GT(high_result.mean, low_result.mean)
        << "high_corr=" << high_result.mean << " low_corr=" << low_result.mean;
}

// PLAN_IMPROVE_NOTEBOOK.md Fase 3 §2 punto 3: "PayoffPriceQ" (mismo nombre de medida) dispatch-ea
// a GbmBasketModel via Registry<IMeasure>, exactamente el mismo camino que ya usan los notebooks
// para GbmModel.
TEST(RiskNeutralGbmBasketMeasureTest, PayoffPriceQViaRegistryMatchesDirectCallToRiskNeutralPriceGbmBasket) {
    Registries registries;
    register_builtins(registries);
    const pf::ObservableId a{"EQ.SPOT.A"}, b{"EQ.SPOT.B"};
    const double s0 = 100.0, r = 0.03, q = 0.0, sigma = 0.2, maturity = 1.0;

    engine::GbmBasketModel model(
        basket_params("EQ.SPOT.A,EQ.SPOT.B", {s0, s0}, {r, r}, {q, q}, {sigma, sigma}, {1.0, 0.3, 0.3, 1.0})
    );
    pf::PayoffProduct product("BASKET_CALL", basket_call(a, b, 180.0, maturity));
    auto measure = registries.measures.create("PayoffPriceQ");

    engine::MeasureResult via_registry =
        measure->evaluate(model, product, flat_market(), pricing_context(100'000, 13), cpu_execution());
    pf::QValuationResult direct = pf::risk_neutral_price_gbm(*product.payoff_program(), model, 100'000, 13);

    ASSERT_TRUE(via_registry.has_scalar);
    EXPECT_DOUBLE_EQ(via_registry.scalar, direct.mean);
}

} // namespace
