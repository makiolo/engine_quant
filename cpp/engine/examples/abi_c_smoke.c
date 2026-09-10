/* Sonda en C puro (no C++) de engine/abi.h (PLAN.md Fase 6, §5.5): demuestra/verifica que la
 * ABI se puede consumir de verdad desde un compilador de C, sin tocar cxx/nanobind/el bridge
 * de Excel -- lo mas parecido que se puede probar en este arbol a un consumidor externo real
 * (Julia via ccall, .NET via P/Invoke, Go via cgo), todos los cuales enlazan contra el mismo
 * .dll/.so y una traduccion de este mismo header a su propio lenguaje.
 *
 * Ejercita el mismo caso base (IRS 5y anual a la par bajo Hull-White 1F, PLAN.md §5.2) y la
 * misma semilla que clients/excel/README.md ("Verificacion manual"): si el CVA impreso no es
 * 426.7618244093184, algo se rompio en la traduccion C ABI <-> engine::Registries/IMeasure.
 */

#include "engine/abi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fail(const char* what) {
    char buffer[256];
    size_t len = engine_abi_last_error(buffer, sizeof(buffer));
    fprintf(stderr, "FALLO: %s (%.*s)\n", what, (int) len, buffer);
    return 1;
}

int main(void) {
    EngineParam hw_params[4];
    EngineParam irs_params[3];
    EngineParam measure_params[5];
    double payment_times[5] = {1.0, 2.0, 3.0, 4.0, 5.0};
    double accruals[5] = {1.0, 1.0, 1.0, 1.0, 1.0};
    double monitoring_times[4] = {0.0, 1.0, 2.0, 3.0};
    const char** model_names = NULL;
    size_t model_count;
    size_t i;
    int found_hull_white;
    EngineModel* model;
    EngineProduct* product;
    EngineMeasure* measure;
    EngineMeasureResult result;
    char backend[16];
    size_t backend_len;

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

    {
        /* ExposureProfile ademas de UnilateralCVA (misma semilla/caso que
         * clients/excel/README.md, "Verificacion manual"): EE≈[0, 12862.62, 13673.53]. */
        EngineParam profile_params[3];
        EngineMeasureResult profile_result;
        double profile_times[3] = {0.0, 1.0, 2.0};
        EngineMeasure* profile_measure = engine_abi_create_measure("ExposureProfile");
        if (!profile_measure) {
            engine_abi_free_product(product);
            engine_abi_free_model(model);
            return fail("engine_abi_create_measure(ExposureProfile)");
        }
        profile_params[0].key = "monitoring_times";
        profile_params[0].kind = ENGINE_PARAM_VECTOR;
        profile_params[0].values = profile_times;
        profile_params[0].count = 3;
        profile_params[1].key = "n_paths";
        profile_params[1].kind = ENGINE_PARAM_DOUBLE;
        profile_params[1].scalar = 5000.0;
        profile_params[2].key = "seed";
        profile_params[2].kind = ENGINE_PARAM_DOUBLE;
        profile_params[2].scalar = 7.0;

        memset(&profile_result, 0, sizeof(profile_result));
        if (engine_abi_evaluate(profile_measure, model, product, profile_params, 3, &profile_result) != 0) {
            engine_abi_free_measure(profile_measure);
            engine_abi_free_product(product);
            engine_abi_free_model(model);
            return fail("engine_abi_evaluate(ExposureProfile)");
        }
        printf("ExposureProfile EE (seed=7)  = [%.2f, %.2f, %.2f]  (esperado: [0, 12862.62, 13673.53])\n",
               profile_result.primary[0], profile_result.primary[1], profile_result.primary[2]);
        engine_abi_free_measure_result(&profile_result);
        engine_abi_free_measure(profile_measure);
    }

    measure = engine_abi_create_measure("UnilateralCVA");
    if (!measure) {
        engine_abi_free_model(model);
        engine_abi_free_product(product);
        return fail("engine_abi_create_measure(UnilateralCVA)");
    }

    measure_params[0].key = "monitoring_times";
    measure_params[0].kind = ENGINE_PARAM_VECTOR;
    measure_params[0].values = monitoring_times;
    measure_params[0].count = 4;
    measure_params[1].key = "n_paths";
    measure_params[1].kind = ENGINE_PARAM_DOUBLE;
    measure_params[1].scalar = 5000.0;
    measure_params[2].key = "seed";
    measure_params[2].kind = ENGINE_PARAM_DOUBLE;
    measure_params[2].scalar = 13.0;
    measure_params[3].key = "hazard_rate";
    measure_params[3].kind = ENGINE_PARAM_DOUBLE;
    measure_params[3].scalar = 0.02;
    measure_params[4].key = "recovery_rate";
    measure_params[4].kind = ENGINE_PARAM_DOUBLE;
    measure_params[4].scalar = 0.4;

    memset(&result, 0, sizeof(result));
    if (engine_abi_evaluate(measure, model, product, measure_params, 5, &result) != 0) {
        engine_abi_free_measure(measure);
        engine_abi_free_product(product);
        engine_abi_free_model(model);
        return fail("engine_abi_evaluate(UnilateralCVA)");
    }

    printf("UnilateralCVA (seed=13) = %.13f  (esperado: 426.7618244093184)\n", result.scalar);

    engine_abi_free_measure_result(&result);
    engine_abi_free_measure(measure);
    engine_abi_free_product(product);
    engine_abi_free_model(model);

    backend_len = engine_abi_get_compute_backend(backend, sizeof(backend));
    printf("backend de computo: %.*s (gpu disponible: %s)\n",
           (int) backend_len, backend, engine_abi_is_gpu_backend_available() ? "si" : "no");

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
        backend_len = engine_abi_last_error(backend, sizeof(backend));
        printf("error esperado al pedir un modelo inexistente: %.*s...\n", (int) backend_len, backend);
    }

    printf("OK: engine/abi.h consumido desde C puro de punta a punta.\n");
    return 0;
}
