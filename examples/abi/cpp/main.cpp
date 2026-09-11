// Ejemplo de C++ consumiendo engine/abi.h (PLAN.md Fase 6, §5.5/§7.13; ENGINE.PRICE en PLAN.md
// §7.15) SIN pasar por el registry C++ interno (engine::Registries/engine::price, cpp/engine/
// include/engine/*.hpp) ni por cxx -- exactamente la misma superficie extern "C" que vería un
// consumidor externo real (Julia vía ccall, .NET vía P/Invoke, Go vía cgo). Este fichero
// envuelve esa superficie con RAII y excepciones porque el consumidor aquí sí es C++ y puede
// permitírselo; abi.h en sí no asume eso (por eso es un header C puro, ver examples/abi/c-puro
// más abajo).
//
// Compilar dentro de este repo (ver examples/abi/README.md): es un target más de
// cpp/engine/CMakeLists.txt (engine_abi_cpp_example), `cmake --build build` lo compila junto
// al resto. Fuera de este árbol de CMake, contra un engine_abi ya compilado:
//   cl /std:c++17 /EHsc /I <repo>\cpp\engine\include main.cpp <repo>\build\cpp\engine\engine_abi.lib

#include "engine/abi.h"

#include <cmath>
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

using ModelPtr = std::unique_ptr<EngineModel, ModelDeleter>;
using ProductPtr = std::unique_ptr<EngineProduct, ProductDeleter>;

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

// RAII para el array EnginePriceResultEntry* que devuelve engine_abi_price (owned por engine_abi,
// ver abi.h): se libera en el destructor con engine_abi_free_price_results, nunca con delete[]
// directo.
class PriceResults {
public:
    PriceResults(EnginePriceResultEntry* entries, std::size_t count) : entries_(entries), count_(count) {}
    ~PriceResults() { engine_abi_free_price_results(entries_, count_); }
    PriceResults(const PriceResults&) = delete;
    PriceResults& operator=(const PriceResults&) = delete;

    const EngineMeasureResult& operator[](const char* measure_name) const {
        for (std::size_t i = 0; i < count_; ++i) {
            if (std::string(entries_[i].measure_name) == measure_name) return entries_[i].result;
        }
        throw std::out_of_range(std::string("PriceResults: no se pidio la medida '") + measure_name + "'");
    }

private:
    EnginePriceResultEntry* entries_;
    std::size_t count_;
};

// Sustituye por completo create_measure/evaluate (PLAN.md §7.15): calcula un lote de medidas
// nombradas de una vez.
PriceResults price(
    EngineProduct* product, std::vector<const char*> measure_names, EngineModel* model,
    const EngineMarketSnapshot& market, const EnginePricingContext& pricing, const EngineExecutionContext& execution
) {
    EnginePriceResultEntry* entries = nullptr;
    std::size_t count = 0;
    int rc = engine_abi_price(
        product, measure_names.data(), measure_names.size(), model, &market, &pricing, &execution, &entries, &count);
    if (rc != 0) throw_last_error("engine_abi_price");
    return PriceResults(entries, count);
}

} // namespace

int main() {
    std::cout << "engine_abi_version() = " << engine_abi_version() << "\n";

    // payment_times/accruals deben seguir vivos mientras se usan los EngineParam que apuntan a
    // ellos (uno por llamada, ver vector_param); les basta con sobrevivir hasta el final de la
    // llamada correspondiente, aquí hasta el final de main().
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

    // Mismo caso base que cpp/engine/tests/test_registry.cpp (Registry.
    // UnilateralCvaMatchesGoldenValue/ExposureProfileMatchesGoldenValue), pero aquí basta con
    // invariantes cualitativos: esta sonda verifica el mecanismo de la ABI, no vuelve a fijar
    // el numero exacto.
    double pillar = 1.0, zero_rate = 0.02;
    EngineMarketSnapshot market{};
    market.pillars = &pillar;
    market.zero_rates = &zero_rate;
    market.count = 1;
    market.hazard_rate = 0.02;
    market.recovery_rate = 0.4;

    EnginePricingContext pricing{};
    pricing.pricing_date = 0.0;
    pricing.n_paths = 5000;
    pricing.n_steps = 208; // ~1 paso/semana sobre los 4 anios hasta la ultima fecha de reseteo
    pricing.seed = 7;

    EngineExecutionContext execution{};
    execution.backend = "cpu";
    execution.precision = "FP64";

    PriceResults results = price(
        product.get(), {"PV", "DV01", "ExpectedExposure", "PFE95", "UnilateralCVA"}, model.get(), market, pricing,
        execution);

    const EngineMeasureResult& pv = results["PV"];
    const EngineMeasureResult& dv01 = results["DV01"];
    const EngineMeasureResult& ee = results["ExpectedExposure"];
    const EngineMeasureResult& pfe = results["PFE95"];
    const EngineMeasureResult& cva = results["UnilateralCVA"];

    std::cout << "PV            = " << pv.scalar << "  (swap a la par: ~0)\n";
    std::cout << "DV01          = " << dv01.scalar << "  (swap pagador: > 0)\n";
    std::cout << "UnilateralCVA = " << cva.scalar << "  (> 0 con hazard_rate > 0)\n";
    for (std::size_t i = 0; i < ee.len; ++i) {
        std::cout << "  t=" << ee.times[i] << ": EE=" << ee.primary[i] << "  PFE95=" << pfe.primary[i] << "\n";
    }

    if (std::abs(pv.scalar) > 1e-6) throw std::runtime_error("PV de un swap a la par deberia ser ~0");
    if (!(dv01.scalar > 0.0) || !(cva.scalar > 0.0)) throw std::runtime_error("se esperaba DV01 > 0 y UnilateralCVA > 0");
    for (std::size_t i = 0; i < ee.len; ++i) {
        if (ee.primary[i] < 0.0 || pfe.primary[i] < ee.primary[i]) {
            throw std::runtime_error("se esperaba ExpectedExposure >= 0 y PFE95 >= ExpectedExposure");
        }
    }

    std::cout << "gpu disponible: " << (engine_abi_is_gpu_backend_available() ? "si" : "no") << "\n";

    // engine_abi_price rechaza un nombre de medida desconocido (PLAN.md §7.15): el error queda
    // en engine_abi_last_error(), nunca lanza/aborta a traves de esta frontera C -- price() de
    // este fichero lo convierte en una excepcion de C++.
    try {
        price(product.get(), {"NoExiste"}, model.get(), market, pricing, execution);
        std::cerr << "ERROR: se esperaba una excepcion con una medida desconocida\n";
        return 1;
    } catch (const std::exception& e) {
        std::cout << "error esperado al pedir una medida inexistente: " << e.what() << "\n";
    }

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

    std::cout << "OK: ejemplo de C++ sobre engine/abi.h (ENGINE.PRICE) completado.\n";
    return 0;
}
