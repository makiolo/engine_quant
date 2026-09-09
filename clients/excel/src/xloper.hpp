#pragma once

// Utilidades de bajo nivel para traducir entre XLOPER12 (API C de Excel, XLCALL.H) y los
// tipos de `engine` (PLAN.md §5.4, §7.6). Sin dependencia de Excel12/Excel12v: solo lee/
// construye estructuras XLOPER12 en memoria, por lo que es testeable con `cargo`... con
// GoogleTest, sin Excel instalado (ver clients/excel/tests/test_xloper.cpp).

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "XLCALL.H"

#include <string>
#include <utility>
#include <vector>

#include "engine/measure.hpp"
#include "engine/params.hpp"

namespace xlbridge {

// --- Codificación de cadenas Excel12: XCHAR con longitud en la posición 0, sin terminador
// nulo (PLAN.md Fase 4, §7.8: "Strings in xltypeMulti", Microsoft Learn "Memory Management
// in Excel"). std::string se asume UTF-8; la conversión usa Wide*/MultiByteToWideChar de
// Win32 (disponibles fuera de Excel, sin necesitar Excel12).
std::vector<XCHAR> to_xl_string_buffer(const std::string& utf8);
std::vector<XCHAR> to_xl_string_buffer(const wchar_t* utf16_nul_terminated);
std::string from_xl_string(const XCHAR* data, std::size_t len);

// --- Lectura de argumentos "Q" (PLAN.md Fase 4: siempre llegan como xltypeNum/Str/Bool/Err/
// Multi/Missing/Nil, nunca xltypeRef/SRef; Excel ya los deja "sin coaccionar" salvo eso).
bool is_blank(const XLOPER12& x);
double read_double(const XLOPER12& x);
bool read_bool(const XLOPER12& x);
std::string read_string(const XLOPER12& x);

// Vista rows x cols sobre un argumento Q: si es xltypeMulti, apunta a val.array.lparray; si
// es un valor suelto (usuario pasó una única celda), se trata como tabla 1x1.
struct Table {
    RW rows = 0;
    COL cols = 0;
    const XLOPER12* base = nullptr;

    const XLOPER12& cell(RW r, COL c) const { return base[static_cast<long>(r) * cols + c]; }
};
Table as_table(const XLOPER12& x);

// Resultado de interpretar un rango Q "params" (col 0 = clave, cols 1..N = valores de esa
// fila): el mismo Params que consume el registry C++ (PLAN.md §5.4), más una representación
// canónica ordenada por clave, usada para memoizar instancias por handle (ver handles.hpp).
struct ParsedParams {
    engine::Params params;
    std::string canonical;
};
ParsedParams table_to_params(const XLOPER12& params_arg);

// --- Construcción de valores de retorno: todo lo que devuelve una UDF de engine_excel.cpp
// se reserva en el heap y se marca xlbitDLLFree (PLAN.md Fase 4, §7.8: "todo lo que
// devolvemos es propiedad de la DLL"), para que Excel llame de vuelta a xlAutoFree12
// (engine_excel.cpp) y liberarlo de forma consistente vía free_xloper.
XLOPER12* new_error(int xlerr_code);
XLOPER12* new_num(double value);
XLOPER12* new_str(const std::string& utf8);
XLOPER12* new_string_column(const std::vector<std::string>& values);
XLOPER12* new_measure_result(const engine::MeasureResult& result);

// Liberación simétrica de cualquier XLOPER12 devuelto por las funciones new_* de arriba
// (incluye el recorrido recursivo de xltypeMulti, PLAN.md Fase 4 §7.8). Se invoca desde
// xlAutoFree12 (engine_excel.cpp) para cada XLOPER12 marcado xlbitDLLFree que Excel
// devuelve.
void free_xloper(XLOPER12* p);

} // namespace xlbridge
