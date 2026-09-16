// PLAN_BACKWARD.md §9 Fase 6: `engine::Portfolio` -- objeto first-class de orquestacion (lista de
// trades bajo un UNICO IModel/MarketSnapshot). Aceptacion EXACTA (§13 DoD, repetida aqui): para
// una cartera HOMOGENEA de 2-3 trades bajo el MISMO modelo, `Portfolio::hessian()`/`hvp()` deben
// coincidir EXACTAMENTE (no con tolerancia estadistica -- solo redondeo de punto flotante,
// ~1e-9) con sumar A MANO los `HessianReport`/`HvpReport` de `compute_hessian`/`compute_hvp`
// llamados trade a trade; `Portfolio::price()` debe coincidir con `price_many` sobre el mismo
// vector de trades.

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "engine/bootstrap.hpp"
#include "engine/greeks.hpp"
#include "engine/model.hpp"
#include "engine/payoff/expression.hpp"
#include "engine/payoff/payoff_product.hpp"
#include "engine/portfolio.hpp"
#include "engine/price.hpp"
#include "engine/product.hpp"

namespace {

namespace pf = engine::payoff;

using engine::ExecutionContext;
using engine::MarketSnapshot;
using engine::Params;
using engine::Portfolio;
using engine::PricingContext;
using engine::Registries;
using engine::register_builtins;

engine::HullWhite1FModel make_hull_white() {
    return engine::HullWhite1FModel(Params{{"a", 0.1}, {"b", 0.03}, {"sigma", 0.01}, {"r0", 0.02}});
}

PricingContext pricing_context(std::uint64_t n_paths, std::uint64_t seed) {
    return PricingContext(Params{
        {"pricing_date", 0.0}, {"n_paths", static_cast<double>(n_paths)}, {"n_steps", 1.0}, {"seed", static_cast<double>(seed)}
    });
}

ExecutionContext cpu_execution() {
    return ExecutionContext(Params{{"backend", std::string("cpu")}, {"precision", std::string("fp64")}});
}

MarketSnapshot flat_market() { return MarketSnapshot({1.0}, {0.02}); }

std::shared_ptr<engine::IrSwapProduct> make_irs(double notional, double fixed_rate) {
    return std::make_shared<engine::IrSwapProduct>(Params{
        {"notional", notional},
        {"fixed_rate", fixed_rate},
        {"payment_times", std::vector<double>{1.0, 2.0, 3.0}},
        {"accruals", std::vector<double>{1.0, 1.0, 1.0}},
    });
}

// GBM: usado para el trade "homogeneo" via likelihood ratio (Fase 1), como oraculo alternativo
// al de Hull-White -- mismo patron que european_call/make_gbm_q en test_greeks.cpp.
pf::TimePoint tp(double t) { return pf::TimePoint{t}; }

pf::ContractPtr european_call(const pf::ObservableId& spot, double strike, double maturity) {
    return pf::when(
        tp(maturity),
        pf::cashflow(pf::Currency{"USD"}, pf::maximum(pf::sub(pf::fixing(spot, tp(maturity)), pf::constant(strike)), pf::constant(0.0)))
    );
}

engine::GbmModel make_gbm_q(const std::string& observable) {
    return engine::GbmModel(Params{{"s0", 100.0}, {"r", 0.05}, {"q", 0.0}, {"sigma", 0.2}, {"observable", observable}});
}

} // namespace

TEST(PortfolioTest, EmptyPortfolioHasZeroSizeAndEmptyResults) {
    Registries registries;
    register_builtins(registries);
    Portfolio portfolio;
    EXPECT_EQ(portfolio.size(), 0u);
    EXPECT_TRUE(portfolio.trades().empty());

    engine::HullWhite1FModel model = make_hull_white();
    // price() es un envoltorio fino sobre price_many YA EXISTENTE (sin logica propia): hereda su
    // comportamiento tal cual, incluido que una lista de trades vacia es un error explicito, no
    // un PriceBatchResult vacio silencioso.
    EXPECT_THROW(
        portfolio.price(registries, {"PV"}, model, flat_market(), pricing_context(1'000, 7), cpu_execution()),
        std::invalid_argument
    );

    engine::greeks::HessianReport hessian_report = portfolio.hessian(
        registries, "HullWhiteModelNpv", Params{}, model, flat_market(), pricing_context(1'000, 7), cpu_execution()
    );
    EXPECT_TRUE(hessian_report.entries.empty());
    EXPECT_TRUE(hessian_report.skipped.empty());
}

TEST(PortfolioTest, AddIncreasesSizeAndTradesIsAccessible) {
    Portfolio portfolio;
    portfolio.add(make_irs(1'000'000.0, 0.02));
    portfolio.add(make_irs(2'000'000.0, 0.025));
    EXPECT_EQ(portfolio.size(), 2u);
    ASSERT_EQ(portfolio.trades().size(), 2u);
    EXPECT_EQ(portfolio.trades()[0]->type_name(), "IRSwap");
}

// `Portfolio::price()` es un envoltorio fino sobre `engine::price_many` YA EXISTENTE: debe
// coincidir EXACTAMENTE (misma ruta de codigo, ni siquiera hay margen de redondeo distinto) con
// llamar a `price_many` a mano sobre el mismo vector de punteros.
TEST(PortfolioTest, PriceMatchesPriceManyOnTheSameVectorOfTrades) {
    Registries registries;
    register_builtins(registries);

    std::shared_ptr<engine::IrSwapProduct> swap_a = make_irs(1'000'000.0, 0.02);
    std::shared_ptr<engine::IrSwapProduct> swap_b = make_irs(2'000'000.0, 0.025);
    std::shared_ptr<engine::IrSwapProduct> swap_c = make_irs(500'000.0, 0.018);

    Portfolio portfolio;
    portfolio.add(swap_a);
    portfolio.add(swap_b);
    portfolio.add(swap_c);

    engine::HullWhite1FModel model = make_hull_white();
    MarketSnapshot market = flat_market();
    PricingContext pricing = pricing_context(1'000, 7);
    ExecutionContext execution = cpu_execution();

    engine::PriceBatchResult from_portfolio = portfolio.price(registries, {"PV", "DV01"}, model, market, pricing, execution);

    std::vector<const engine::IProduct*> raw = {swap_a.get(), swap_b.get(), swap_c.get()};
    engine::PriceBatchResult from_price_many =
        engine::price_many(registries, raw, std::vector<std::string>{"PV", "DV01"}, model, market, pricing, execution);

    ASSERT_EQ(from_portfolio.size(), from_price_many.size());
    for (std::size_t i = 0; i < from_portfolio.size(); ++i) {
        EXPECT_EQ(from_portfolio[i].trade_index, from_price_many[i].trade_index);
        ASSERT_EQ(from_portfolio[i].measures.size(), from_price_many[i].measures.size());
        for (std::size_t j = 0; j < from_portfolio[i].measures.size(); ++j) {
            EXPECT_EQ(from_portfolio[i].measures[j].measure_name, from_price_many[i].measures[j].measure_name);
            EXPECT_DOUBLE_EQ(from_portfolio[i].measures[j].result.scalar, from_price_many[i].measures[j].result.scalar);
        }
    }
}

// Aceptacion EXACTA del plan (§13 DoD): Portfolio::hessian() de 3 IrSwapProduct bajo el MISMO
// HullWhite1FModel coincide, entrada a entrada, con sumar A MANO los HessianReport de
// compute_hessian llamado trade a trade -- identidad exacta (tolerancia solo de redondeo, 1e-9).
TEST(PortfolioTest, HessianOfThreeHomogeneousSwapsMatchesManualSumOfPerTradeHessianExactly) {
    Registries registries;
    register_builtins(registries);

    std::shared_ptr<engine::IrSwapProduct> swap_a = make_irs(1'000'000.0, 0.02);
    std::shared_ptr<engine::IrSwapProduct> swap_b = make_irs(2'000'000.0, 0.025);
    std::shared_ptr<engine::IrSwapProduct> swap_c = make_irs(500'000.0, 0.018);

    Portfolio portfolio;
    portfolio.add(swap_a);
    portfolio.add(swap_b);
    portfolio.add(swap_c);

    engine::HullWhite1FModel model = make_hull_white();
    MarketSnapshot market = flat_market();
    PricingContext pricing = pricing_context(1'000, 7);
    ExecutionContext execution = cpu_execution();

    engine::greeks::HessianReport portfolio_report =
        portfolio.hessian(registries, "HullWhiteModelNpv", Params{}, model, market, pricing, execution);

    // Oraculo independiente: compute_hessian llamado a mano, trade a trade, sumado manualmente.
    std::vector<engine::IrSwapProduct*> swaps = {swap_a.get(), swap_b.get(), swap_c.get()};
    std::vector<engine::greeks::HessianReport> manual_per_trade;
    for (engine::IrSwapProduct* swap : swaps) {
        manual_per_trade.push_back(
            engine::greeks::compute_hessian(registries, "HullWhiteModelNpv", Params{}, model, *swap, market, pricing, execution)
        );
    }

    ASSERT_TRUE(portfolio_report.skipped.empty()) << (portfolio_report.skipped.empty() ? "" : portfolio_report.skipped.front());
    for (const auto& rep : manual_per_trade) ASSERT_TRUE(rep.skipped.empty());

    // Los tres swaps, bajo el mismo modelo Hull-White 1F, producen exactamente 10 pares (4
    // parametros -> 4+3+2+1). Cartera homogenea: Portfolio::hessian() no debe perder ninguno.
    ASSERT_EQ(manual_per_trade[0].entries.size(), 10u);
    ASSERT_EQ(portfolio_report.entries.size(), 10u);

    auto find_manual_sum = [&](const engine::greeks::RiskFactor& fi, const engine::greeks::RiskFactor& fj) {
        double sum = 0.0;
        for (const auto& rep : manual_per_trade) {
            bool found = false;
            for (const auto& entry : rep.entries) {
                bool same_ij = (entry.factor_i.name == fi.name && entry.factor_j.name == fj.name) ||
                                (entry.factor_i.name == fj.name && entry.factor_j.name == fi.name);
                if (same_ij) {
                    sum += entry.value;
                    found = true;
                    break;
                }
            }
            EXPECT_TRUE(found) << "par no encontrado en un trade individual: " << fi.name << "/" << fj.name;
        }
        return sum;
    };

    for (const engine::greeks::HessianEntry& entry : portfolio_report.entries) {
        double manual_sum = find_manual_sum(entry.factor_i, entry.factor_j);
        EXPECT_NEAR(entry.value, manual_sum, 1e-9 * std::max(1.0, std::abs(manual_sum)))
            << "par " << entry.factor_i.name << "/" << entry.factor_j.name;
        // Hull-White (forward-over-forward) es formula cerrada: nunca lleva std_error.
        EXPECT_FALSE(entry.std_error.has_value());
        EXPECT_EQ(entry.method_used, engine::greeks::GreekMethod::AadForwardOverForward);
    }
}

// Mismo criterio de aceptacion, para `Portfolio::hvp()`: coincide exactamente con sumar a mano
// los HvpComponent::value de compute_hvp llamado trade a trade.
TEST(PortfolioTest, HvpOfThreeHomogeneousSwapsMatchesManualSumOfPerTradeHvpExactly) {
    Registries registries;
    register_builtins(registries);

    std::shared_ptr<engine::IrSwapProduct> swap_a = make_irs(1'000'000.0, 0.02);
    std::shared_ptr<engine::IrSwapProduct> swap_b = make_irs(2'000'000.0, 0.025);
    std::shared_ptr<engine::IrSwapProduct> swap_c = make_irs(500'000.0, 0.018);

    Portfolio portfolio;
    portfolio.add(swap_a);
    portfolio.add(swap_b);
    portfolio.add(swap_c);

    engine::HullWhite1FModel model = make_hull_white();
    MarketSnapshot market = flat_market();
    PricingContext pricing = pricing_context(1'000, 7);
    ExecutionContext execution = cpu_execution();

    using engine::greeks::RiskFactor;
    using engine::greeks::RiskFactorKind;
    std::vector<RiskFactor> factors = {
        RiskFactor{RiskFactorKind::ModelParameter, "model", "a", std::nullopt},
        RiskFactor{RiskFactorKind::ModelParameter, "model", "b", std::nullopt},
        RiskFactor{RiskFactorKind::ModelParameter, "model", "sigma", std::nullopt},
        RiskFactor{RiskFactorKind::ModelParameter, "model", "r0", std::nullopt},
    };
    std::vector<double> direction = {1.0, 0.5, -0.25, 2.0};

    engine::greeks::HvpReport portfolio_hvp =
        portfolio.hvp(registries, "HullWhiteModelNpv", Params{}, model, market, pricing, execution, factors, direction);

    std::vector<engine::IrSwapProduct*> swaps = {swap_a.get(), swap_b.get(), swap_c.get()};
    std::vector<engine::greeks::HvpReport> manual_per_trade;
    for (engine::IrSwapProduct* swap : swaps) {
        manual_per_trade.push_back(engine::greeks::compute_hvp(
            registries, "HullWhiteModelNpv", Params{}, model, *swap, market, pricing, execution, factors, direction
        ));
    }

    ASSERT_TRUE(portfolio_hvp.skipped.empty());
    ASSERT_EQ(portfolio_hvp.components.size(), factors.size());

    for (const engine::greeks::HvpComponent& component : portfolio_hvp.components) {
        double manual_sum = 0.0;
        for (const auto& rep : manual_per_trade) {
            bool found = false;
            for (const auto& c : rep.components) {
                if (c.factor.name == component.factor.name) {
                    manual_sum += c.value;
                    found = true;
                    break;
                }
            }
            EXPECT_TRUE(found) << "factor no encontrado en un trade individual: " << component.factor.name;
        }
        EXPECT_NEAR(component.value, manual_sum, 1e-9 * std::max(1.0, std::abs(manual_sum))) << component.factor.name;
    }
}

// Fase 1 (likelihood ratio, GBM): mismo criterio de aceptacion sobre una combinacion distinta de
// (modelo,metrica), para no dejar el test de aceptacion atado solo a Hull-White.
TEST(PortfolioTest, HessianOfTwoHomogeneousGbmPayoffsMatchesManualSumExactly) {
    Registries registries;
    register_builtins(registries);

    const pf::ObservableId spot{"EQ.SPOT.XYZ"};
    std::shared_ptr<engine::IProduct> call_a = std::make_shared<pf::PayoffProduct>("CALL_100", european_call(spot, 100.0, 1.0));
    std::shared_ptr<engine::IProduct> call_b = std::make_shared<pf::PayoffProduct>("CALL_90", european_call(spot, 90.0, 1.0));

    Portfolio portfolio;
    portfolio.add(call_a);
    portfolio.add(call_b);

    engine::GbmModel model = make_gbm_q(spot.value);
    MarketSnapshot market = flat_market();
    PricingContext pricing = pricing_context(20'000, 11);
    ExecutionContext execution = cpu_execution();

    engine::greeks::HessianReport portfolio_report =
        portfolio.hessian(registries, "PayoffPriceQ", Params{}, model, market, pricing, execution);

    engine::greeks::HessianReport manual_a =
        engine::greeks::compute_hessian(registries, "PayoffPriceQ", Params{}, model, *call_a, market, pricing, execution);
    engine::greeks::HessianReport manual_b =
        engine::greeks::compute_hessian(registries, "PayoffPriceQ", Params{}, model, *call_b, market, pricing, execution);

    ASSERT_TRUE(portfolio_report.skipped.empty());
    ASSERT_TRUE(manual_a.skipped.empty());
    ASSERT_TRUE(manual_b.skipped.empty());
    ASSERT_EQ(manual_a.entries.size(), 3u); // {spot,volatility} -> gamma,volga,vanna
    ASSERT_EQ(portfolio_report.entries.size(), 3u);

    for (const auto& entry : portfolio_report.entries) {
        double manual_sum = 0.0;
        for (const auto* rep : {&manual_a, &manual_b}) {
            bool found = false;
            for (const auto& e : rep->entries) {
                bool same_ij = (e.factor_i.name == entry.factor_i.name && e.factor_j.name == entry.factor_j.name) ||
                                (e.factor_i.name == entry.factor_j.name && e.factor_j.name == entry.factor_i.name);
                if (same_ij) {
                    manual_sum += e.value;
                    found = true;
                    break;
                }
            }
            EXPECT_TRUE(found);
        }
        // Suma exacta de floats ya calculados -- misma tolerancia que el resto de este fichero.
        EXPECT_NEAR(entry.value, manual_sum, 1e-9 * std::max(1.0, std::abs(manual_sum)));
        EXPECT_EQ(entry.method_used, engine::greeks::GreekMethod::LikelihoodRatioHessian);
        // GBM/likelihood ratio SIEMPRE lleva std_error (Monte Carlo) -- la suma en cuadratura
        // debe estar presente y coincidir con sqrt(sum of squares) de los dos trades.
        ASSERT_TRUE(entry.std_error.has_value());
        double expected_variance = 0.0;
        for (const auto* rep : {&manual_a, &manual_b}) {
            for (const auto& e : rep->entries) {
                bool same_ij = (e.factor_i.name == entry.factor_i.name && e.factor_j.name == entry.factor_j.name) ||
                                (e.factor_i.name == entry.factor_j.name && e.factor_j.name == entry.factor_i.name);
                if (same_ij) {
                    ASSERT_TRUE(e.std_error.has_value());
                    expected_variance += (*e.std_error) * (*e.std_error);
                    break;
                }
            }
        }
        EXPECT_NEAR(*entry.std_error, std::sqrt(expected_variance), 1e-9);
    }
}
