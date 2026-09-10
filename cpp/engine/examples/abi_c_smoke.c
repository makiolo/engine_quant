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

    /* Calibracion (PLAN.md §7.14, generalizada en §7.18): EngineCalibrator es un handle mas,
     * creado por nombre igual que EngineModel/EngineProduct -- el mismo engine_abi_calibrate
     * sirve para HullWhite1F y HullWhite2F, sin una funcion por modelo (a diferencia de la
     * version anterior de esta ABI, con un engine_abi_calibrate_hull_white especifico). El
     * mercado se fabrica aqui a mano (una curva creciente sencilla, no necesariamente la que
     * generaria uno u otro modelo exactamente -- esta sonda solo demuestra el mecanismo de la
     * ABI, no vuelve a fijar un valor de referencia numerico). */
    {
        double cal_pillars[6] = {0.5, 1.0, 2.0, 5.0, 10.0, 20.0};
        double cal_zero_rates[6] = {0.018, 0.019, 0.021, 0.024, 0.026, 0.027};
        EngineMarketSnapshot cal_market;
        const char* calibrator_names[2] = {"HullWhite1F", "HullWhite2F"};
        int m;

        memset(&cal_market, 0, sizeof(cal_market));
        cal_market.pillars = cal_pillars;
        cal_market.zero_rates = cal_zero_rates;
        cal_market.count = 6;

        for (m = 0; m < 2; ++m) {
            EngineCalibrator* calibrator = engine_abi_create_calibrator(calibrator_names[m]);
            EngineParam initial_guess[6];
            size_t n_initial_guess;
            EngineCalibrationResult cal_result;
            EngineModel* calibrated_model;

            if (!calibrator) return fail("engine_abi_create_calibrator");

            initial_guess[0].key = "a";
            initial_guess[0].kind = ENGINE_PARAM_DOUBLE;
            initial_guess[0].scalar = 0.3;
            initial_guess[1].key = "b";
            initial_guess[1].kind = ENGINE_PARAM_DOUBLE;
            initial_guess[1].scalar = (m == 0) ? 0.02 : 0.2; /* HullWhite2F: b es velocidad, no nivel */
            initial_guess[2].key = "sigma";
            initial_guess[2].kind = ENGINE_PARAM_DOUBLE;
            initial_guess[2].scalar = 0.01;
            if (m == 0) {
                initial_guess[3].key = "r0";
                initial_guess[3].kind = ENGINE_PARAM_DOUBLE;
                initial_guess[3].scalar = 0.02;
                n_initial_guess = 4;
            } else {
                initial_guess[3].key = "eta";
                initial_guess[3].kind = ENGINE_PARAM_DOUBLE;
                initial_guess[3].scalar = 0.01;
                initial_guess[4].key = "rho";
                initial_guess[4].kind = ENGINE_PARAM_DOUBLE;
                initial_guess[4].scalar = -0.5;
                initial_guess[5].key = "r0";
                initial_guess[5].kind = ENGINE_PARAM_DOUBLE;
                initial_guess[5].scalar = 0.02;
                n_initial_guess = 6;
            }

            memset(&cal_result, 0, sizeof(cal_result));
            if (engine_abi_calibrate(calibrator, &cal_market, initial_guess, n_initial_guess, &cal_result) != 0) {
                engine_abi_free_calibrator(calibrator);
                return fail("engine_abi_calibrate");
            }
            printf(
                "calibracion %s: rmse=%.3e iterations=%d converged=%s\n",
                calibrator_names[m], cal_result.rmse, cal_result.iterations, cal_result.converged ? "si" : "no"
            );

            /* El resultado se pasa directamente a engine_abi_create_model, sin traduccion --
             * cierra el circulo Mercado -> calibrar -> Modelo calibrado. */
            calibrated_model = engine_abi_create_model(calibrator_names[m], cal_result.optimal_params, cal_result.n_params);
            if (!calibrated_model) {
                engine_abi_free_calibration_result(&cal_result);
                engine_abi_free_calibrator(calibrator);
                return fail("engine_abi_create_model desde el resultado de calibrar");
            }

            engine_abi_free_model(calibrated_model);
            engine_abi_free_calibration_result(&cal_result);
            engine_abi_free_calibrator(calibrator);
        }

        /* engine_abi_create_calibrator rechaza un nombre desconocido igual que
         * engine_abi_create_model (PLAN.md §5.5: "ninguna excepcion cruza esta frontera"). */
        {
            EngineCalibrator* unknown = engine_abi_create_calibrator("NoExiste");
            if (unknown != NULL) {
                engine_abi_free_calibrator(unknown);
                fprintf(stderr, "FALLO: se esperaba NULL al crear un calibrador desconocido\n");
                return 1;
            }
        }
    }

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

    /* engine_abi_calc_batch (PLAN.md §7.17/§7.19): lote homogeneo -- 3 swaps del mismo
     * calendario, cada uno con su propio notional/fixed_rate explicito (sin use_par_rate,
     * que el lote no soporta), vectorizado sin bucle escalar. EngineProduct** es un patron
     * nuevo en esta ABI (hasta ahora un handle se pasaba de uno en uno). */
    {
        double batch_notionals[3] = {1000000.0, 2500000.0, 500000.0};
        double batch_fixed_rates[3] = {0.02, 0.015, 0.025};
        EngineProduct* batch_products[3];
        const EngineProduct* batch_products_const[3];
        EngineParam batch_irs_params[4];
        const char* batch_measure_names[3] = {"PV", "ExpectedExposure", "UnilateralCVA"};
        EngineCalcBatchResultEntry* batch_entries = NULL;
        size_t batch_count = 0;
        int bi;

        for (bi = 0; bi < 3; ++bi) {
            batch_irs_params[0].key = "notional";
            batch_irs_params[0].kind = ENGINE_PARAM_DOUBLE;
            batch_irs_params[0].scalar = batch_notionals[bi];
            batch_irs_params[1].key = "fixed_rate";
            batch_irs_params[1].kind = ENGINE_PARAM_DOUBLE;
            batch_irs_params[1].scalar = batch_fixed_rates[bi];
            batch_irs_params[2].key = "payment_times";
            batch_irs_params[2].kind = ENGINE_PARAM_VECTOR;
            batch_irs_params[2].values = payment_times;
            batch_irs_params[2].count = 5;
            batch_irs_params[3].key = "accruals";
            batch_irs_params[3].kind = ENGINE_PARAM_VECTOR;
            batch_irs_params[3].values = accruals;
            batch_irs_params[3].count = 5;

            batch_products[bi] = engine_abi_create_product("IRSwap", batch_irs_params, 4);
            if (!batch_products[bi]) return fail("engine_abi_create_product (lote)");
            batch_products_const[bi] = batch_products[bi];
        }

        if (engine_abi_calc_batch(
                batch_products_const, 3, batch_measure_names, 3, model, &market, &pricing, &execution,
                &batch_entries, &batch_count
            ) != 0) {
            for (bi = 0; bi < 3; ++bi) engine_abi_free_product(batch_products[bi]);
            return fail("engine_abi_calc_batch");
        }

        for (bi = 0; bi < (int) batch_count; ++bi) {
            const EngineCalcResultEntry* pv_row = NULL;
            size_t k;
            for (k = 0; k < batch_entries[bi].n_measures; ++k) {
                if (strcmp(batch_entries[bi].measures[k].measure_name, "PV") == 0) {
                    pv_row = &batch_entries[bi].measures[k];
                }
            }
            printf(
                "lote trade_index=%zu PV=%.4f\n", batch_entries[bi].trade_index, pv_row ? pv_row->result.scalar : 0.0
            );
        }

        engine_abi_free_calc_batch_results(batch_entries, batch_count);
        for (bi = 0; bi < 3; ++bi) engine_abi_free_product(batch_products[bi]);
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
