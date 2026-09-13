// Precio bajo Q via Longstaff-Schwartz (GBM) de un PayoffProgram con Exercise (PLAN_PRODUCTS.md
// §10, §12 Fase 9): engine::payoff::exercise_price_gbm, preflight de ModelCapabilities y los
// criterios de aceptacion explicitos de la fase (americana >= europea para put sin dividendos,
// convergencia por fechas) contra la ruta europea ya existente (risk_neutral_price_gbm sobre un
// AST vanilla, Fase 5) -- mismo espiritu que test_gbm_measures.cpp: ejercita el camino completo
// C++ AST -> JSON -> Rust (compile/lsm/eval) -> QValuationResult, no solo el lado Rust (ya
// cubierto en rust/crates/engine-core/src/payoff/api.rs).

#include <gtest/gtest.h>

#include <vector>

#include "engine/model.hpp"
#include "engine/payoff/errors.hpp"
#include "engine/payoff/expression.hpp"
#include "engine/payoff/measures.hpp"
#include "engine/payoff/payoff_product.hpp"
#include "engine/payoff/predicate.hpp"

namespace {

namespace pf = engine::payoff;

pf::TimePoint tp(double t) { return pf::TimePoint{t}; }

engine::GbmModel make_gbm(double s0, double r, double q, double sigma, const std::string& observable) {
    return engine::GbmModel(engine::Params{
        {"s0", s0}, {"r", r}, {"q", q}, {"sigma", sigma}, {"observable", observable}
    });
}

pf::ContractPtr european_payoff(const pf::ObservableId& spot, const char* kind, double strike, double maturity) {
    pf::ScalarExprPtr intrinsic =
        kind == std::string("call")
            ? pf::maximum(pf::sub(pf::fixing(spot, tp(maturity)), pf::constant(strike)), pf::constant(0.0))
            : pf::maximum(pf::sub(pf::constant(strike), pf::fixing(spot, tp(maturity))), pf::constant(0.0));
    return pf::when(tp(maturity), pf::cashflow(pf::Currency{"USD"}, intrinsic));
}

// Derecho de ejercicio en `dates` (estrictamente anteriores a `maturity`) mas la continuacion
// europea de `european_payoff` en `maturity` -- misma construccion que
// rust/crates/engine-core/src/payoff/api.rs::bermuda_contract_json, aqui armada con el AST C++.
pf::ContractPtr bermuda(
    const pf::ObservableId& spot, const char* kind, double strike, double maturity,
    const std::vector<pf::TimePoint>& dates
) {
    pf::ScalarExprPtr exercise_value =
        kind == std::string("call")
            ? pf::maximum(pf::sub(pf::current(spot), pf::constant(strike)), pf::constant(0.0))
            : pf::maximum(pf::sub(pf::constant(strike), pf::current(spot)), pf::constant(0.0));
    return pf::exercise(pf::EventId{"EX"}, dates, exercise_value, european_payoff(spot, kind, strike, maturity));
}

TEST(ExerciseGbmMeasureTest, BermudaPutPriceIsAtLeastTheEuropeanPutPriceWithoutDividends) {
    const pf::ObservableId spot{"EQ.SPOT.XYZ"};
    const double s0 = 100.0, strike = 100.0, r = 0.05, q = 0.0, sigma = 0.2, maturity = 1.0;
    engine::GbmModel model = make_gbm(s0, r, q, sigma, spot.value);

    pf::PayoffProduct european_product("EUROPEAN_PUT", european_payoff(spot, "put", strike, maturity));
    pf::PayoffProduct bermuda_product(
        "BERMUDA_PUT", bermuda(spot, "put", strike, maturity, {tp(0.25), tp(0.5), tp(0.75)})
    );

    pf::QValuationResult european =
        pf::risk_neutral_price_gbm(*european_product.payoff_program(), model, 200'000, 7);
    pf::ExercisePolicyResult bermuda_result =
        pf::exercise_price_gbm(*bermuda_product.payoff_program(), model, 200'000, 8);

    double tolerance = 8.0 * (european.std_error + bermuda_result.price.std_error);
    EXPECT_GE(bermuda_result.price.mean + tolerance, european.mean)
        << "bermuda=" << bermuda_result.price.mean << " europea=" << european.mean;
    EXPECT_EQ(bermuda_result.price.measure, pf::ProbabilityMeasure::RiskNeutralQ);
    EXPECT_EQ(bermuda_result.dates.size(), 3u);
}

TEST(ExerciseGbmMeasureTest, BermudaCallPriceMatchesEuropeanCallPriceWithoutDividends) {
    // Sin dividendos, nunca es optimo ejercer una call americana anticipadamente -- el precio
    // bermuda/americano debe coincidir con el europeo dentro de tolerancia estadistica.
    const pf::ObservableId spot{"EQ.SPOT.XYZ"};
    const double s0 = 100.0, strike = 100.0, r = 0.05, q = 0.0, sigma = 0.2, maturity = 1.0;
    engine::GbmModel model = make_gbm(s0, r, q, sigma, spot.value);

    pf::PayoffProduct european_product("EUROPEAN_CALL", european_payoff(spot, "call", strike, maturity));
    std::vector<pf::TimePoint> dates;
    for (int i = 1; i <= 9; ++i) dates.push_back(tp(0.1 * i));
    pf::PayoffProduct bermuda_product("BERMUDA_CALL", bermuda(spot, "call", strike, maturity, dates));

    pf::QValuationResult european =
        pf::risk_neutral_price_gbm(*european_product.payoff_program(), model, 200'000, 29);
    pf::ExercisePolicyResult bermuda_result =
        pf::exercise_price_gbm(*bermuda_product.payoff_program(), model, 200'000, 30);

    double tolerance = 8.0 * (european.std_error + bermuda_result.price.std_error);
    EXPECT_NEAR(bermuda_result.price.mean, european.mean, tolerance);
}

TEST(ExerciseGbmMeasureTest, PreflightRejectsObservableTheModelDoesNotGenerate) {
    const pf::ObservableId spot{"EQ.SPOT.XYZ"};
    pf::PayoffProduct product("BERMUDA_PUT", bermuda(spot, "put", 100.0, 1.0, {tp(0.5)}));
    engine::GbmModel model = make_gbm(100.0, 0.05, 0.0, 0.2, "EQ.SPOT.OTHER_TICKER");

    EXPECT_THROW(pf::exercise_price_gbm(*product.payoff_program(), model, 1'000, 7), pf::ValidationError);
}

TEST(ExerciseGbmMeasureTest, PriceIsReproducibleGivenTheSameSeed) {
    // "decisiones reproducibles con seed": misma seed, mismo precio Y mismo diagnostico.
    const pf::ObservableId spot{"EQ.SPOT.XYZ"};
    engine::GbmModel model = make_gbm(100.0, 0.05, 0.0, 0.2, spot.value);
    pf::PayoffProduct product(
        "BERMUDA_PUT", bermuda(spot, "put", 100.0, 1.0, {tp(0.25), tp(0.5), tp(0.75)})
    );

    pf::ExercisePolicyResult first = pf::exercise_price_gbm(*product.payoff_program(), model, 20'000, 41);
    pf::ExercisePolicyResult second = pf::exercise_price_gbm(*product.payoff_program(), model, 20'000, 41);

    EXPECT_DOUBLE_EQ(first.price.mean, second.price.mean);
    ASSERT_EQ(first.dates.size(), second.dates.size());
    for (std::size_t i = 0; i < first.dates.size(); ++i) {
        EXPECT_DOUBLE_EQ(first.dates[i].date, second.dates[i].date);
        EXPECT_EQ(first.dates[i].n_in_the_money, second.dates[i].n_in_the_money);
        EXPECT_DOUBLE_EQ(first.dates[i].exercised_fraction, second.dates[i].exercised_fraction);
    }
}

} // namespace
