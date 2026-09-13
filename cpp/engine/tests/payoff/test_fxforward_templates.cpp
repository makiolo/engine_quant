// Equivalencia formula de referencia vs AST para FXForward via template reutilizable
// (PLAN_PRODUCTS.md §12 Fase 8, §9.4): generaliza el caso construido a mano en
// `test_measures.cpp::FxForwardMultiCurrency` usando `templates::fx_forward`, y compara contra
// la formula cerrada de PLAN_FXFORWARD.md §2.2 (documento de referencia, nunca implementado como
// `FxForwardProduct` -- PLAN_PRODUCTS.md §9.4 usa el AST precisamente para no necesitar esa
// clase nominal).

#include <gtest/gtest.h>

#include "engine/payoff/evaluation_context.hpp"
#include "engine/payoff/event_state.hpp"
#include "engine/payoff/fxforward_templates.hpp"
#include "engine/payoff/market_path.hpp"
#include "engine/payoff/measures.hpp"

namespace {

namespace pf = engine::payoff;
namespace tmpl = engine::payoff::templates;

pf::TimePoint tp(double t) { return pf::TimePoint{t}; }

struct FxForwardFixture {
    pf::Currency eur{"EUR"};
    pf::Currency usd{"USD"};
    pf::CurveId eur_curve{"IR.DF.EUR.OIS"};
    pf::CurveId usd_curve{"IR.DF.USD.OIS"};

    double notional_foreign = 1'000'000.0;
    double forward_rate = 1.10; // USD por EUR
    double maturity = 1.0;

    double spot = 1.12; // S: USD por EUR
    double df_eur = 0.99; // DF_for_eff(T), sin basis
    double df_usd = 0.97; // DF_dom(T)

    pf::MarketPath market_path() const {
        pf::MarketPath path;
        path.set_discount_factor(eur_curve, tp(0.0), tp(maturity), df_eur);
        path.set_discount_factor(usd_curve, tp(0.0), tp(maturity), df_usd);
        path.set_fx_rate(eur, usd, tp(maturity), spot);
        return path;
    }

    pf::DiscountingPolicy discounting() const {
        pf::DiscountingPolicy d;
        d.set_curve(eur, eur_curve);
        d.set_curve(usd, usd_curve);
        return d;
    }

    // PLAN_FXFORWARD.md §2.2: PV = sign * [N_for * S * DF_for_eff(T) - N_for * K * DF_dom(T)].
    double closed_form_pv(bool buy_foreign) const {
        double sign = buy_foreign ? 1.0 : -1.0;
        return sign * (notional_foreign * spot * df_eur - notional_foreign * forward_rate * df_usd);
    }

    pf::ContractPtr ast(bool buy_foreign) const {
        tmpl::FxForwardSpec spec{eur, usd, notional_foreign, forward_rate, tp(maturity), buy_foreign};
        return tmpl::fx_forward(spec);
    }
};

TEST(FxForwardTemplateTest, PresentValueMatchesClosedFormBuyForeign) {
    FxForwardFixture fx;
    pf::MarketPath path = fx.market_path();
    pf::FixingStore historical;
    pf::RuntimeState state;
    pf::EvaluationContext context{path, historical, state};

    pf::ValuationResult result = pf::present_value(fx.ast(/*buy_foreign=*/true), context, fx.discounting(), fx.usd);

    EXPECT_NEAR(result.present_value, fx.closed_form_pv(/*buy_foreign=*/true), 1e-6);
    EXPECT_EQ(result.dependencies.currencies.count(fx.eur), 1u);
    EXPECT_EQ(result.dependencies.currencies.count(fx.usd), 1u);
}

TEST(FxForwardTemplateTest, PresentValueMatchesClosedFormSellForeign) {
    FxForwardFixture fx;
    pf::MarketPath path = fx.market_path();
    pf::FixingStore historical;
    pf::RuntimeState state;
    pf::EvaluationContext context{path, historical, state};

    pf::ValuationResult result = pf::present_value(fx.ast(/*buy_foreign=*/false), context, fx.discounting(), fx.usd);

    EXPECT_NEAR(result.present_value, fx.closed_form_pv(/*buy_foreign=*/false), 1e-6);
}

// Forward "a la par" (PLAN_FXFORWARD.md §2.2: K_par = F_mkt(T) = S*DF_for_eff(T)/DF_dom(T)) ->
// PV ~ 0, mismo criterio de sanity que un IRS a la par.
TEST(FxForwardTemplateTest, AtParForwardRateGivesZeroPresentValue) {
    FxForwardFixture fx;
    fx.forward_rate = fx.spot * fx.df_eur / fx.df_usd;
    pf::MarketPath path = fx.market_path();
    pf::FixingStore historical;
    pf::RuntimeState state;
    pf::EvaluationContext context{path, historical, state};

    pf::ValuationResult result = pf::present_value(fx.ast(/*buy_foreign=*/true), context, fx.discounting(), fx.usd);

    EXPECT_NEAR(result.present_value, 0.0, 1e-6);
}

} // namespace
