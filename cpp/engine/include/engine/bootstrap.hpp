#pragma once

#include "engine/calibrator.hpp"
#include "engine/measure.hpp"
#include "engine/model.hpp"
#include "engine/product.hpp"
#include "engine/registry.hpp"

namespace engine {

struct Registries {
    Registry<IModel> models;
    Registry<IProduct> products;
    Registry<IMeasure> measures;
    Registry<ICalibrator> calibrators;
};

// Punto único de arranque (PLAN.md §5.4): registra explícitamente cada modelo/producto/medida
// disponible. Añadir un modelo/producto/medida nuevo implica una línea aquí, nada más.
void register_builtins(Registries& registries);

} // namespace engine
