// Cliente Excel (XLL, PLAN.md Fase 4, §7.8): las 7 UDFs mas los 3 puntos de entrada que Excel
// exige de todo XLL (xlAutoOpen/xlAutoClose/xlAutoFree12). Toda la logica de traduccion vive
// en xloper.hpp/handles.hpp (testeada sin Excel, ver clients/excel/tests); este fichero es
// deliberadamente delgado: registra las UDFs contra la tabla `kFunctions` y les da forma de
// "try/catch -> #VALUE!" (ninguna excepcion de C++ puede cruzar la frontera con Excel, PLAN.md
// Fase 4 §7.8).

#include "xloper.hpp"
#include "handles.hpp"

#include <vector>

namespace {

// Construye un XLOPER12 xltypeStr *local* (no reservado en el heap, no marcado xlbitDLLFree):
// vive mientras el buffer que referencia siga vivo, pensado solo para argumentos que nosotros
// enviamos a Excel (xlfRegister), nunca para valores de retorno de una UDF (esos usan
// xlbridge::new_str, en el heap, ver xloper.hpp).
XLOPER12 local_str(std::vector<XCHAR>& storage, const wchar_t* text) {
    storage = xlbridge::to_xl_string_buffer(text);
    XLOPER12 x{};
    x.xltype = xltypeStr;
    x.val.str = storage.data();
    return x;
}

struct FnSpec {
    const wchar_t* procedure;    // nombre exportado en engine_excel.def
    const wchar_t* type_text;    // PLAN.md Fase 4 §7.8: "U" = retorno XLOPER12, "Q" = argumento XLOPER12
    const wchar_t* name;         // nombre visible en Excel (formula bar / asistente de funciones)
    const wchar_t* argument_text;
    const wchar_t* help;
};

// PLAN.md §5.4 (registro explicito centralizado) aplicado tambien aqui: una unica tabla que
// enumera todo lo que este XLL expone, sin auto-registro implicito.
constexpr FnSpec kFunctions[] = {
    {L"xlEngineListModels", L"U", L"ENGINE.LIST_MODELS", L"",
     L"Lista los modelos registrados en el motor."},
    {L"xlEngineListProducts", L"U", L"ENGINE.LIST_PRODUCTS", L"",
     L"Lista los productos registrados en el motor."},
    {L"xlEngineListMeasures", L"U", L"ENGINE.LIST_MEASURES", L"",
     L"Lista las medidas registradas en el motor."},
    {L"xlEngineCreateModel", L"UQQ", L"ENGINE.CREATE_MODEL", L"nombre,params",
     L"Crea un modelo (params: rango clave/valor) y devuelve su handle."},
    {L"xlEngineCreateProduct", L"UQQ", L"ENGINE.CREATE_PRODUCT", L"nombre,params",
     L"Crea un producto (params: rango clave/valor) y devuelve su handle."},
    {L"xlEngineCreateMeasure", L"UQ", L"ENGINE.CREATE_MEASURE", L"nombre",
     L"Crea una medida y devuelve su handle."},
    {L"xlEngineEvaluate", L"UQQQQ", L"ENGINE.EVALUATE", L"medida,modelo,producto,params",
     L"Evalua una medida sobre un modelo y un producto (params: rango clave/valor)."},
};

} // namespace

// --- UDFs (PLAN.md §7.8: mismo modelo mental que engine.Engine en Python -- list_models/
// list_products/list_measures/create_model/create_product/create_measure/Measure.evaluate,
// PLAN.md §7.7 -- via los mismos Registries/Registry<T>::create/IMeasure::evaluate).

extern "C" LPXLOPER12 WINAPI xlEngineListModels() {
    try {
        return xlbridge::new_string_column(xlbridge::shared().list_models());
    } catch (...) {
        return xlbridge::new_error(xlerrValue);
    }
}

extern "C" LPXLOPER12 WINAPI xlEngineListProducts() {
    try {
        return xlbridge::new_string_column(xlbridge::shared().list_products());
    } catch (...) {
        return xlbridge::new_error(xlerrValue);
    }
}

extern "C" LPXLOPER12 WINAPI xlEngineListMeasures() {
    try {
        return xlbridge::new_string_column(xlbridge::shared().list_measures());
    } catch (...) {
        return xlbridge::new_error(xlerrValue);
    }
}

extern "C" LPXLOPER12 WINAPI xlEngineCreateModel(LPXLOPER12 name, LPXLOPER12 params) {
    try {
        std::string handle = xlbridge::shared().create_model(xlbridge::read_string(*name), *params);
        return xlbridge::new_str(handle);
    } catch (...) {
        return xlbridge::new_error(xlerrValue);
    }
}

extern "C" LPXLOPER12 WINAPI xlEngineCreateProduct(LPXLOPER12 name, LPXLOPER12 params) {
    try {
        std::string handle = xlbridge::shared().create_product(xlbridge::read_string(*name), *params);
        return xlbridge::new_str(handle);
    } catch (...) {
        return xlbridge::new_error(xlerrValue);
    }
}

extern "C" LPXLOPER12 WINAPI xlEngineCreateMeasure(LPXLOPER12 name) {
    try {
        std::string handle = xlbridge::shared().create_measure(xlbridge::read_string(*name));
        return xlbridge::new_str(handle);
    } catch (...) {
        return xlbridge::new_error(xlerrValue);
    }
}

extern "C" LPXLOPER12 WINAPI xlEngineEvaluate(
    LPXLOPER12 measure, LPXLOPER12 model, LPXLOPER12 product, LPXLOPER12 params
) {
    try {
        engine::MeasureResult result = xlbridge::shared().evaluate(
            xlbridge::read_string(*measure), xlbridge::read_string(*model),
            xlbridge::read_string(*product), *params);
        return xlbridge::new_measure_result(result);
    } catch (...) {
        return xlbridge::new_error(xlerrValue);
    }
}

// --- Puntos de entrada que Excel exige de todo XLL (engine_excel.def controla que se exporten
// sin decorar, PLAN.md Fase 4 §7.8).

extern "C" int WINAPI xlAutoOpen() {
    XLOPER12 module_name{};
    Excel12(xlGetName, &module_name, 0); // ruta de este XLL, para pxModuleText (DLL-only)

    for (const FnSpec& fn : kFunctions) {
        std::vector<XCHAR> b_proc, b_type, b_name, b_args, b_category;
        XLOPER12 procedure = local_str(b_proc, fn.procedure);
        XLOPER12 type_text = local_str(b_type, fn.type_text);
        XLOPER12 function_name = local_str(b_name, fn.name);
        XLOPER12 argument_text = local_str(b_args, fn.argument_text);
        XLOPER12 category = local_str(b_category, L"Motor XVA");
        XLOPER12 macro_type{};
        macro_type.xltype = xltypeNum;
        macro_type.val.num = 1.0; // funcion de hoja de calculo (PLAN.md Fase 4 §7.8)

        XLOPER12 result{};
        Excel12(
            xlfRegister, &result, 7,
            &module_name, &procedure, &type_text, &function_name, &argument_text,
            &macro_type, &category);
    }

    Excel12(xlFree, nullptr, 1, &module_name);
    return 1;
}

extern "C" int WINAPI xlAutoClose() {
    xlbridge::shared().clear();
    return 1;
}

extern "C" void WINAPI xlAutoFree12(LPXLOPER12 p_oper) {
    xlbridge::free_xloper(p_oper);
}
