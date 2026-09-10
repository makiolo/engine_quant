// Test de integracion del XLL real (PLAN.md Fase 4, §7.8): a diferencia de
// engine_excel_bridge_tests (logica pura, sin Excel12/Excel12v), este ejecutable carga el
// engine_excel.xll ya compilado con LoadLibrary y ejerce exactamente el mismo contrato que
// usa Excel -- xlAutoOpen llamando a Excel12(xlfRegister, ...) y las UDFs devolviendo
// XLOPER12* -- sin necesitar Excel instalado: el propio ejecutable exporta `MdCallBack12`
// (PLAN.md Fase 4, NOTICE.md de thirdparty/xlcall), el mismo punto de entrada que
// XLCALL.CPP busca via GetProcAddress(GetModuleHandle(NULL), "MdCallBack12") -- Excel.exe
// hace exactamente lo mismo salvo que exporta su propia implementacion real; aqui exportamos
// un stub que registra las llamadas de xlAutoOpen y responde lo minimo para que el codigo
// real de engine_excel.cpp funcione sin cambios.
//
// Cierra el hueco que engine_excel_bridge_tests no cubre (nunca llama a Excel12/Excel12v) y
// que no se pudo verificar con Excel real via automatizacion COM en el entorno de
// desarrollo (bloqueada por politicas de seguridad de ese equipo, ver PLAN.md §7.8):
// prueba el .xll compilado de verdad, con su xlAutoOpen/xlAutoFree12 reales.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "XLCALL.H"

#include <cstdio>
#include <string>
#include <vector>

namespace {

DWORD base_type(const XLOPER12& x) { return x.xltype & ~(xlbitXLFree | xlbitDLLFree); }

std::string narrow(const XLOPER12* x) {
    if (!x || base_type(*x) != xltypeStr) return {};
    XCHAR len = x->val.str[0];
    std::string out(len, '\0');
    for (XCHAR i = 0; i < len; ++i) out[i] = static_cast<char>(x->val.str[1 + i]);
    return out;
}

int g_register_calls = 0;
std::vector<std::string> g_registered_names;

} // namespace

// Simula el lado de Excel: responde a xlGetName/xlFree/xlfRegister, las 3 llamadas que hace
// xlAutoOpen (engine_excel.cpp). Cualquier otra xlfn se rechaza (no la necesita este test).
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
            ++g_register_calls;
            if (coper >= 4) g_registered_names.push_back(narrow(rgpxloper12[3])); // pxFunctionText
            if (xloper12Res) {
                xloper12Res->xltype = xltypeNum;
                xloper12Res->val.num = g_register_calls;
            }
            return xlretSuccess;
        }
        default:
            return xlretFailed;
    }
}

namespace {

bool check(bool condition, const char* message) {
    if (!condition) std::fprintf(stderr, "FALLO: %s\n", message);
    return condition;
}

} // namespace

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
    using CreateModelFn = LPXLOPER12(WINAPI*)(LPXLOPER12, LPXLOPER12);

    auto auto_open = reinterpret_cast<AutoOpenFn>(GetProcAddress(module, "xlAutoOpen"));
    auto auto_free = reinterpret_cast<AutoFreeFn>(GetProcAddress(module, "xlAutoFree12"));
    auto list_models = reinterpret_cast<ListFn>(GetProcAddress(module, "xlEngineListModels"));
    auto list_measures = reinterpret_cast<ListFn>(GetProcAddress(module, "xlEngineListMeasures"));
    auto create_model = reinterpret_cast<CreateModelFn>(GetProcAddress(module, "xlEngineCreateModel"));
    auto create_market = reinterpret_cast<CreateModelFn>(GetProcAddress(module, "xlEngineCreateMarket"));

    bool ok = check(auto_open != nullptr, "GetProcAddress(xlAutoOpen)")
        & check(auto_free != nullptr, "GetProcAddress(xlAutoFree12)")
        & check(list_models != nullptr, "GetProcAddress(xlEngineListModels)")
        & check(list_measures != nullptr, "GetProcAddress(xlEngineListMeasures)")
        & check(create_model != nullptr, "GetProcAddress(xlEngineCreateModel)")
        & check(create_market != nullptr, "GetProcAddress(xlEngineCreateMarket)");
    if (!ok) return 1;

    int rc = auto_open();
    ok &= check(rc == 1, "xlAutoOpen devuelve 1");
    ok &= check(g_register_calls == 12, "xlAutoOpen registra exactamente 12 UDFs via xlfRegister");
    for (const auto& name : g_registered_names) {
        ok &= check(name.rfind("ENGINE.", 0) == 0, "cada UDF registrada se llama ENGINE.*");
    }

    LPXLOPER12 models = list_models();
    ok &= check(models != nullptr, "xlEngineListModels devuelve un XLOPER12");
    if (models) {
        ok &= check(base_type(*models) == xltypeMulti, "xlEngineListModels devuelve xltypeMulti");
        if (base_type(*models) == xltypeMulti) {
            bool found_1f = false, found_2f = false;
            for (int i = 0; i < models->val.array.rows; ++i) {
                const std::string name = narrow(&models->val.array.lparray[i]);
                if (name == "HullWhite1F") found_1f = true;
                if (name == "HullWhite2F") found_2f = true;
            }
            ok &= check(found_1f, "\"HullWhite1F\" aparece en xlEngineListModels()");
            // Segundo modelo del motor (PLAN.md §7.16): registrado en engine::bootstrap, debe
            // aparecer aquí sin ningún cambio en engine_excel.cpp (xlEngineListModels delega
            // en Registry<IModel>::list(), no enumera modelos a mano).
            ok &= check(found_2f, "\"HullWhite2F\" aparece en xlEngineListModels()");
        }
        auto_free(models);
    }

    // PLAN.md §7.15: ENGINE.LIST_MEASURES ahora devuelve los 5 nombres de ENGINE.CALC, no los
    // nombres registrados en crudo en Registry<IMeasure>.
    LPXLOPER12 measures = list_measures();
    ok &= check(measures != nullptr, "xlEngineListMeasures devuelve un XLOPER12");
    if (measures) {
        ok &= check(base_type(*measures) == xltypeMulti, "xlEngineListMeasures devuelve xltypeMulti");
        if (base_type(*measures) == xltypeMulti) {
            bool found = false;
            for (int i = 0; i < measures->val.array.rows; ++i) {
                if (narrow(&measures->val.array.lparray[i]) == "UnilateralCVA") found = true;
            }
            ok &= check(found, "\"UnilateralCVA\" aparece en xlEngineListMeasures()");
        }
        auto_free(measures);
    }

    FreeLibrary(module);

    if (ok) {
        std::printf(
            "OK: xlAutoOpen registro %d UDFs (ENGINE.*), xlEngineListModels() incluye HullWhite1F\n",
            g_register_calls);
    }
    return ok ? 0 : 1;
}
