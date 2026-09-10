// Cliente Excel (XLL, PLAN.md Fase 4, §7.8): las UDFs mas los 3 puntos de entrada que Excel
// exige de todo XLL (xlAutoOpen/xlAutoClose/xlAutoFree12). Toda la logica de traduccion vive
// en xloper.hpp/handles.hpp (testeada sin Excel, ver clients/excel/tests); este fichero es
// deliberadamente delgado.
//
// Añadir una UDF nueva son *dos* sitios, no tres:
//   1. su definicion (`extern "C" __declspec(dllexport) LPXLOPER12 WINAPI xlEngineFoo(...)`,
//      cuerpo envuelto en xlbridge::guarded para no repetir el try/catch->#VALUE!).
//   2. una fila en `kFunctions` -- ENGINE_XLL_ENTRY deriva el nombre exportado del propio
//      identificador C++ (WSTRINGIZE) y fuerza en tiempo de compilacion que esa funcion
//      exista con ese nombre exacto (`void(&fn)`), asi que un typo en la tabla no compila en
//      vez de fallar en silencio dentro de Excel con #NAME?.
// No hace falta un fichero .def: en x64 __stdcall no decora nombres (a diferencia de x86),
// asi que __declspec(dllexport) basta para que Excel resuelva el "procedure" de xlfRegister
// (y los propios xlAuto*) por GetProcAddress con el nombre exacto.

#include "xloper.hpp"
#include "handles.hpp"

#include "engine/engine.hpp"

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
    const wchar_t* procedure;    // nombre exportado, derivado de la funcion C++ (ver ENGINE_XLL_ENTRY)
    const wchar_t* type_text;    // "U" = retorno XLOPER12, "Q" = argumento XLOPER12 (PLAN.md §7.8)
    const wchar_t* name;         // nombre visible en Excel (formula bar / asistente de funciones)
    const wchar_t* argument_text;
    const wchar_t* help;
};

#define ENGINE_XLL_WIDE_(s) L##s
#define ENGINE_XLL_WIDE(s) ENGINE_XLL_WIDE_(s)
#define ENGINE_XLL_WSTRINGIZE(x) ENGINE_XLL_WIDE(#x)

// `void(&fn)` obliga a que `fn` exista con ese nombre exacto (error de compilacion, no un
// #NAME? silencioso en Excel); el operador coma lo descarta y deja el FnSpec como valor de la
// expresion, valido como inicializador de un elemento de kFunctions.
#define ENGINE_XLL_ENTRY(fn, type_text, excel_name, argument_text, help) \
    (void(&fn), FnSpec{ENGINE_XLL_WSTRINGIZE(fn), type_text, excel_name, argument_text, help})

} // namespace

// --- UDFs (PLAN.md §7.8: mismo modelo mental que engine.Engine en Python -- list_models/
// list_products/list_measures/create_model/create_product/create_measure/Measure.evaluate,
// PLAN.md §7.7 -- via los mismos Registries/Registry<T>::create/IMeasure::evaluate).
// xlbridge::guarded (xloper.hpp) centraliza el try/catch -> #VALUE!: ninguna excepcion de
// C++ puede cruzar la frontera con Excel.

extern "C" __declspec(dllexport) LPXLOPER12 WINAPI xlEngineListModels() {
    return xlbridge::guarded([] {
        return xlbridge::new_string_column(xlbridge::shared().list_models());
    });
}

extern "C" __declspec(dllexport) LPXLOPER12 WINAPI xlEngineListProducts() {
    return xlbridge::guarded([] {
        return xlbridge::new_string_column(xlbridge::shared().list_products());
    });
}

extern "C" __declspec(dllexport) LPXLOPER12 WINAPI xlEngineListMeasures() {
    return xlbridge::guarded([] {
        return xlbridge::new_string_column(xlbridge::shared().list_measures());
    });
}

extern "C" __declspec(dllexport) LPXLOPER12 WINAPI xlEngineCreateModel(LPXLOPER12 name, LPXLOPER12 params) {
    return xlbridge::guarded([&] {
        return xlbridge::new_str(xlbridge::shared().create_model(xlbridge::read_string(*name), *params));
    });
}

extern "C" __declspec(dllexport) LPXLOPER12 WINAPI xlEngineCreateProduct(LPXLOPER12 name, LPXLOPER12 params) {
    return xlbridge::guarded([&] {
        return xlbridge::new_str(xlbridge::shared().create_product(xlbridge::read_string(*name), *params));
    });
}

extern "C" __declspec(dllexport) LPXLOPER12 WINAPI xlEngineCreateMeasure(LPXLOPER12 name) {
    return xlbridge::guarded([&] {
        return xlbridge::new_str(xlbridge::shared().create_measure(xlbridge::read_string(*name)));
    });
}

extern "C" __declspec(dllexport) LPXLOPER12 WINAPI xlEngineEvaluate(
    LPXLOPER12 measure, LPXLOPER12 model, LPXLOPER12 product, LPXLOPER12 params
) {
    return xlbridge::guarded([&] {
        return xlbridge::new_measure_result(xlbridge::shared().evaluate(
            xlbridge::read_string(*measure), xlbridge::read_string(*model),
            xlbridge::read_string(*product), *params));
    });
}

// Selección de backend de cómputo (PLAN.md §7.12): a diferencia de Python (un `with` acota
// el cambio a un bloque), una hoja de Excel no tiene un equivalente de "bloque" -- por eso
// aquí es, como anticipaba el propio PLAN.md, una UDF que cambia un estado global de proceso
// que leen las llamadas siguientes a ENGINE.EVALUATE, no un argumento más de esa función.
//
// Importante para quien la use: ENGINE.EVALUATE no *depende* de la celda donde está
// ENGINE.SET_BACKEND (Excel solo recalcula una fórmula cuando cambia algo de lo que depende
// explícitamente), así que cambiar de backend y volver a pulsar Intro en la celda de
// SET_BACKEND no recalcula por sí solo las celdas EVALUATE ya existentes -- hace falta un
// recálculo manual (Ctrl+Alt+Intro) después de cambiar de backend para que reflejen el nuevo
// backend. Alternativa para quien quiera recálculo automático: hacer que ENGINE.SET_BACKEND
// devuelva el nombre del backend y meter esa celda como argumento extra (sin usarlo) de las
// fórmulas EVALUATE relevantes, para que Excel las trate como dependientes.
extern "C" __declspec(dllexport) LPXLOPER12 WINAPI xlEngineSetBackend(LPXLOPER12 name) {
    return xlbridge::guarded([&] {
        std::string requested = xlbridge::read_string(*name);
        if (!engine::set_compute_backend(requested)) {
            throw std::invalid_argument(
                "Backend de computo no disponible: '" + requested + "'");
        }
        return xlbridge::new_str(engine::compute_backend_name());
    });
}

extern "C" __declspec(dllexport) LPXLOPER12 WINAPI xlEngineGetBackend() {
    return xlbridge::guarded([] {
        return xlbridge::new_str(engine::compute_backend_name());
    });
}

// Market/calibración (PLAN.md §7.14): mismo modelo mental que list_models/create_model, pero
// ENGINE.CALIBRATE toma el mercado directamente como rango (no como handle -- un
// MarketSnapshot no necesita memoizarse, se consume una sola vez por llamada) y el resultado
// es una tabla clave/valor pensada para poder pasarse tal cual a ENGINE.CREATE_MODEL (ver
// xlbridge::new_calibration_result).

extern "C" __declspec(dllexport) LPXLOPER12 WINAPI xlEngineListCalibrators() {
    return xlbridge::guarded([] {
        return xlbridge::new_string_column(xlbridge::shared().list_calibrators());
    });
}

extern "C" __declspec(dllexport) LPXLOPER12 WINAPI xlEngineCreateCalibrator(LPXLOPER12 name) {
    return xlbridge::guarded([&] {
        return xlbridge::new_str(xlbridge::shared().create_calibrator(xlbridge::read_string(*name)));
    });
}

extern "C" __declspec(dllexport) LPXLOPER12 WINAPI xlEngineCalibrate(
    LPXLOPER12 calibrator, LPXLOPER12 market, LPXLOPER12 initial_guess
) {
    return xlbridge::guarded([&] {
        return xlbridge::new_calibration_result(
            xlbridge::shared().calibrate(xlbridge::read_string(*calibrator), *market, *initial_guess));
    });
}

// PLAN.md §5.4 (registro explicito centralizado) aplicado tambien aqui: una unica tabla que
// enumera todo lo que este XLL expone, sin auto-registro implicito. Va despues de las UDFs
// (no antes): ENGINE_XLL_ENTRY necesita verlas ya declaradas para el chequeo `void(&fn)`.
namespace {
constexpr FnSpec kFunctions[] = {
    ENGINE_XLL_ENTRY(xlEngineListModels, L"U", L"ENGINE.LIST_MODELS", L"",
                      L"Lista los modelos registrados en el motor."),
    ENGINE_XLL_ENTRY(xlEngineListProducts, L"U", L"ENGINE.LIST_PRODUCTS", L"",
                      L"Lista los productos registrados en el motor."),
    ENGINE_XLL_ENTRY(xlEngineListMeasures, L"U", L"ENGINE.LIST_MEASURES", L"",
                      L"Lista las medidas registradas en el motor."),
    ENGINE_XLL_ENTRY(xlEngineCreateModel, L"UQQ", L"ENGINE.CREATE_MODEL", L"nombre,params",
                      L"Crea un modelo (params: rango clave/valor) y devuelve su handle."),
    ENGINE_XLL_ENTRY(xlEngineCreateProduct, L"UQQ", L"ENGINE.CREATE_PRODUCT", L"nombre,params",
                      L"Crea un producto (params: rango clave/valor) y devuelve su handle."),
    ENGINE_XLL_ENTRY(xlEngineCreateMeasure, L"UQ", L"ENGINE.CREATE_MEASURE", L"nombre",
                      L"Crea una medida y devuelve su handle."),
    ENGINE_XLL_ENTRY(xlEngineEvaluate, L"UQQQQ", L"ENGINE.EVALUATE", L"medida,modelo,producto,params",
                      L"Evalua una medida sobre un modelo y un producto (params: rango clave/valor)."),
    ENGINE_XLL_ENTRY(xlEngineSetBackend, L"UQ", L"ENGINE.SET_BACKEND", L"nombre",
                      L"Selecciona el backend de computo global ('cpu'/'gpu'). Requiere recalculo "
                      L"manual (Ctrl+Alt+Intro) de las celdas EVALUATE existentes."),
    ENGINE_XLL_ENTRY(xlEngineGetBackend, L"U", L"ENGINE.GET_BACKEND", L"",
                      L"Backend de computo actualmente seleccionado ('cpu' o 'gpu')."),
    ENGINE_XLL_ENTRY(xlEngineListCalibrators, L"U", L"ENGINE.LIST_CALIBRATORS", L"",
                      L"Lista los calibradores registrados en el motor."),
    ENGINE_XLL_ENTRY(xlEngineCreateCalibrator, L"UQ", L"ENGINE.CREATE_CALIBRATOR", L"nombre",
                      L"Crea un calibrador y devuelve su handle."),
    ENGINE_XLL_ENTRY(xlEngineCalibrate, L"UQQQ", L"ENGINE.CALIBRATE", L"calibrador,mercado,estimacion_inicial",
                      L"Calibra un modelo a un mercado (rango de 2 columnas: pillars, zero_rates) "
                      L"partiendo de una estimacion inicial (rango clave/valor); el resultado se "
                      L"puede pasar tal cual a ENGINE.CREATE_MODEL."),
};
} // namespace

// --- Puntos de entrada que Excel exige de todo XLL (__declspec(dllexport): ver comentario de
// cabecera, sin fichero .def).

extern "C" __declspec(dllexport) int WINAPI xlAutoOpen() {
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

extern "C" __declspec(dllexport) int WINAPI xlAutoClose() {
    xlbridge::shared().clear();
    return 1;
}

extern "C" __declspec(dllexport) void WINAPI xlAutoFree12(LPXLOPER12 p_oper) {
    xlbridge::free_xloper(p_oper);
}
