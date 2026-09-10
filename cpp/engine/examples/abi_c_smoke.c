/* Sonda en C puro (no C++) de engine/abi.h (PLAN.md Fase 6, §5.5; ENGINE.CALC en PLAN.md
 * §7.15): demuestra/verifica que la ABI se puede consumir de verdad desde un compilador de C,
 * sin tocar cxx/nanobind/el bridge de Excel -- lo mas parecido que se puede probar en este
 * arbol a un consumidor externo real (Julia via ccall, .NET via P/Invoke, Go via cgo), todos
 * los cuales enlazan contra el mismo .dll/.so y una traduccion de este mismo header a su
 * propio lenguaje.
 *
 * Ejercita el mismo caso base (IRS 5y anual a la par bajo Hull-White 1F, PLAN.md §5.2) que el
 * valor de referencia exacto de cpp/engine/tests/test_registry.cpp (Registry.
 * UnilateralCvaMatchesGoldenValue/ExposureProfileMatchesGoldenValue) -- aqui basta con
 * invariantes cualitativos (no negatividad, PFE95>=EE, PV~0, DV01>0, CVA>0): esta sonda
 * verifica el mecanismo de la ABI, no vuelve a fijar el numero exacto (eso ya lo hace
 * test_registry.cpp, comprobado alli bit a bit).
 */

#include "engine/abi.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fail(const char* what) {
    char buffer[256];
    size_t len = engine_abi_last_error(buffer, sizeof(buffer));
    fprintf(stderr, "FALLO: %s (%.*s)\n", what, (int) len, buffer);
    return 1;
}

static const EngineCalcResultEntry* find_entry(EngineCalcResultEntry* entries, size_t count, const char* name) {
    size_t i;
    for (i = 0; i < count; ++i) {
        if (strcmp(entries[i].measure_name, name) == 0) return &entries[i];
    }
    return NULL;
}

int main(void) {
    EngineParam hw_params[4];
    EngineParam irs_params[3];
    double payment_times[5] = {1.0, 2.0, 3.0, 4.0, 5.0};
    double accruals[5] = {1.0, 1.0, 1.0, 1.0, 1.0};
    double pillar = 1.0, zero_rate = 0.02;
    const char** model_names = NULL;
    size_t model_count;
    size_t i;
    int found_hull_white;
    EngineModel* model;
    EngineProduct* product;
    EngineMarketSnapshot market;
    EnginePricingContext pricing;
    EngineExecutionContext execution;
    const char* measure_names[5] = {"PV", "DV01", "ExpectedExposure", "PFE95", "UnilateralCVA"};
    EngineCalcResultEntry* entries = NULL;
    size_t entry_count = 0;
    const EngineCalcResultEntry *pv, *dv01, *ee, *pfe, *cva;
    char error_buf[256];
    size_t error_len;

    printf("engine_abi_version() = %d\n", engine_abi_version());

    model_count = engine_abi_list_models(&model_names);
    found_hull_white = 0;
    for (i = 0; i < model_count; ++i) {
        if (strcmp(model_names[i], "HullWhite1F") == 0) found_hull_white = 1;
    }
    engine_abi_free_string_list(model_names, model_count);
    if (!found_hull_white) return fail("HullWhite1F no aparece en engine_abi_list_models");

    hw_params[0].key = "a";
    hw_params[0].kind = ENGINE_PARAM_DOUBLE;
    hw_params[0].scalar = 0.1;
    hw_params[1].key = "b";
    hw_params[1].kind = ENGINE_PARAM_DOUBLE;
    hw_params[1].scalar = 0.03;
    hw_params[2].key = "sigma";
    hw_params[2].kind = ENGINE_PARAM_DOUBLE;
    hw_params[2].scalar = 0.01;
    hw_params[3].key = "r0";
    hw_params[3].kind = ENGINE_PARAM_DOUBLE;
    hw_params[3].scalar = 0.02;

    model = engine_abi_create_model("HullWhite1F", hw_params, 4);
    if (!model) return fail("engine_abi_create_model(HullWhite1F)");

    irs_params[0].key = "notional";
    irs_params[0].kind = ENGINE_PARAM_DOUBLE;
    irs_params[0].scalar = 1000000.0;
    irs_params[1].key = "payment_times";
    irs_params[1].kind = ENGINE_PARAM_VECTOR;
    irs_params[1].values = payment_times;
    irs_params[1].count = 5;
    irs_params[2].key = "accruals";
    irs_params[2].kind = ENGINE_PARAM_VECTOR;
    irs_params[2].values = accruals;
    irs_params[2].count = 5;

    product = engine_abi_create_product("IRSwap", irs_params, 3);
    if (!product) {
        engine_abi_free_model(model);
        return fail("engine_abi_create_product(IRSwap)");
    }

    memset(&market, 0, sizeof(market));
    market.pillars = &pillar;
    market.zero_rates = &zero_rate;
    market.count = 1;
    market.hazard_rate = 0.02;
    market.recovery_rate = 0.4;

    memset(&pricing, 0, sizeof(pricing));
    pricing.pricing_date = 0.0;
    pricing.n_paths = 5000;
    pricing.n_steps = 208; /* ~1 paso/semana sobre los 4 anios hasta la ultima fecha de reseteo */
    pricing.seed = 7;

    execution.backend = "cpu";
    execution.precision = "FP64";

    if (engine_abi_calc(
            product, measure_names, 5, model, &market, &pricing, &execution, &entries, &entry_count
        ) != 0) {
        engine_abi_free_product(product);
        engine_abi_free_model(model);
        return fail("engine_abi_calc");
    }

    pv = find_entry(entries, entry_count, "PV");
    dv01 = find_entry(entries, entry_count, "DV01");
    ee = find_entry(entries, entry_count, "ExpectedExposure");
    pfe = find_entry(entries, entry_count, "PFE95");
    cva = find_entry(entries, entry_count, "UnilateralCVA");
    if (!pv || !dv01 || !ee || !pfe || !cva) {
        engine_abi_free_calc_results(entries, entry_count);
        engine_abi_free_product(product);
        engine_abi_free_model(model);
        return fail("engine_abi_calc no devolvio las 5 medidas esperadas");
    }

    printf("PV            = %.4f  (swap a la par: ~0)\n", pv->result.scalar);
    printf("DV01          = %.4f  (swap pagador: > 0)\n", dv01->result.scalar);
    printf("UnilateralCVA = %.4f  (> 0 con hazard_rate > 0)\n", cva->result.scalar);
    for (i = 0; i < ee->result.len; ++i) {
        printf("  t=%.0f: EE=%.2f  PFE95=%.2f\n", ee->result.times[i], ee->result.primary[i], pfe->result.primary[i]);
    }

    if (fabs(pv->result.scalar) > 1e-6) {
        engine_abi_free_calc_results(entries, entry_count);
        engine_abi_free_product(product);
        engine_abi_free_model(model);
        fprintf(stderr, "FALLO: PV de un swap a la par deberia ser ~0\n");
        return 1;
    }
    if (dv01->result.scalar <= 0.0 || cva->result.scalar <= 0.0) {
        engine_abi_free_calc_results(entries, entry_count);
        engine_abi_free_product(product);
        engine_abi_free_model(model);
        fprintf(stderr, "FALLO: se esperaba DV01 > 0 y UnilateralCVA > 0\n");
        return 1;
    }
    for (i = 0; i < ee->result.len; ++i) {
        if (ee->result.primary[i] < 0.0 || pfe->result.primary[i] < ee->result.primary[i]) {
            engine_abi_free_calc_results(entries, entry_count);
            engine_abi_free_product(product);
            engine_abi_free_model(model);
            fprintf(stderr, "FALLO: se esperaba ExpectedExposure >= 0 y PFE95 >= ExpectedExposure\n");
            return 1;
        }
    }

    engine_abi_free_calc_results(entries, entry_count);

    printf("backend disponible en GPU: %s\n", engine_abi_is_gpu_backend_available() ? "si" : "no");

    /* engine_abi_calc rechaza un nombre de medida desconocido (PLAN.md §7.15): el error queda
     * en engine_abi_last_error(), nunca lanza/aborta a traves de esta frontera C. */
    {
        const char* bad_names[1] = {"NoExiste"};
        EngineCalcResultEntry* bad_entries = NULL;
        size_t bad_count = 0;
        int rc = engine_abi_calc(product, bad_names, 1, model, &market, &pricing, &execution, &bad_entries, &bad_count);
        if (rc == 0) {
            engine_abi_free_calc_results(bad_entries, bad_count);
            engine_abi_free_product(product);
            engine_abi_free_model(model);
            fprintf(stderr, "FALLO: se esperaba error con un nombre de medida desconocido\n");
            return 1;
        }
        error_len = engine_abi_last_error(error_buf, sizeof(error_buf));
        printf("error esperado al pedir una medida inexistente: %.*s\n", (int) error_len, error_buf);
    }

    engine_abi_free_product(product);
    engine_abi_free_model(model);

    /* Manejo de errores (PLAN.md §5.5: "ninguna excepcion de C++ cruza la frontera"): un
     * nombre de modelo desconocido devuelve NULL, nunca lanza/aborta -- el detalle queda en
     * engine_abi_last_error(). */
    {
        EngineModel* unknown = engine_abi_create_model("NoExiste", NULL, 0);
        if (unknown != NULL) {
            engine_abi_free_model(unknown);
            fprintf(stderr, "FALLO: se esperaba NULL al crear un modelo desconocido\n");
            return 1;
        }
        error_len = engine_abi_last_error(error_buf, sizeof(error_buf));
        printf("error esperado al pedir un modelo inexistente: %.*s\n", (int) error_len, error_buf);
    }

    printf("OK: engine/abi.h (ENGINE.CALC) consumido desde C puro de punta a punta.\n");
    return 0;
}
