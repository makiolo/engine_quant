// Ejemplo de C++ consumiendo engine/abi.h (PLAN.md Fase 6, §5.5/§7.13) SIN pasar por el
// registry C++ interno (engine::Registries/IMeasure, cpp/engine/include/engine/*.hpp) ni por
// cxx -- exactamente la misma superficie extern "C" que vería un consumidor externo real
// (Julia vía ccall, .NET vía P/Invoke, Go vía cgo). Este fichero envuelve esa superficie con
// RAII y excepciones porque el consumidor aquí sí es C++ y puede permitírselo; abi.h en sí no
// asume eso (por eso es un header C puro, ver examples/abi/c-puro más abajo).
//
// Compilar dentro de este repo (ver examples/abi/README.md): es un target más de
// cpp/engine/CMakeLists.txt (engine_abi_cpp_example), `cmake --build build` lo compila junto
// al resto. Fuera de este árbol de CMake, contra un engine_abi ya compilado:
//   cl /std:c++17 /EHsc /I <repo>\cpp\engine\include main.cpp <repo>\build\cpp\engine\engine_abi.lib

#include "engine/abi.h"

#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

// Traduce el patrón "NULL/!=0 + engine_abi_last_error" del header a una excepción -- lo único
// que hace falta para que el resto de este fichero se lea como C++ normal, sin comprobar NULL
// después de cada llamada.
[[noreturn]] void throw_last_error(const std::string& what) {
    char buffer[512];
    std::size_t len = engine_abi_last_error(buffer, sizeof(buffer));
    throw std::runtime_error(what + ": " + std::string(buffer, len));
}

// Deleters para std::unique_ptr sobre los handles opacos de la ABI: liberan exactamente como
// documenta abi.h (engine_abi_free_*), nunca delete/free directo sobre un EngineModel* etc.
struct ModelDeleter {
    void operator()(EngineModel* p) const { engine_abi_free_model(p); }
};
struct ProductDeleter {
    void operator()(EngineProduct* p) const { engine_abi_free_product(p); }
};
struct MeasureDeleter {
    void operator()(EngineMeasure* p) const { engine_abi_free_measure(p); }
};

using ModelPtr = std::unique_ptr<EngineModel, ModelDeleter>;
using ProductPtr = std::unique_ptr<EngineProduct, ProductDeleter>;
using MeasurePtr = std::unique_ptr<EngineMeasure, MeasureDeleter>;

EngineParam scalar_param(const char* key, double value) {
    EngineParam p{};
    p.key = key;
    p.kind = ENGINE_PARAM_DOUBLE;
    p.scalar = value;
    return p;
}

EngineParam vector_param(const char* key, const std::vector<double>& values) {
    EngineParam p{};
    p.key = key;
    p.kind = ENGINE_PARAM_VECTOR;
    p.values = values.data();
    p.count = values.size();
    return p;
}

ModelPtr create_model(const char* name, const std::vector<EngineParam>& params) {
    EngineModel* raw = engine_abi_create_model(name, params.data(), params.size());
    if (!raw) throw_last_error(std::string("engine_abi_create_model(") + name + ")");
    return ModelPtr(raw);
}

ProductPtr create_product(const char* name, const std::vector<EngineParam>& params) {
    EngineProduct* raw = engine_abi_create_product(name, params.data(), params.size());
    if (!raw) throw_last_error(std::string("engine_abi_create_product(") + name + ")");
    return ProductPtr(raw);
}

MeasurePtr create_measure(const char* name) {
    EngineMeasure* raw = engine_abi_create_measure(name);
    if (!raw) throw_last_error(std::string("engine_abi_create_measure(") + name + ")");
    return MeasurePtr(raw);
}

// RAII para EngineMeasureResult (los arrays times/primary/secondary son owned por engine_abi,
// ver abi.h): se liberan en el destructor con engine_abi_free_measure_result, nunca con
// delete[] directo.
class MeasureResult {
public:
    explicit MeasureResult(EngineMeasureResult value) : value_(value) {}
    ~MeasureResult() { engine_abi_free_measure_result(&value_); }
    MeasureResult(const MeasureResult&) = delete;
    MeasureResult& operator=(const MeasureResult&) = delete;

    const EngineMeasureResult& get() const { return value_; }

private:
    EngineMeasureResult value_;
};

MeasureResult evaluate(
    EngineMeasure* measure, EngineModel* model, EngineProduct* product, const std::vector<EngineParam>& params
) {
    EngineMeasureResult result{};
    if (engine_abi_evaluate(measure, model, product, params.data(), params.size(), &result) != 0) {
        throw_last_error("engine_abi_evaluate");
    }
    return MeasureResult(result);
}

} // namespace

int main() {
    std::cout << "engine_abi_version() = " << engine_abi_version() << "\n";

    // payment_times/accruals/monitoring_times deben seguir vivos mientras se usan los
    // EngineParam que apuntan a ellos (uno por llamada, ver vector_param); les basta con
    // sobrevivir hasta el final de la llamada correspondiente, aquí hasta el final de main().
    std::vector<double> payment_times{1.0, 2.0, 3.0, 4.0, 5.0};
    std::vector<double> accruals{1.0, 1.0, 1.0, 1.0, 1.0};

    ModelPtr model = create_model(
        "HullWhite1F",
        {scalar_param("a", 0.1), scalar_param("b", 0.03), scalar_param("sigma", 0.01), scalar_param("r0", 0.02)}
    );
    ProductPtr product = create_product(
        "IRSwap",
        {scalar_param("notional", 1'000'000.0), vector_param("payment_times", payment_times),
         vector_param("accruals", accruals)}
    );

    // ExposureProfile y UnilateralCVA sobre el mismo caso/semillas que
    // clients/excel/README.md ("Verificación manual") -- si estos números no coinciden, algo
    // se rompió en la traducción C ABI <-> engine::Registries/IMeasure.
    std::vector<double> profile_times{0.0, 1.0, 2.0};
    MeasurePtr profile_measure = create_measure("ExposureProfile");
    MeasureResult profile = evaluate(
        profile_measure.get(), model.get(), product.get(),
        {vector_param("monitoring_times", profile_times), scalar_param("n_paths", 5000.0),
         scalar_param("seed", 7.0)}
    );
    std::cout << "ExposureProfile EE = [" << profile.get().primary[0] << ", " << profile.get().primary[1] << ", "
              << profile.get().primary[2] << "]  (esperado [0, 12862.62, 13673.53])\n";

    std::vector<double> cva_times{0.0, 1.0, 2.0, 3.0};
    MeasurePtr cva_measure = create_measure("UnilateralCVA");
    MeasureResult cva = evaluate(
        cva_measure.get(), model.get(), product.get(),
        {vector_param("monitoring_times", cva_times), scalar_param("n_paths", 5000.0), scalar_param("seed", 13.0),
         scalar_param("hazard_rate", 0.02), scalar_param("recovery_rate", 0.4)}
    );
    std::cout << "UnilateralCVA = " << cva.get().scalar << "  (esperado 426.7618244093184)\n";

    // Backend de cómputo (PLAN.md §7.12), misma ABI que el resto.
    char backend[16];
    std::size_t len = engine_abi_get_compute_backend(backend, sizeof(backend));
    std::cout << "backend: " << std::string(backend, len)
              << "  (gpu disponible: " << (engine_abi_is_gpu_backend_available() ? "si" : "no") << ")\n";

    // Manejo de errores (PLAN.md §5.5): un nombre de modelo desconocido nunca lanza/aborta al
    // otro lado de la ABI, devuelve NULL -- create_model() de este fichero lo convierte en
    // una excepción de C++, comportamiento normal a este lado.
    try {
        create_model("NoExiste", {});
        std::cerr << "ERROR: se esperaba una excepcion al pedir un modelo inexistente\n";
        return 1;
    } catch (const std::exception& e) {
        std::cout << "error esperado al pedir un modelo inexistente: " << e.what() << "\n";
    }

    std::cout << "OK: ejemplo de C++ sobre engine/abi.h completado.\n";
    return 0;
}
