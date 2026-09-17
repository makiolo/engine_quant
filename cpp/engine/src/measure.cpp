#include "engine/measure.hpp"

#include <cmath>
#include <stdexcept>
#include <utility>

#include "engine/engine.hpp"
#include "engine/payoff/market_snapshot_bridge.hpp"
#include "engine/payoff/measures.hpp"
#include "engine/payoff/payoff_product.hpp"

namespace engine {

namespace {

// Fechas de reseteo del IRS -- start() más todos los payment_times() salvo el último (el
// swap no vuelve a resetear después de su pago final) -- misma definición que
// `IrSwap::is_reset_date` en Rust (`rust/crates/engine-core/src/products/irs.rs`). Sustituye
// el "monitoring_times" que antes llegaba en measure_params (PLAN.md §7.15).
std::vector<double> reset_dates(const IrSwapProduct& product) {
    std::vector<double> dates;
    const std::vector<double>& payment_times = product.payment_times();
    dates.reserve(payment_times.size());
    dates.push_back(product.start());
    for (std::size_t i = 0; i + 1 < payment_times.size(); ++i) {
        dates.push_back(payment_times[i]);
    }
    return dates;
}

// Interfaz homogénea entre modelos (PLAN.md §7.16): cada medida solo necesita saber, por
// modelo concreto, cómo llamar al core Rust -- el resto de cada Measure::evaluate es idéntico
// sea cual sea el modelo. Estas cuatro funciones son el único sitio de esta capa que conoce la
// existencia de HullWhite1FModel/HullWhite2FModel a la vez; añadir un tercer modelo de tipo
// corto implica una rama más aquí, nada más en el resto del archivo.

ExposureProfile compute_exposure_profile(
    const IModel& model, const IrSwapProduct& irs_product,
    const PricingContext& pricing, const ExecutionContext& execution
) {
    const std::vector<double> monitoring_times = reset_dates(irs_product);
    if (const auto* hw1 = dynamic_cast<const HullWhite1FModel*>(&model)) {
        return irs_hull_white_exposure_profile(
            execution.backend(),
            hw1->a(), hw1->b(), hw1->sigma(), hw1->r0(),
            irs_product.notional(), irs_product.fixed_rate(), irs_product.use_par_rate(),
            irs_product.start(),
            irs_product.payment_times(), irs_product.accruals(),
            monitoring_times,
            pricing.n_steps(), pricing.n_paths(), pricing.seed()
        );
    }
    if (const auto* hw2 = dynamic_cast<const HullWhite2FModel*>(&model)) {
        return irs_hull_white_2f_exposure_profile(
            execution.backend(),
            hw2->a(), hw2->b(), hw2->sigma(), hw2->eta(), hw2->rho(), hw2->r0(),
            irs_product.notional(), irs_product.fixed_rate(), irs_product.use_par_rate(),
            irs_product.start(),
            irs_product.payment_times(), irs_product.accruals(),
            monitoring_times,
            pricing.n_steps(), pricing.n_paths(), pricing.seed()
        );
    }
    throw std::invalid_argument("ExposureProfileMeasure: modelo no soportado: " + model.type_name());
}

double compute_cva_from_exposure(
    const IModel& model, const ExecutionContext& execution,
    const std::vector<double>& times, const std::vector<double>& ee,
    double hazard_rate, double recovery_rate
) {
    if (const auto* hw1 = dynamic_cast<const HullWhite1FModel*>(&model)) {
        return unilateral_cva_from_exposure(
            execution.backend(),
            hw1->a(), hw1->b(), hw1->sigma(), hw1->r0(),
            times, ee, hazard_rate, recovery_rate
        );
    }
    if (const auto* hw2 = dynamic_cast<const HullWhite2FModel*>(&model)) {
        return unilateral_cva_from_exposure_2f(
            execution.backend(),
            hw2->a(), hw2->b(), hw2->sigma(), hw2->eta(), hw2->rho(), hw2->r0(),
            times, ee, hazard_rate, recovery_rate
        );
    }
    throw std::invalid_argument("UnilateralCvaMeasure: modelo no soportado: " + model.type_name());
}

// CVA unilateral a partir de un perfil de exposición ya calculado, descontando por la curva
// OBSERVADA de `market` (`MarketSnapshot::discount_factor`) en vez de la dinámica analítica de
// un modelo de tipo corto -- MISMA fórmula que `unilateral_cva_with_discount` (Rust,
// exposure.rs): `(1-R) * Σ_i EE_i · (S(t_{i-1})-S(t_i)) · DF(t_i)`, supervivencia `S(t) =
// exp(-hazard_rate*t)`. Usada por `PayoffUnilateralCvaQMeasure::evaluate` (measure.hpp tiene el
// razonamiento completo de por qué esta medida NO reutiliza `compute_cva_from_exposure` de
// arriba: esa función necesita un modelo Hull-White para su propio descuento analítico, que no
// tiene sentido para `GbmModel`/`PayoffProduct` -- que ya descuentan por la curva de mercado en
// `PresentValueMeasure`/`Dv01Measure`). Segunda implementación deliberada de la misma
// agregación matemática que la de Rust, documentada explícitamente como tal.
double compute_cva_from_exposure_market(
    const MarketSnapshot& market,
    const std::vector<double>& times, const std::vector<double>& ee,
    double hazard_rate, double recovery_rate
) {
    double cva = 0.0;
    double prev_survival = 1.0;
    for (std::size_t i = 0; i < times.size(); ++i) {
        double survival = std::exp(-hazard_rate * times[i]);
        double default_prob = prev_survival - survival;
        cva += (1.0 - recovery_rate) * ee[i] * default_prob * market.discount_factor(times[i]);
        prev_survival = survival;
    }
    return cva;
}

// --- PV/DV01 por curva de mercado (PLAN_REAPI.md §6 Fase 4) --------------------------------
// Réplica en bonos cero-cupón EXACTAMENTE igual a `IrSwap::npv` en Rust
// (`rust/crates/engine-core/src/products/irs.rs`) pero descontando por
// `MarketSnapshot::discount_factor(t)` en vez de `model.zero_coupon_bond(...)`: PV/DV01 de un
// swap vainilla son función únicamente de la curva de descuento observada, no del tipo corto.
// Asume t=0 (mismo límite documentado en `IrSwap::npv`: la pata flotante aún no ha fijado su
// primer cupón) -- PresentValueMeasure/Dv01Measure ya ignoraban `PricingContext::pricing_date`
// antes de esta fase, sin cambio de comportamiento ahí.

double par_rate_from_market(
    const MarketSnapshot& market, double start,
    const std::vector<double>& payment_times, const std::vector<double>& accruals
) {
    double numerator = market.discount_factor(start) - market.discount_factor(payment_times.back());
    double denominator = 0.0;
    for (std::size_t i = 0; i < payment_times.size(); ++i) {
        denominator += accruals[i] * market.discount_factor(payment_times[i]);
    }
    return numerator / denominator;
}

// Tipo fijo efectivo del contrato: el propio `fixed_rate()` si es explícito, o el par rate
// calculado bajo `market` si el swap se pidió "a la par" (`use_par_rate() == true`) --
// `price_batch`/`price_many`/`price_grid` nunca llegan aquí con `use_par_rate() == true`
// (`require_homogeneous_irs_batch` ya lo rechaza), solo `price()` (trade único) lo necesita.
double effective_fixed_rate(const MarketSnapshot& market, const IrSwapProduct& irs_product) {
    if (!irs_product.use_par_rate()) return irs_product.fixed_rate();
    return par_rate_from_market(market, irs_product.start(), irs_product.payment_times(), irs_product.accruals());
}

// `valuation_time` (PLAN_GREEKS.md §7.3/Fase 5, aditivo, default `0.0`): reescala cada discount
// factor absoluto de `market` (anclado en "hoy"=0 sin importar `valuation_time`, la curva
// observada no se mueve) a "valor en `valuation_time` de un cashflow en `t`" ==
// `discount_factor(t) / discount_factor(valuation_time)` -- Theta puro (§7.1): misma curva,
// mismo tipo fijo efectivo, solo avanza el reloj. `valuation_time == 0.0` da
// `discount_factor(0.0) == 1.0` exacto (ver `Curve::discount_factor`), preservando la formula
// previa byte a byte.
//
// Guardia explicita (`valuation_time > irs_product.start()`): esta formula representa la pata
// flotante completa como DOS discount factors (`P(start)-P(end)`, replica en bonos cero-cupon de
// "la pata flotante vale su nocional en el proximo reset") -- una vez pasado `start()` el primer
// reset ya ocurrio (la pata flotante fijo un cupon conocido que este motor simplificado no
// desglosa por periodo) y la formula deja de ser valida. Sin un `FixingStore` para la pata
// flotante (PLAN_GREEKS.md §7.4, mismo principio que la ruta Monte Carlo de payoff), Theta que
// cruza `start()` se rechaza explicito, nunca se aproxima en silencio.
double npv_from_market(
    const MarketSnapshot& market, const IrSwapProduct& irs_product, double fixed_rate, double valuation_time
) {
    if (valuation_time > irs_product.start()) {
        throw std::invalid_argument(
            "npv_from_market: valuation_time (" + std::to_string(valuation_time) +
            ") posterior al start (" + std::to_string(irs_product.start()) +
            ") del swap -- el primer reset de la pata flotante ya habria ocurrido y este motor no "
            "modela fixings historicos de esa pata (PLAN_GREEKS.md §7.4)"
        );
    }
    const double discount_at_valuation = market.discount_factor(valuation_time);
    double p_start = market.discount_factor(irs_product.start()) / discount_at_valuation;
    double p_end = market.discount_factor(irs_product.payment_times().back()) / discount_at_valuation;
    double floating_leg = irs_product.notional() * (p_start - p_end);

    double fixed_leg = 0.0;
    const std::vector<double>& payment_times = irs_product.payment_times();
    const std::vector<double>& accruals = irs_product.accruals();
    for (std::size_t i = 0; i < payment_times.size(); ++i) {
        fixed_leg +=
            irs_product.notional() * fixed_rate * accruals[i] * (market.discount_factor(payment_times[i]) / discount_at_valuation);
    }
    return floating_leg - fixed_leg;
}

double compute_npv(const MarketSnapshot& market, const IrSwapProduct& irs_product, double valuation_time = 0.0) {
    return npv_from_market(market, irs_product, effective_fixed_rate(market, irs_product), valuation_time);
}

// DV01 = NPV(curva bumpeada en `bump`) - NPV(curva base), MISMO tipo fijo efectivo (fijado UNA
// vez bajo la curva base: el contrato no se re-estructura al mover el mercado) en ambas
// revaloraciones -- bump-and-reval, no AAD. `bump_market_parallel` (PLAN_GREEKS.md §4.2/Fase 3,
// engine/market.hpp) es el único punto del motor que construye esta curva desplazada -- antes
// duplicado aquí y en payoff/market_snapshot_bridge.cpp.
double compute_dv01(const MarketSnapshot& market, const IrSwapProduct& irs_product, double bump, double valuation_time = 0.0) {
    double fixed_rate = effective_fixed_rate(market, irs_product);
    double base = npv_from_market(market, irs_product, fixed_rate, valuation_time);
    double bumped = npv_from_market(bump_market_parallel(market, bump), irs_product, fixed_rate, valuation_time);
    return bumped - base;
}

// Bucketed DV01 (PLAN_REAPI.md §6 Fase 5): un delta por pillar, MISMO tipo fijo efectivo
// (fijado una vez bajo la curva base, igual que compute_dv01) en todas las revaloraciones --
// sum(deltas) == compute_dv01(market, irs_product, bump) porque exp(-z(t)*t) para el pillar i
// solo depende de zero_rates[i] vía interpolación local: bumpear todos los pillars a la vez
// (bump paralelo) y bumpearlos uno a uno y sumar dan, hasta convexidad de segundo orden entre
// pillars, el mismo resultado -- ver el test de consistencia en test_registry.cpp.
//
// Sin `valuation_time` (siempre `0.0`): Theta de un DV01 bucketed queda fuera de esta fase, ver
// el doc-comment de `Dv01Measure::evaluate`.
std::vector<double> dv01_bucketed_from_market(const MarketSnapshot& market, const IrSwapProduct& irs_product, double bump) {
    double fixed_rate = effective_fixed_rate(market, irs_product);
    double base = npv_from_market(market, irs_product, fixed_rate, 0.0);

    std::vector<double> deltas;
    deltas.reserve(market.pillars().size());
    for (std::size_t i = 0; i < market.pillars().size(); ++i) {
        double bumped = npv_from_market(bump_market_pillar(market, i, bump), irs_product, fixed_rate, 0.0);
        deltas.push_back(bumped - base);
    }
    return deltas;
}

// Columnas notional/fixed_rate del lote (PLAN.md §7.19) -- el resto del calendario
// (start/payment_times/accruals) se toma del primer trade, ya validado igual en todos por el
// llamante (`engine::price_batch`).
void columnarize(const std::vector<const IrSwapProduct*>& irs_products, std::vector<double>& notionals, std::vector<double>& fixed_rates) {
    notionals.reserve(irs_products.size());
    fixed_rates.reserve(irs_products.size());
    for (const auto* p : irs_products) {
        notionals.push_back(p->notional());
        fixed_rates.push_back(p->fixed_rate());
    }
}

} // namespace

std::vector<ExposureProfile> compute_exposure_profile_batch(
    const IModel& model, const std::vector<const IrSwapProduct*>& irs_products,
    const PricingContext& pricing, const ExecutionContext& execution
) {
    const IrSwapProduct& first = *irs_products.front();
    const std::vector<double> monitoring_times = reset_dates(first);
    std::vector<double> notionals, fixed_rates;
    columnarize(irs_products, notionals, fixed_rates);

    if (const auto* hw1 = dynamic_cast<const HullWhite1FModel*>(&model)) {
        return irs_hull_white_exposure_profile_batch(
            execution.backend(),
            hw1->a(), hw1->b(), hw1->sigma(), hw1->r0(),
            notionals, fixed_rates,
            first.start(), first.payment_times(), first.accruals(),
            monitoring_times,
            pricing.n_steps(), pricing.n_paths(), pricing.seed()
        );
    }
    if (const auto* hw2 = dynamic_cast<const HullWhite2FModel*>(&model)) {
        return irs_hull_white_2f_exposure_profile_batch(
            execution.backend(),
            hw2->a(), hw2->b(), hw2->sigma(), hw2->eta(), hw2->rho(), hw2->r0(),
            notionals, fixed_rates,
            first.start(), first.payment_times(), first.accruals(),
            monitoring_times,
            pricing.n_steps(), pricing.n_paths(), pricing.seed()
        );
    }
    throw std::invalid_argument("ExposureProfileMeasure: modelo no soportado (lote): " + model.type_name());
}

std::vector<double> compute_cva_from_exposure_batch(
    const IModel& model, const ExecutionContext& execution,
    const std::vector<ExposureProfile>& profiles,
    double hazard_rate, double recovery_rate
) {
    if (const auto* hw1 = dynamic_cast<const HullWhite1FModel*>(&model)) {
        return unilateral_cva_from_exposure_batch(
            execution.backend(), hw1->a(), hw1->b(), hw1->sigma(), hw1->r0(),
            profiles, hazard_rate, recovery_rate
        );
    }
    if (const auto* hw2 = dynamic_cast<const HullWhite2FModel*>(&model)) {
        return unilateral_cva_from_exposure_2f_batch(
            execution.backend(), hw2->a(), hw2->b(), hw2->sigma(), hw2->eta(), hw2->rho(), hw2->r0(),
            profiles, hazard_rate, recovery_rate
        );
    }
    throw std::invalid_argument("UnilateralCvaMeasure: modelo no soportado (lote): " + model.type_name());
}

std::vector<double> compute_npv_batch(const MarketSnapshot& market, const std::vector<const IrSwapProduct*>& irs_products) {
    std::vector<double> results;
    results.reserve(irs_products.size());
    for (const IrSwapProduct* irs : irs_products) {
        // require_homogeneous_irs_batch (price.cpp) ya garantiza use_par_rate() == false aquí.
        // valuation_time=0.0: Theta en lote queda fuera de esta fase (aditivo, sin cambios).
        results.push_back(npv_from_market(market, *irs, irs->fixed_rate(), 0.0));
    }
    return results;
}

std::vector<double> compute_dv01_batch(
    const MarketSnapshot& market, const std::vector<const IrSwapProduct*>& irs_products, double bump
) {
    MarketSnapshot bumped_market = bump_market_parallel(market, bump);
    std::vector<double> results;
    results.reserve(irs_products.size());
    for (const IrSwapProduct* irs : irs_products) {
        double base = npv_from_market(market, *irs, irs->fixed_rate(), 0.0);
        double bumped = npv_from_market(bumped_market, *irs, irs->fixed_rate(), 0.0);
        results.push_back(bumped - base);
    }
    return results;
}

std::vector<std::vector<double>> compute_dv01_bucketed_batch(
    const MarketSnapshot& market, const std::vector<const IrSwapProduct*>& irs_products, double bump
) {
    std::vector<std::vector<double>> results;
    results.reserve(irs_products.size());
    for (const IrSwapProduct* irs : irs_products) {
        // require_homogeneous_irs_batch (price.cpp) ya garantiza use_par_rate() == false aquí.
        results.push_back(dv01_bucketed_from_market(market, *irs, bump));
    }
    return results;
}

MeasureResult ExposureProfileMeasure::evaluate(
    const IModel& model, const IProduct& product, const MarketSnapshot&,
    const PricingContext& pricing, const ExecutionContext& execution
) const {
    const auto* irs_product = dynamic_cast<const IrSwapProduct*>(&product);
    if (!irs_product) {
        throw std::invalid_argument(
            "ExposureProfileMeasure: producto no soportado: " + product.type_name());
    }

    ExposureProfile profile = compute_exposure_profile(model, *irs_product, pricing, execution);

    MeasureResult result;
    result.times = std::move(profile.times);
    result.primary = std::move(profile.ee);
    result.secondary = std::move(profile.pfe_95);
    result.has_scalar = false;
    result.scalar = 0.0;
    return result;
}

MeasureResult UnilateralCvaMeasure::evaluate(
    const IModel& model, const IProduct& product, const MarketSnapshot& market,
    const PricingContext& pricing, const ExecutionContext& execution
) const {
    // Composición (no duplica la llamada a compute_exposure_profile): el tipo de producto se
    // valida dentro de ExposureProfileMeasure::evaluate.
    ExposureProfileMeasure exposure_measure(Params{});
    MeasureResult result = exposure_measure.evaluate(model, product, market, pricing, execution);

    double cva = compute_cva_from_exposure(
        model, execution, result.times, result.primary,
        market.hazard_rate(), market.recovery_rate()
    );

    result.has_scalar = true;
    result.scalar = cva;
    return result;
}

MeasureResult PresentValueMeasure::evaluate(
    const IModel&, const IProduct& product, const MarketSnapshot& market,
    const PricingContext& pricing, const ExecutionContext&
) const {
    MeasureResult result;
    result.has_scalar = true;

    if (const auto* irs_product = dynamic_cast<const IrSwapProduct*>(&product)) {
        result.scalar = compute_npv(market, *irs_product, pricing.pricing_date());
        return result;
    }
    // Rama genérica única para cualquier `PayoffProduct` (PLAN_PRODUCTS.md §9.1/§9.5, Fase 8:
    // "price_many mezcla IRS, FXForward y payoff custom sin ramas de producto nuevas"): un
    // payoff nuevo con esta misma forma (schedule determinista de cashflows en una única
    // moneda) no necesita una rama de producto propia aquí, a diferencia de `IrSwapProduct`
    // arriba -- ver market_snapshot_bridge.hpp para el alcance exacto (una moneda, sin
    // observables que `MarketSnapshot` no modele).
    if (const auto* payoff_product = dynamic_cast<const payoff::PayoffProduct*>(&product)) {
        result.scalar = payoff::present_value_from_market_snapshot(
            payoff_product->payoff_program()->contract, market, pricing.pricing_date()
        ).present_value;
        return result;
    }
    throw std::invalid_argument("PresentValueMeasure: producto no soportado: " + product.type_name());
}

MeasureResult HullWhiteModelNpvMeasure::evaluate(
    const IModel& model, const IProduct& product, const MarketSnapshot&,
    const PricingContext&, const ExecutionContext&
) const {
    const auto* irs_product = dynamic_cast<const IrSwapProduct*>(&product);
    if (!irs_product) {
        throw std::invalid_argument("HullWhiteModelNpvMeasure: producto no soportado: " + product.type_name());
    }

    MeasureResult result;
    result.has_scalar = true;

    if (const auto* hw1f = dynamic_cast<const HullWhite1FModel*>(&model)) {
        result.scalar = irs_hull_white_npv(
            hw1f->a(), hw1f->b(), hw1f->sigma(), hw1f->r0(),
            irs_product->notional(), irs_product->fixed_rate(), irs_product->use_par_rate(), irs_product->start(),
            irs_product->payment_times(), irs_product->accruals()
        );
        return result;
    }
    if (const auto* hw2f = dynamic_cast<const HullWhite2FModel*>(&model)) {
        result.scalar = irs_hull_white_2f_npv(
            hw2f->a(), hw2f->b(), hw2f->sigma(), hw2f->eta(), hw2f->rho(), hw2f->r0(),
            irs_product->notional(), irs_product->fixed_rate(), irs_product->use_par_rate(), irs_product->start(),
            irs_product->payment_times(), irs_product->accruals()
        );
        return result;
    }
    throw std::invalid_argument("HullWhiteModelNpvMeasure: modelo no soportado: " + model.type_name());
}

MeasureResult Dv01Measure::evaluate(
    const IModel&, const IProduct& product, const MarketSnapshot& market,
    const PricingContext& pricing, const ExecutionContext&
) const {
    MeasureResult result;
    if (const auto* irs_product = dynamic_cast<const IrSwapProduct*>(&product)) {
        if (bucketed_) {
            result.times = market.pillars();
            result.primary = dv01_bucketed_from_market(market, *irs_product, bump_);
            result.has_scalar = false;
        } else {
            result.has_scalar = true;
            result.scalar = compute_dv01(market, *irs_product, bump_, pricing.pricing_date());
        }
        return result;
    }
    // Misma rama genérica que PresentValueMeasure (ver ahí). `bucketed` (PLAN_GREEKS.md §11 Fase
    // 3, generaliza la Fase 5 de PLAN_REAPI.md a PayoffProduct): un delta por pillar vía
    // `bump_and_reval_pillar_from_market_snapshot`, misma convención unidireccional que el bump
    // paralelo de abajo -- la suma de los deltas coincide con el DV01 paralelo (mismo argumento
    // que `dv01_bucketed_from_market` para IrSwapProduct, verificado en
    // test_price_many_mixed_products.cpp).
    if (const auto* payoff_product = dynamic_cast<const payoff::PayoffProduct*>(&product)) {
        const auto& contract = payoff_product->payoff_program()->contract;
        if (bucketed_) {
            result.times = market.pillars();
            result.primary.reserve(market.pillars().size());
            for (std::size_t i = 0; i < market.pillars().size(); ++i) {
                result.primary.push_back(payoff::bump_and_reval_pillar_from_market_snapshot(contract, market, i, bump_));
            }
            result.has_scalar = false;
            return result;
        }
        result.has_scalar = true;
        result.scalar = payoff::bump_and_reval_from_market_snapshot(contract, market, bump_);
        return result;
    }
    throw std::invalid_argument("Dv01Measure: producto no soportado: " + product.type_name());
}

// --- Monte Carlo de PayoffProduct bajo Q/P (ver el doc-comment de measure.hpp) --------------

MeasureResult PayoffPriceQMeasure::evaluate(
    const IModel& model, const IProduct& product, const MarketSnapshot&,
    const PricingContext& pricing, const ExecutionContext&
) const {
    const auto* payoff_product = dynamic_cast<const payoff::PayoffProduct*>(&product);
    if (!payoff_product) {
        throw std::invalid_argument("PayoffPriceQMeasure: producto no soportado: " + product.type_name());
    }
    // PLAN_IMPROVE_NOTEBOOK.md Fase 3 §2 punto 3 (decision de diseno documentada): se generaliza
    // ESTA MISMA medida "PayoffPriceQ" (mismo nombre, mismo registro en bootstrap.cpp) para
    // aceptar tambien GbmBasketModel, en vez de registrar una medida separada
    // "PayoffBasketPriceQ" -- el AST/compilador Rust ya es agnostico al numero de observables
    // (`CompiledPayoff::observable_slots`), asi que "que modelo dio el precio" es un detalle de
    // QUIEN evalua, no de QUE se pide: un caller no deberia tener que saber de antemano si un
    // contrato se va a precisar contra un unico activo o un basket para elegir el nombre de la
    // medida correcta.
    if (const auto* gbm_model = dynamic_cast<const GbmModel*>(&model)) {
        payoff::QValuationResult out = payoff::risk_neutral_price_gbm(
            *payoff_product->payoff_program(), *gbm_model, pricing.n_paths(), pricing.seed(), pricing.pricing_date()
        );
        MeasureResult result;
        result.has_scalar = true;
        result.scalar = out.mean;
        return result;
    }
    if (const auto* basket_model = dynamic_cast<const GbmBasketModel*>(&model)) {
        payoff::QValuationResult out =
            payoff::risk_neutral_price_gbm(*payoff_product->payoff_program(), *basket_model, pricing.n_paths(), pricing.seed());
        MeasureResult result;
        result.has_scalar = true;
        result.scalar = out.mean;
        return result;
    }
    throw std::invalid_argument("PayoffPriceQMeasure: modelo no soportado: " + model.type_name());
}

MeasureResult PayoffExerciseQMeasure::evaluate(
    const IModel& model, const IProduct& product, const MarketSnapshot&,
    const PricingContext& pricing, const ExecutionContext&
) const {
    const auto* payoff_product = dynamic_cast<const payoff::PayoffProduct*>(&product);
    if (!payoff_product) {
        throw std::invalid_argument("PayoffExerciseQMeasure: producto no soportado: " + product.type_name());
    }
    const auto* gbm_model = dynamic_cast<const GbmModel*>(&model);
    if (!gbm_model) {
        throw std::invalid_argument("PayoffExerciseQMeasure: modelo no soportado: " + model.type_name());
    }

    payoff::ExercisePolicyResult out =
        payoff::exercise_price_gbm(*payoff_product->payoff_program(), *gbm_model, pricing.n_paths(), pricing.seed());

    MeasureResult result;
    result.has_scalar = true;
    result.scalar = out.price.mean;
    result.times.reserve(out.dates.size());
    result.primary.reserve(out.dates.size());
    result.secondary.reserve(out.dates.size());
    for (const payoff::ExerciseDateDiagnostic& diagnostic : out.dates) {
        result.times.push_back(diagnostic.date);
        result.primary.push_back(diagnostic.exercised_fraction);
        result.secondary.push_back(static_cast<double>(diagnostic.n_in_the_money));
    }
    return result;
}

MeasureResult PayoffHitProbabilityQMeasure::evaluate(
    const IModel& model, const IProduct& product, const MarketSnapshot&,
    const PricingContext& pricing, const ExecutionContext&
) const {
    const auto* payoff_product = dynamic_cast<const payoff::PayoffProduct*>(&product);
    if (!payoff_product) {
        throw std::invalid_argument("PayoffHitProbabilityQMeasure: producto no soportado: " + product.type_name());
    }
    const auto* gbm_model = dynamic_cast<const GbmModel*>(&model);
    if (!gbm_model) {
        throw std::invalid_argument("PayoffHitProbabilityQMeasure: modelo no soportado: " + model.type_name());
    }

    payoff::HitProbabilityResult out = payoff::hit_probability_gbm(
        *payoff_product->payoff_program(), *gbm_model, payoff::EventId{event_}, pricing.n_paths(), pricing.seed()
    );

    MeasureResult result;
    result.has_scalar = true;
    result.scalar = out.probability;
    return result;
}

MeasureResult PayoffExposureProfileQMeasure::evaluate(
    const IModel& model, const IProduct& product, const MarketSnapshot&,
    const PricingContext& pricing, const ExecutionContext&
) const {
    const auto* payoff_product = dynamic_cast<const payoff::PayoffProduct*>(&product);
    if (!payoff_product) {
        throw std::invalid_argument("PayoffExposureProfileQMeasure: producto no soportado: " + product.type_name());
    }
    const auto* gbm_model = dynamic_cast<const GbmModel*>(&model);
    if (!gbm_model) {
        throw std::invalid_argument("PayoffExposureProfileQMeasure: modelo no soportado: " + model.type_name());
    }

    std::vector<payoff::TimePoint> exposure_times;
    exposure_times.reserve(exposure_times_.size());
    for (double t : exposure_times_) exposure_times.push_back(payoff::TimePoint{t});

    ExposureProfile profile = payoff::payoff_exposure_profile_gbm(
        *payoff_product->payoff_program(), *gbm_model, exposure_times, pricing.n_paths(), pricing.seed()
    );

    MeasureResult result;
    result.times = std::move(profile.times);
    result.primary = std::move(profile.ee);
    result.secondary = std::move(profile.pfe_95);
    result.has_scalar = false;
    return result;
}

MeasureResult PayoffUnilateralCvaQMeasure::evaluate(
    const IModel& model, const IProduct& product, const MarketSnapshot& market,
    const PricingContext& pricing, const ExecutionContext& execution
) const {
    // Composición (no duplica la llamada a payoff_exposure_profile_gbm): el tipo de
    // producto/modelo se valida dentro de PayoffExposureProfileQMeasure::evaluate.
    PayoffExposureProfileQMeasure exposure_measure(Params{{"exposure_times", exposure_times_}});
    MeasureResult result = exposure_measure.evaluate(model, product, market, pricing, execution);

    double cva = compute_cva_from_exposure_market(
        market, result.times, result.primary, market.hazard_rate(), market.recovery_rate()
    );

    result.has_scalar = true;
    result.scalar = cva;
    return result;
}

MeasureResult PayoffForecastPMeasure::evaluate(
    const IModel& model, const IProduct& product, const MarketSnapshot&,
    const PricingContext& pricing, const ExecutionContext&
) const {
    const auto* payoff_product = dynamic_cast<const payoff::PayoffProduct*>(&product);
    if (!payoff_product) {
        throw std::invalid_argument("PayoffForecastPMeasure: producto no soportado: " + product.type_name());
    }
    const auto* gbm_p_model = dynamic_cast<const GbmPModel*>(&model);
    if (!gbm_p_model) {
        throw std::invalid_argument("PayoffForecastPMeasure: modelo no soportado: " + model.type_name());
    }

    payoff::ForecastResult out =
        payoff::forecast_gbm_p(*payoff_product->payoff_program(), *gbm_p_model, pricing.n_paths(), pricing.seed());

    MeasureResult result;
    result.has_scalar = true;
    result.scalar = out.mean;
    return result;
}

MeasureResult PayoffHitProbabilityPMeasure::evaluate(
    const IModel& model, const IProduct& product, const MarketSnapshot&,
    const PricingContext& pricing, const ExecutionContext&
) const {
    const auto* payoff_product = dynamic_cast<const payoff::PayoffProduct*>(&product);
    if (!payoff_product) {
        throw std::invalid_argument("PayoffHitProbabilityPMeasure: producto no soportado: " + product.type_name());
    }
    const auto* gbm_p_model = dynamic_cast<const GbmPModel*>(&model);
    if (!gbm_p_model) {
        throw std::invalid_argument("PayoffHitProbabilityPMeasure: modelo no soportado: " + model.type_name());
    }

    payoff::HitProbabilityResult out = payoff::hit_probability_gbm(
        *payoff_product->payoff_program(), *gbm_p_model, payoff::EventId{event_}, pricing.n_paths(), pricing.seed()
    );

    MeasureResult result;
    result.has_scalar = true;
    result.scalar = out.probability;
    return result;
}

MeasureResult PayoffPnlDistributionPMeasure::evaluate(
    const IModel& model, const IProduct& product, const MarketSnapshot&,
    const PricingContext& pricing, const ExecutionContext&
) const {
    const auto* payoff_product = dynamic_cast<const payoff::PayoffProduct*>(&product);
    if (!payoff_product) {
        throw std::invalid_argument("PayoffPnlDistributionPMeasure: producto no soportado: " + product.type_name());
    }
    const auto* gbm_p_model = dynamic_cast<const GbmPModel*>(&model);
    if (!gbm_p_model) {
        throw std::invalid_argument("PayoffPnlDistributionPMeasure: modelo no soportado: " + model.type_name());
    }

    payoff::PnlDistributionResult out = payoff::pnl_distribution_gbm_p(
        *payoff_product->payoff_program(), *gbm_p_model, pricing.n_paths(), pricing.seed(), confidence_
    );

    MeasureResult result;
    result.has_scalar = true;
    result.scalar = out.mean;
    result.primary = {out.var};
    result.secondary = {out.es};
    return result;
}

MeasureResult PayoffSensitivityQMeasure::evaluate(
    const IModel& model, const IProduct& product, const MarketSnapshot&,
    const PricingContext& pricing, const ExecutionContext&
) const {
    const auto* payoff_product = dynamic_cast<const payoff::PayoffProduct*>(&product);
    if (!payoff_product) {
        throw std::invalid_argument("PayoffSensitivityQMeasure: producto no soportado: " + product.type_name());
    }
    const auto* gbm_model = dynamic_cast<const GbmModel*>(&model);
    if (!gbm_model) {
        throw std::invalid_argument("PayoffSensitivityQMeasure: modelo no soportado: " + model.type_name());
    }

    payoff::SensitivityResult out = payoff::payoff_sensitivity_gbm(
        *payoff_product->payoff_program(), *gbm_model, greek_, pricing.n_paths(), pricing.seed()
    );

    MeasureResult result;
    result.has_scalar = true;
    result.scalar = out.value;
    return result;
}

} // namespace engine
