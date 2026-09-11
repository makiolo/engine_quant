#include "engine/measure.hpp"

#include <stdexcept>
#include <utility>

#include "engine/engine.hpp"

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

double npv_from_market(const MarketSnapshot& market, const IrSwapProduct& irs_product, double fixed_rate) {
    double p_start = market.discount_factor(irs_product.start());
    double p_end = market.discount_factor(irs_product.payment_times().back());
    double floating_leg = irs_product.notional() * (p_start - p_end);

    double fixed_leg = 0.0;
    const std::vector<double>& payment_times = irs_product.payment_times();
    const std::vector<double>& accruals = irs_product.accruals();
    for (std::size_t i = 0; i < payment_times.size(); ++i) {
        fixed_leg += irs_product.notional() * fixed_rate * accruals[i] * market.discount_factor(payment_times[i]);
    }
    return floating_leg - fixed_leg;
}

double compute_npv(const MarketSnapshot& market, const IrSwapProduct& irs_product) {
    return npv_from_market(market, irs_product, effective_fixed_rate(market, irs_product));
}

// Curva paralela-bumpeada en `bump` (PLAN_REAPI.md §6 Fase 4): mismos pillars/hazard_rate/
// recovery_rate, cada zero_rate desplazado en `bump`.
MarketSnapshot bump_curve(const MarketSnapshot& market, double bump) {
    std::vector<double> bumped_rates = market.zero_rates();
    for (double& z : bumped_rates) z += bump;
    return MarketSnapshot(market.pillars(), std::move(bumped_rates), market.hazard_rate(), market.recovery_rate());
}

// DV01 = NPV(curva bumpeada en `bump`) - NPV(curva base), MISMO tipo fijo efectivo (fijado UNA
// vez bajo la curva base: el contrato no se re-estructura al mover el mercado) en ambas
// revaloraciones -- bump-and-reval, no AAD.
double compute_dv01(const MarketSnapshot& market, const IrSwapProduct& irs_product, double bump) {
    double fixed_rate = effective_fixed_rate(market, irs_product);
    double base = npv_from_market(market, irs_product, fixed_rate);
    double bumped = npv_from_market(bump_curve(market, bump), irs_product, fixed_rate);
    return bumped - base;
}

// Curva con un ÚNICO pillar bumpeado en `bump` (PLAN_REAPI.md §6 Fase 5), el resto sin tocar
// -- construida sobre bump_curve() de la Fase 4 (bump paralelo), aquí pillar a pillar.
MarketSnapshot bump_pillar(const MarketSnapshot& market, std::size_t pillar_index, double bump) {
    std::vector<double> bumped_rates = market.zero_rates();
    bumped_rates[pillar_index] += bump;
    return MarketSnapshot(market.pillars(), std::move(bumped_rates), market.hazard_rate(), market.recovery_rate());
}

// Bucketed DV01 (PLAN_REAPI.md §6 Fase 5): un delta por pillar, MISMO tipo fijo efectivo
// (fijado una vez bajo la curva base, igual que compute_dv01) en todas las revaloraciones --
// sum(deltas) == compute_dv01(market, irs_product, bump) porque exp(-z(t)*t) para el pillar i
// solo depende de zero_rates[i] vía interpolación local: bumpear todos los pillars a la vez
// (bump paralelo) y bumpearlos uno a uno y sumar dan, hasta convexidad de segundo orden entre
// pillars, el mismo resultado -- ver el test de consistencia en test_registry.cpp.
std::vector<double> dv01_bucketed_from_market(const MarketSnapshot& market, const IrSwapProduct& irs_product, double bump) {
    double fixed_rate = effective_fixed_rate(market, irs_product);
    double base = npv_from_market(market, irs_product, fixed_rate);

    std::vector<double> deltas;
    deltas.reserve(market.pillars().size());
    for (std::size_t i = 0; i < market.pillars().size(); ++i) {
        double bumped = npv_from_market(bump_pillar(market, i, bump), irs_product, fixed_rate);
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
        results.push_back(npv_from_market(market, *irs, irs->fixed_rate()));
    }
    return results;
}

std::vector<double> compute_dv01_batch(
    const MarketSnapshot& market, const std::vector<const IrSwapProduct*>& irs_products, double bump
) {
    MarketSnapshot bumped_market = bump_curve(market, bump);
    std::vector<double> results;
    results.reserve(irs_products.size());
    for (const IrSwapProduct* irs : irs_products) {
        double base = npv_from_market(market, *irs, irs->fixed_rate());
        double bumped = npv_from_market(bumped_market, *irs, irs->fixed_rate());
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
    const PricingContext&, const ExecutionContext&
) const {
    const auto* irs_product = dynamic_cast<const IrSwapProduct*>(&product);
    if (!irs_product) {
        throw std::invalid_argument("PresentValueMeasure: producto no soportado: " + product.type_name());
    }

    MeasureResult result;
    result.has_scalar = true;
    result.scalar = compute_npv(market, *irs_product);
    return result;
}

MeasureResult Dv01Measure::evaluate(
    const IModel&, const IProduct& product, const MarketSnapshot& market,
    const PricingContext&, const ExecutionContext&
) const {
    const auto* irs_product = dynamic_cast<const IrSwapProduct*>(&product);
    if (!irs_product) {
        throw std::invalid_argument("Dv01Measure: producto no soportado: " + product.type_name());
    }

    MeasureResult result;
    if (bucketed_) {
        result.times = market.pillars();
        result.primary = dv01_bucketed_from_market(market, *irs_product, bump_);
        result.has_scalar = false;
    } else {
        result.has_scalar = true;
        result.scalar = compute_dv01(market, *irs_product, bump_);
    }
    return result;
}

} // namespace engine
