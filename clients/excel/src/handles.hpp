#pragma once

// Registry de modelos/productos/medidas expuesto a Excel como "handles" (PLAN.md Fase 4,
// §7.8): una celda de Excel solo puede contener números/cadenas/booleanos/arrays, nunca un
// puntero a IModel/IProduct/IMeasure directamente (a diferencia de `engine.Model`/
// `engine.Product`/`engine.Measure` en Python, que sí son objetos Python opacos, PLAN.md
// §7.7). En vez de un mecanismo de "handle" ligado al ciclo de vida de la celda que lo creó
// (invalidación en recálculo, xlfGetCaller, etc. — complejidad importante y no verificable
// sin Excel instalado en este entorno), las instancias se memoizan por una clave canónica
// (nombre + parámetros ordenados, ver xlbridge::table_to_params): los mismos parámetros
// producen siempre el mismo handle, así que la fórmula es determinista y no hace falta
// liberar handles individualmente — la limitación conocida es que las instancias viven hasta
// xlAutoClose (documentado en clients/excel/README.md), no por-celda.
//
// Reutiliza exactamente el mismo `engine::Registries`/`register_builtins`/`Registry<T>::
// create`/`IMeasure::evaluate` que ya consumen cpp/engine/tests (Fase 2) y clients/python
// (Fase 3): añadir un modelo/producto/medida nuevo en bootstrap.cpp lo deja disponible aquí
// sin tocar este fichero (PLAN.md §5.4, §4).

#include <string>
#include <unordered_map>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "XLCALL.H"
#include "engine/bootstrap.hpp"

namespace xlbridge {

class HandleRegistry {
public:
    HandleRegistry();

    std::vector<std::string> list_models() const;
    std::vector<std::string> list_products() const;
    std::vector<std::string> list_measures() const;
    std::vector<std::string> list_calibrators() const;

    std::string create_model(const std::string& name, const XLOPER12& params_arg);
    std::string create_product(const std::string& name, const XLOPER12& params_arg);
    std::string create_measure(const std::string& name);
    // Sin parámetros que memoizar (a diferencia de create_model/create_product): un
    // ICalibrator no tiene estado propio, ver engine::HullWhite1FCalibrator -- el handle se
    // memoiza solo por `name`.
    std::string create_calibrator(const std::string& name);

    engine::MeasureResult evaluate(
        const std::string& measure_handle,
        const std::string& model_handle,
        const std::string& product_handle,
        const XLOPER12& params_arg
    ) const;

    // market_arg: rango de 2 columnas (pillars, zero_rates), ver xlbridge::table_to_market.
    // initial_guess_arg: mismo formato clave/valor que params_arg en el resto de create_*.
    engine::CalibrationResult calibrate(
        const std::string& calibrator_handle, const XLOPER12& market_arg, const XLOPER12& initial_guess_arg
    ) const;

    // Libera todas las instancias memoizadas (xlAutoClose, engine_excel.cpp).
    void clear();

private:
    engine::Registries registries_;
    std::unordered_map<std::string, std::unique_ptr<engine::IModel>> models_;
    std::unordered_map<std::string, std::unique_ptr<engine::IProduct>> products_;
    std::unordered_map<std::string, std::unique_ptr<engine::IMeasure>> measures_;
    std::unordered_map<std::string, std::unique_ptr<engine::ICalibrator>> calibrators_;
};

// Instancia única de proceso (una por XLL cargado en Excel), usada desde engine_excel.cpp.
HandleRegistry& shared();

} // namespace xlbridge
