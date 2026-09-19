// Test de integracion del XLL real (PLAN.md Fase 4, §7.8): carga el .xll con
// LoadLibrary y ejerce xlAutoOpen/las UDFs como lo haria Excel, sin necesitar Excel
// instalado. El ejecutable exporta MdCallBack12, igual que Excel.exe, y el stub
// comprueba el contrato completo de xlfRegister.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "XLCALL.H"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

namespace {

DWORD base_type(const XLOPER12& x) { return x.xltype & ~(xlbitXLFree | xlbitDLLFree); }

std::string narrow(const XLOPER12* x) {
    if (!x || base_type(*x) != xltypeStr || !x->val.str) return {};
    XCHAR len = x->val.str[0];
    std::string out(static_cast<std::size_t>(len), '\0');
    for (XCHAR i = 0; i < len; ++i) out[static_cast<std::size_t>(i)] = static_cast<char>(x->val.str[i + 1]);
    return out;
}

struct RegisterCall {
    std::string procedure;
    std::string type_text;
    std::string function_name;
    std::string argument_text;
    int coper = 0;
    int callback_status = xlretFailed;
    DWORD result_type = 0;
    double result_num = 0.0;
};

enum class RegisterFailure {
    None,
    CallbackFailed,
    ResultError,
};

std::vector<RegisterCall> g_register_calls;
RegisterFailure g_register_failure = RegisterFailure::None;
std::string g_failure_function;
int g_next_register_id = 1;

void reset_register_stub() {
    g_register_calls.clear();
    g_register_failure = RegisterFailure::None;
    g_failure_function.clear();
    g_next_register_id = 1;
}

std::size_t argument_count(const std::string& argument_text) {
    if (argument_text.empty()) return 0;
    return 1 + static_cast<std::size_t>(std::count(argument_text.begin(), argument_text.end(), ','));
}

bool check(bool condition, const char* message) {
    if (!condition) std::fprintf(stderr, "FALLO: %s\n", message);
    return condition;
}

struct ExpectedRegistration {
    const char* procedure;
    const char* type_text;
    const char* function_name;
    const char* argument_text;
    bool table;
};

// El primer carácter Q es el tipo de retorno XLOPER12; cada argumento también es Q.
// Las 16 filas table devuelven xltypeMulti y las 9 filas scalar devuelven un valor único.
constexpr ExpectedRegistration kExpectedRegistrations[] = {
    {"xlEngineVersion", "Q", "ENGINE.VERSION", "", false},
    {"xlEngineListModels", "Q", "ENGINE.LIST_MODELS", "", true},
    {"xlEngineListProducts", "Q", "ENGINE.LIST_PRODUCTS", "", true},
    {"xlEngineListMeasures", "Q", "ENGINE.LIST_MEASURES", "", true},
    {"xlEngineCreateModel", "QQQ", "ENGINE.CREATE_MODEL", "nombre,params", false},
    {"xlEngineCreateProduct", "QQQ", "ENGINE.CREATE_PRODUCT", "nombre,params", false},
    {"xlEngineValidatePayoffSpec", "QQ", "ENGINE.VALIDATE_PAYOFF_SPEC", "spec_json", true},
    {"xlEngineExplainProduct", "QQ", "ENGINE.EXPLAIN_PRODUCT", "producto", false},
    {"xlEngineCreateMarket", "QQ", "ENGINE.CREATE_MARKET", "params", false},
    {"xlEngineCreateContext", "QQ", "ENGINE.CREATE_CONTEXT", "params", false},
    {"xlEngineCreateExecution", "QQ", "ENGINE.CREATE_EXECUTION", "params", false},
    {"xlEnginePrice", "QQQQQQQ", "ENGINE.PRICE", "trade,medidas,modelo,mercado,contexto,ejecucion", true},
    {"xlEnginePriceBatch", "QQQQQQQ", "ENGINE.PRICE_BATCH", "trades,medidas,modelo,mercado,contexto,ejecucion", true},
    {"xlEnginePriceMany", "QQQQQQQ", "ENGINE.PRICE_MANY", "trades,medidas,modelo,mercado,contexto,ejecucion", true},
    {"xlEnginePriceGrid", "QQQQQQQ", "ENGINE.PRICE_GRID", "trades,medidas,modelos,mercados,contexto,ejecucion", true},
    {"xlEngineAllGreeks", "QQQQQQQQQQ", "ENGINE.ALL_GREEKS",
     "trade,metrica,parametros_metrica,modelo,mercado,contexto,ejecucion,incluir_pillars,incluir_orden2", true},
    {"xlEngineHessian", "QQQQQQQQQ", "ENGINE.HESSIAN",
     "trade,metrica,parametros_metrica,modelo,mercado,contexto,ejecucion,factores_de_riesgo", true},
    {"xlEngineHvp", "QQQQQQQQQ", "ENGINE.HVP",
     "trade,metrica,parametros_metrica,modelo,mercado,contexto,ejecucion,direccion", true},
    {"xlEnginePortfolioCreate", "QQ", "ENGINE.PORTFOLIO.CREATE", "trades", false},
    {"xlEnginePortfolioPrice", "QQQQQQQ", "ENGINE.PORTFOLIO.PRICE",
     "portfolio,medidas,modelo,mercado,contexto,ejecucion", true},
    {"xlEnginePortfolioHessian", "QQQQQQQQQ", "ENGINE.PORTFOLIO.HESSIAN",
     "portfolio,metrica,parametros_metrica,modelo,mercado,contexto,ejecucion,factores_de_riesgo", true},
    {"xlEnginePortfolioHvp", "QQQQQQQQQ", "ENGINE.PORTFOLIO.HVP",
     "portfolio,metrica,parametros_metrica,modelo,mercado,contexto,ejecucion,direccion", true},
    {"xlEngineListCalibrators", "Q", "ENGINE.LIST_CALIBRATORS", "", true},
    {"xlEngineCreateCalibrator", "QQ", "ENGINE.CREATE_CALIBRATOR", "nombre", false},
    {"xlEngineCalibrate", "QQQQ", "ENGINE.CALIBRATE", "calibrador,mercado,estimacion_inicial", true},
};

bool validate_registration_metadata() {
    bool ok = check(
        g_register_calls.size() == std::size(kExpectedRegistrations),
        "xlAutoOpen registra exactamente 25 funciones"
    );
    std::size_t table_count = 0;
    const std::size_t count = std::min(g_register_calls.size(), std::size(kExpectedRegistrations));
    for (std::size_t i = 0; i < count; ++i) {
        const RegisterCall& actual = g_register_calls[i];
        const ExpectedRegistration& expected = kExpectedRegistrations[i];
        ok &= check(actual.coper == 7, "xlfRegister recibe exactamente 7 operandos");
        ok &= check(actual.procedure == expected.procedure, "procedure registrado correctamente");
        ok &= check(actual.type_text == expected.type_text, "type_text registrado correctamente");
        ok &= check(!actual.type_text.empty() && actual.type_text[0] == 'Q', "type_text empieza por Q");
        ok &= check(actual.function_name == expected.function_name, "function_name registrado correctamente");
        ok &= check(actual.argument_text == expected.argument_text, "argument_text registrado correctamente");
        ok &= check(
            (!actual.type_text.empty() ? actual.type_text.size() - 1 : 0) == argument_count(actual.argument_text),
            "el numero de argumentos coincide con type_text y argument_text"
        );
        ok &= check(actual.callback_status == xlretSuccess, "xlfRegister devuelve xlretSuccess");
        ok &= check(actual.result_type == xltypeNum, "xlfRegister devuelve un ID numerico");
        ok &= check(
            actual.result_num > 0.0 && std::isfinite(actual.result_num),
            "ID numerico de xlfRegister positivo y finito"
        );
        if (expected.table) ++table_count;
    }
    ok &= check(table_count == 16, "la metadata distingue exactamente 16 UDFs tabulares");
    ok &= check(std::size(kExpectedRegistrations) - table_count == 9, "la metadata distingue 9 UDFs escalares");
    return ok;
}

bool check_string_column(
    LPXLOPER12 value, const char* label, const std::vector<std::string>& expected_names
) {
    bool ok = check(value != nullptr, label);
    if (!value) return false;
    ok &= check(base_type(*value) == xltypeMulti, "la lista devuelve xltypeMulti");
    ok &= check((value->xltype & xlbitDLLFree) != 0, "la lista devuelve xlbitDLLFree");
    if (base_type(*value) != xltypeMulti) return false;
    ok &= check(value->val.array.rows > 1, "la lista tiene mas de una fila");
    ok &= check(value->val.array.columns == 1, "la lista tiene una sola columna");
    ok &= check(value->val.array.lparray != nullptr, "la lista tiene lparray no nulo");
    if (!value->val.array.lparray) return false;

    std::vector<std::string> actual_names;
    for (RW row = 0; row < value->val.array.rows; ++row) {
        const XLOPER12& cell = value->val.array.lparray[row];
        ok &= check(base_type(cell) == xltypeStr, "cada celda de la lista es xltypeStr");
        if (base_type(cell) == xltypeStr) {
            ok &= check(cell.val.str != nullptr, "cada celda string tiene buffer no nulo");
            if (cell.val.str) actual_names.push_back(narrow(&cell));
        }
    }
    std::vector<std::string> sorted_actual = actual_names;
    std::vector<std::string> sorted_expected = expected_names;
    std::sort(sorted_actual.begin(), sorted_actual.end());
    std::sort(sorted_expected.begin(), sorted_expected.end());
    ok &= check(sorted_actual.size() == sorted_expected.size(), "la lista tiene el numero exacto de nombres");
    ok &= check(sorted_actual == sorted_expected, "la lista coincide exactamente como conjunto ordenado");
    return ok;
}

} // namespace

// Simula el lado de Excel. Captura cada registro y permite comprobar que xlAutoOpen
// propaga tanto un status de callback como un resultado xltypeErr.
extern "C" __declspec(dllexport) int __stdcall MdCallBack12(
    int xlfn, int coper, LPXLOPER12* rgpxloper12, LPXLOPER12 xloper12Res
) {
    static XCHAR fake_module_name[32];
    switch (xlfn) {
        case xlGetName: {
            const wchar_t* fake = L"C:\\fake\\engine_excel.xll";
            std::size_t len = wcslen(fake);
            fake_module_name[0] = static_cast<XCHAR>(len);
            for (std::size_t i = 0; i < len; ++i) fake_module_name[i + 1] = static_cast<XCHAR>(fake[i]);
            if (xloper12Res) {
                xloper12Res->xltype = xltypeStr;
                xloper12Res->val.str = fake_module_name;
            }
            return xlretSuccess;
        }
        case xlFree:
            return xlretSuccess;
        case xlfRegister: {
            RegisterCall call;
            call.coper = coper;
            call.procedure = coper > 1 && rgpxloper12 ? narrow(rgpxloper12[1]) : std::string{};
            call.type_text = coper > 2 && rgpxloper12 ? narrow(rgpxloper12[2]) : std::string{};
            call.function_name = coper > 3 && rgpxloper12 ? narrow(rgpxloper12[3]) : std::string{};
            call.argument_text = coper > 4 && rgpxloper12 ? narrow(rgpxloper12[4]) : std::string{};

            if (call.function_name == g_failure_function
                && g_register_failure == RegisterFailure::CallbackFailed) {
                call.callback_status = xlretFailed;
                if (xloper12Res) {
                    xloper12Res->xltype = xltypeErr;
                    xloper12Res->val.err = xlerrValue;
                    call.result_type = base_type(*xloper12Res);
                }
                g_register_calls.push_back(call);
                return xlretFailed;
            }
            if (call.function_name == g_failure_function
                && g_register_failure == RegisterFailure::ResultError) {
                call.callback_status = xlretSuccess;
                if (xloper12Res) {
                    xloper12Res->xltype = xltypeErr;
                    xloper12Res->val.err = xlerrValue;
                    call.result_type = base_type(*xloper12Res);
                }
                g_register_calls.push_back(call);
                return xlretSuccess;
            }

            call.callback_status = xlretSuccess;
            if (xloper12Res) {
                xloper12Res->xltype = xltypeNum;
                xloper12Res->val.num = static_cast<double>(g_next_register_id++);
                call.result_type = base_type(*xloper12Res);
                call.result_num = xloper12Res->val.num;
            }
            g_register_calls.push_back(call);
            return xlretSuccess;
        }
        default:
            return xlretFailed;
    }
}

int main() {
#ifndef ENGINE_EXCEL_XLL_PATH
#error "ENGINE_EXCEL_XLL_PATH no definido (ver clients/excel/tests/CMakeLists.txt)"
#endif
    HMODULE module = LoadLibraryA(ENGINE_EXCEL_XLL_PATH);
    if (!check(module != nullptr, "LoadLibrary del .xll")) {
        std::fprintf(stderr, "  GetLastError=%lu path=%s\n", GetLastError(), ENGINE_EXCEL_XLL_PATH);
        return 1;
    }

    using AutoOpenFn = int(WINAPI*)();
    using AutoFreeFn = void(WINAPI*)(LPXLOPER12);
    using ListFn = LPXLOPER12(WINAPI*)();

    auto auto_open = reinterpret_cast<AutoOpenFn>(GetProcAddress(module, "xlAutoOpen"));
    auto auto_free = reinterpret_cast<AutoFreeFn>(GetProcAddress(module, "xlAutoFree12"));
    auto list_models = reinterpret_cast<ListFn>(GetProcAddress(module, "xlEngineListModels"));
    auto list_products = reinterpret_cast<ListFn>(GetProcAddress(module, "xlEngineListProducts"));
    auto list_measures = reinterpret_cast<ListFn>(GetProcAddress(module, "xlEngineListMeasures"));
    auto list_calibrators = reinterpret_cast<ListFn>(GetProcAddress(module, "xlEngineListCalibrators"));

    bool ok = check(auto_open != nullptr, "GetProcAddress(xlAutoOpen)")
        & check(auto_free != nullptr, "GetProcAddress(xlAutoFree12)")
        & check(list_models != nullptr, "GetProcAddress(xlEngineListModels)")
        & check(list_products != nullptr, "GetProcAddress(xlEngineListProducts)")
        & check(list_measures != nullptr, "GetProcAddress(xlEngineListMeasures)")
        & check(list_calibrators != nullptr, "GetProcAddress(xlEngineListCalibrators)");
    if (!ok) {
        FreeLibrary(module);
        return 1;
    }

    // Error 1: Excel rejects one registration at the callback boundary. The XLL must stop
    // immediately and report xlAutoOpen()==0 instead of claiming a partial success.
    reset_register_stub();
    g_failure_function = "ENGINE.LIST_PRODUCTS";
    g_register_failure = RegisterFailure::CallbackFailed;
    ok &= check(auto_open() == 0, "xlAutoOpen falla si MdCallBack12 devuelve xlretFailed");
    ok &= check(g_register_calls.size() == 3, "xlAutoOpen se detiene en el registro rechazado");
    ok &= check(
        !g_register_calls.empty() && g_register_calls.back().callback_status == xlretFailed,
        "se captura el status xlretFailed"
    );

    // Error 2: the callback succeeds but Excel returns an error XLOPER12 for registration.
    reset_register_stub();
    g_failure_function = "ENGINE.LIST_MEASURES";
    g_register_failure = RegisterFailure::ResultError;
    ok &= check(auto_open() == 0, "xlAutoOpen falla si xlfRegister devuelve xltypeErr");
    ok &= check(g_register_calls.size() == 4, "xlAutoOpen se detiene ante el resultado xltypeErr");
    ok &= check(
        !g_register_calls.empty() && g_register_calls.back().callback_status == xlretSuccess,
        "el callback del segundo caso tiene status success"
    );
    ok &= check(
        !g_register_calls.empty() && g_register_calls.back().result_type == xltypeErr,
        "se captura el resultado xltypeErr"
    );

    // Registro exitoso: comprobar todas las filas, el contrato de tipos y la clasificación
    // 16 tabulares / 9 escalares.
    reset_register_stub();
    ok &= check(auto_open() == 1, "xlAutoOpen devuelve 1 en registro completo");
    ok &= validate_registration_metadata();

    const std::vector<std::string> expected_models{
        "HullWhite1F", "HullWhite2F", "GBM", "GBM_P", "GbmBasket"
    };
    const std::vector<std::string> expected_products{"IRSwap", "Payoff"};
    const std::vector<std::string> expected_measures{
        "ExposureProfile", "UnilateralCVA", "PV", "HullWhiteModelNpv", "DV01",
        "PayoffPriceQ", "PayoffExerciseQ", "PayoffHitProbabilityQ", "PayoffExposureProfileQ",
        "PayoffUnilateralCvaQ", "PayoffForecastP", "PayoffHitProbabilityP", "PayoffPnlDistributionP",
        "PayoffSensitivityQ", "Greek", "ExpectedExposure", "PFE95"
    };
    const std::vector<std::string> expected_calibrators{"HullWhite1F", "HullWhite2F"};

    LPXLOPER12 models = list_models();
    ok &= check_string_column(models, "xlEngineListModels devuelve una columna de strings", expected_models);
    if (models) auto_free(models);

    LPXLOPER12 products = list_products();
    ok &= check_string_column(products, "xlEngineListProducts devuelve una columna de strings", expected_products);
    if (products) auto_free(products);

    LPXLOPER12 measures = list_measures();
    ok &= check_string_column(measures, "xlEngineListMeasures devuelve una columna de strings", expected_measures);
    if (measures) auto_free(measures);

    LPXLOPER12 calibrators = list_calibrators();
    ok &= check_string_column(
        calibrators, "xlEngineListCalibrators devuelve una columna de strings", expected_calibrators
    );
    if (calibrators) auto_free(calibrators);

    FreeLibrary(module);

    if (ok) {
        std::printf(
            "OK: registro completo de %zu UDFs, 16 tabulares, metadata y listas verificadas\n",
            g_register_calls.size()
        );
    }
    return ok ? 0 : 1;
}
