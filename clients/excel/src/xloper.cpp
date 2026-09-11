#include "xloper.hpp"

#include <algorithm>
#include <charconv>
#include <cwchar>
#include <sstream>
#include <stdexcept>

namespace xlbridge {

namespace {

constexpr DWORD kFreeBits = xlbitXLFree | xlbitDLLFree;

DWORD base_type(const XLOPER12& x) { return x.xltype & ~kFreeBits; }

std::string format_double(double value) {
    // std::to_chars: representación decimal más corta que redondea exactamente al mismo
    // double (round-trip garantizado por el estándar), usada como parte de la clave
    // canónica de memoización de handles (handles.hpp) para que el mismo valor numérico
    // produzca siempre la misma clave, sin ambigüedad de precisión/formato.
    char buf[32];
    auto result = std::to_chars(buf, buf + sizeof(buf), value);
    return std::string(buf, result.ptr);
}

std::string format_canonical_value(const engine::ParamValue& value) {
    if (const double* d = std::get_if<double>(&value)) {
        return format_double(*d);
    }
    if (const bool* b = std::get_if<bool>(&value)) {
        return *b ? "true" : "false";
    }
    if (const std::string* s = std::get_if<std::string>(&value)) {
        return "\"" + *s + "\"";
    }
    const auto& vec = std::get<std::vector<double>>(value);
    std::ostringstream oss;
    oss << '[';
    for (std::size_t i = 0; i < vec.size(); ++i) {
        if (i) oss << ',';
        oss << format_double(vec[i]);
    }
    oss << ']';
    return oss.str();
}

} // namespace

std::vector<XCHAR> to_xl_string_buffer(const wchar_t* utf16_nul_terminated) {
    std::size_t len = std::wcslen(utf16_nul_terminated);
    if (len > 32767) len = 32767; // límite documentado de XLOPER12 xltypeStr
    std::vector<XCHAR> buf(len + 1);
    buf[0] = static_cast<XCHAR>(len);
    for (std::size_t i = 0; i < len; ++i) {
        buf[i + 1] = static_cast<XCHAR>(utf16_nul_terminated[i]);
    }
    return buf;
}

std::vector<XCHAR> to_xl_string_buffer(const std::string& utf8) {
    if (utf8.empty()) {
        std::vector<XCHAR> buf(1);
        buf[0] = 0;
        return buf;
    }
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(wlen), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), wlen);

    std::size_t len = wide.size();
    if (len > 32767) len = 32767;
    std::vector<XCHAR> buf(len + 1);
    buf[0] = static_cast<XCHAR>(len);
    for (std::size_t i = 0; i < len; ++i) {
        buf[i + 1] = static_cast<XCHAR>(wide[i]);
    }
    return buf;
}

std::string from_xl_string(const XCHAR* data, std::size_t len) {
    if (len == 0) return {};
    int blen = WideCharToMultiByte(
        CP_UTF8, 0, reinterpret_cast<const wchar_t*>(data), static_cast<int>(len),
        nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(blen), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, reinterpret_cast<const wchar_t*>(data), static_cast<int>(len),
        out.data(), blen, nullptr, nullptr);
    return out;
}

bool is_blank(const XLOPER12& x) {
    DWORD t = base_type(x);
    return t == xltypeMissing || t == xltypeNil;
}

double read_double(const XLOPER12& x) {
    if (base_type(x) != xltypeNum) {
        throw std::invalid_argument("xlbridge: se esperaba un numero");
    }
    return x.val.num;
}

bool read_bool(const XLOPER12& x) {
    if (base_type(x) != xltypeBool) {
        throw std::invalid_argument("xlbridge: se esperaba un booleano");
    }
    return x.val.xbool != 0;
}

std::string read_string(const XLOPER12& x) {
    if (base_type(x) != xltypeStr) {
        throw std::invalid_argument("xlbridge: se esperaba una cadena");
    }
    XCHAR len = x.val.str[0];
    return from_xl_string(x.val.str + 1, static_cast<std::size_t>(len));
}

std::vector<std::string> read_string_list(const XLOPER12& x) {
    std::vector<std::string> out;
    Table tbl = as_table(x);
    for (RW r = 0; r < tbl.rows; ++r) {
        for (COL c = 0; c < tbl.cols; ++c) {
            const XLOPER12& cell = tbl.cell(r, c);
            if (is_blank(cell)) continue;
            out.push_back(read_string(cell));
        }
    }
    return out;
}

Table as_table(const XLOPER12& x) {
    Table tbl;
    if (base_type(x) == xltypeMulti) {
        tbl.rows = x.val.array.rows;
        tbl.cols = x.val.array.columns;
        tbl.base = x.val.array.lparray;
    } else {
        tbl.rows = 1;
        tbl.cols = 1;
        tbl.base = &x;
    }
    return tbl;
}

ParsedParams table_to_params(const XLOPER12& params_arg) {
    ParsedParams out;
    if (is_blank(params_arg)) {
        return out; // rango omitido: sin parametros (igual que create_measure(name) en Python)
    }

    Table tbl = as_table(params_arg);
    std::vector<std::pair<std::string, std::string>> canonical_entries;

    for (RW r = 0; r < tbl.rows; ++r) {
        const XLOPER12& key_cell = tbl.cell(r, 0);
        if (is_blank(key_cell)) continue; // fila vacia: se ignora

        std::string key = read_string(key_cell);

        std::vector<double> nums;
        bool have_bool = false;
        bool bool_value = false;
        bool have_string = false;
        std::string string_value;
        int non_blank = 0;

        for (COL c = 1; c < tbl.cols; ++c) {
            const XLOPER12& v = tbl.cell(r, c);
            if (is_blank(v)) continue;
            ++non_blank;
            DWORD vt = base_type(v);
            if (vt == xltypeBool) {
                have_bool = true;
                bool_value = read_bool(v);
            } else if (vt == xltypeNum) {
                nums.push_back(v.val.num);
            } else if (vt == xltypeStr) {
                have_string = true;
                string_value = read_string(v);
            } else {
                throw std::invalid_argument("xlbridge: valor no soportado para la clave '" + key + "'");
            }
        }

        if (non_blank == 0) {
            throw std::invalid_argument("xlbridge: fila sin valor para la clave '" + key + "'");
        }

        engine::ParamValue value;
        if (have_string && nums.empty() && !have_bool && non_blank == 1) {
            // ExecutionContext (PLAN.md §7.15: "backend"/"precision") es el único consumidor
            // de valores de texto en un rango de parámetros hoy.
            value = string_value;
        } else if (have_bool && nums.empty() && !have_string && non_blank == 1) {
            value = bool_value;
        } else if (!nums.empty() && !have_bool && !have_string) {
            value = (nums.size() == 1) ? engine::ParamValue(nums[0]) : engine::ParamValue(nums);
        } else {
            throw std::invalid_argument("xlbridge: valores mixtos para la clave '" + key + "'");
        }

        canonical_entries.emplace_back(key, format_canonical_value(value));
        out.params.emplace(std::move(key), std::move(value));
    }

    std::sort(canonical_entries.begin(), canonical_entries.end());
    std::ostringstream oss;
    for (std::size_t i = 0; i < canonical_entries.size(); ++i) {
        if (i) oss << ';';
        oss << canonical_entries[i].first << '=' << canonical_entries[i].second;
    }
    out.canonical = oss.str();
    return out;
}

XLOPER12* new_error(int xlerr_code) {
    XLOPER12* out = new XLOPER12{};
    out->xltype = xltypeErr | xlbitDLLFree;
    out->val.err = xlerr_code;
    return out;
}

XLOPER12* new_num(double value) {
    XLOPER12* out = new XLOPER12{};
    out->xltype = xltypeNum | xlbitDLLFree;
    out->val.num = value;
    return out;
}

XLOPER12* new_str(const std::string& utf8) {
    std::vector<XCHAR> buf = to_xl_string_buffer(utf8);
    XCHAR* owned = new XCHAR[buf.size()];
    std::copy(buf.begin(), buf.end(), owned);

    XLOPER12* out = new XLOPER12{};
    out->xltype = xltypeStr | xlbitDLLFree;
    out->val.str = owned;
    return out;
}

XLOPER12* new_string_column(const std::vector<std::string>& values) {
    if (values.empty()) return new_error(xlerrNA);

    RW n = static_cast<RW>(values.size());
    XLOPER12* cells = new XLOPER12[static_cast<std::size_t>(n)]{};
    for (RW i = 0; i < n; ++i) {
        std::vector<XCHAR> buf = to_xl_string_buffer(values[static_cast<std::size_t>(i)]);
        XCHAR* owned = new XCHAR[buf.size()];
        std::copy(buf.begin(), buf.end(), owned);
        cells[i].xltype = xltypeStr;
        cells[i].val.str = owned;
    }

    XLOPER12* out = new XLOPER12{};
    out->xltype = xltypeMulti | xlbitDLLFree;
    out->val.array.rows = n;
    out->val.array.columns = 1;
    out->val.array.lparray = cells;
    return out;
}

XLOPER12* new_measure_result(const engine::MeasureResult& result) {
    if (result.has_scalar) {
        return new_num(result.scalar);
    }

    RW n = static_cast<RW>(result.times.size());
    if (n == 0) return new_error(xlerrNA);

    XLOPER12* cells = new XLOPER12[static_cast<std::size_t>(n) * 3]{};
    for (RW i = 0; i < n; ++i) {
        cells[i * 3 + 0].xltype = xltypeNum;
        cells[i * 3 + 0].val.num = result.times[static_cast<std::size_t>(i)];
        cells[i * 3 + 1].xltype = xltypeNum;
        cells[i * 3 + 1].val.num = result.primary[static_cast<std::size_t>(i)];
        cells[i * 3 + 2].xltype = xltypeNum;
        cells[i * 3 + 2].val.num = result.secondary[static_cast<std::size_t>(i)];
    }

    XLOPER12* out = new XLOPER12{};
    out->xltype = xltypeMulti | xlbitDLLFree;
    out->val.array.rows = n;
    out->val.array.columns = 3;
    out->val.array.lparray = cells;
    return out;
}

XLOPER12* new_price_result(const engine::PriceResult& result) {
    RW total_rows = 0;
    for (const auto& entry : result) {
        total_rows += entry.result.has_scalar ? 1 : static_cast<RW>(entry.result.times.size());
    }
    if (total_rows == 0) return new_error(xlerrNA);

    XLOPER12* cells = new XLOPER12[static_cast<std::size_t>(total_rows) * 3]{};
    RW row = 0;
    for (const auto& entry : result) {
        auto write_name = [&](RW r) {
            std::vector<XCHAR> buf = to_xl_string_buffer(entry.measure_name);
            XCHAR* owned = new XCHAR[buf.size()];
            std::copy(buf.begin(), buf.end(), owned);
            cells[r * 3 + 0].xltype = xltypeStr;
            cells[r * 3 + 0].val.str = owned;
        };

        if (entry.result.has_scalar) {
            write_name(row);
            cells[row * 3 + 1].xltype = xltypeNil; // Time en blanco: medida escalar
            cells[row * 3 + 2].xltype = xltypeNum;
            cells[row * 3 + 2].val.num = entry.result.scalar;
            ++row;
        } else {
            // ExpectedExposure/PFE95 (PLAN.md §7.15): price.cpp ya copia la serie pedida
            // (primary o secondary del origen) al campo `primary` de este resultado, así que
            // basta con leer siempre `primary` aquí sin saber cuál era el campo de origen.
            for (std::size_t i = 0; i < entry.result.times.size(); ++i) {
                write_name(row);
                cells[row * 3 + 1].xltype = xltypeNum;
                cells[row * 3 + 1].val.num = entry.result.times[i];
                cells[row * 3 + 2].xltype = xltypeNum;
                cells[row * 3 + 2].val.num = entry.result.primary[i];
                ++row;
            }
        }
    }

    XLOPER12* out = new XLOPER12{};
    out->xltype = xltypeMulti | xlbitDLLFree;
    out->val.array.rows = total_rows;
    out->val.array.columns = 3;
    out->val.array.lparray = cells;
    return out;
}

namespace {

// Numero de filas que ocupa un engine::PriceResult en formato largo (una por medida escalar,
// una por fecha de las de perfil) -- compartido por new_price_batch_result/new_price_grid_result
// (PLAN.md §7.19), mismo criterio que ya usa new_price_result inline.
std::size_t calc_result_row_count(const engine::PriceResult& result) {
    std::size_t n = 0;
    for (const auto& entry : result) n += entry.result.has_scalar ? 1 : entry.result.times.size();
    return n;
}

// Escribe las columnas [MeasureName, Time, Value] de un PriceResult en `cells`, empezando en la
// fila `start_row` y en la columna `index_columns` (las columnas de indice -- TradeIndex, o
// TradeIndex+ModelIndex+MarketIndex -- ya escritas a su izquierda por el llamante). Devuelve la
// fila siguiente libre. `total_columns` es el ancho total de la tabla (para el stride
// row*total_columns).
RW write_calc_result_rows(
    XLOPER12* cells, RW start_row, COL total_columns, COL index_columns, const engine::PriceResult& result
) {
    RW row = start_row;
    for (const auto& entry : result) {
        auto write_name = [&](RW r) {
            std::vector<XCHAR> buf = to_xl_string_buffer(entry.measure_name);
            XCHAR* owned = new XCHAR[buf.size()];
            std::copy(buf.begin(), buf.end(), owned);
            cells[r * total_columns + index_columns + 0].xltype = xltypeStr;
            cells[r * total_columns + index_columns + 0].val.str = owned;
        };

        if (entry.result.has_scalar) {
            write_name(row);
            cells[row * total_columns + index_columns + 1].xltype = xltypeNil;
            cells[row * total_columns + index_columns + 2].xltype = xltypeNum;
            cells[row * total_columns + index_columns + 2].val.num = entry.result.scalar;
            ++row;
        } else {
            for (std::size_t i = 0; i < entry.result.times.size(); ++i) {
                write_name(row);
                cells[row * total_columns + index_columns + 1].xltype = xltypeNum;
                cells[row * total_columns + index_columns + 1].val.num = entry.result.times[i];
                cells[row * total_columns + index_columns + 2].xltype = xltypeNum;
                cells[row * total_columns + index_columns + 2].val.num = entry.result.primary[i];
                ++row;
            }
        }
    }
    return row;
}

} // namespace

XLOPER12* new_price_batch_result(const engine::PriceBatchResult& result) {
    RW total_rows = 0;
    for (const auto& entry : result) total_rows += static_cast<RW>(calc_result_row_count(entry.measures));
    if (total_rows == 0) return new_error(xlerrNA);

    constexpr COL kColumns = 4; // TradeIndex, MeasureName, Time, Value
    XLOPER12* cells = new XLOPER12[static_cast<std::size_t>(total_rows) * kColumns]{};
    RW row = 0;
    for (const auto& entry : result) {
        RW rows_for_trade = static_cast<RW>(calc_result_row_count(entry.measures));
        for (RW r = row; r < row + rows_for_trade; ++r) {
            cells[r * kColumns + 0].xltype = xltypeNum;
            cells[r * kColumns + 0].val.num = static_cast<double>(entry.trade_index);
        }
        row = write_calc_result_rows(cells, row, kColumns, 1, entry.measures);
    }

    XLOPER12* out = new XLOPER12{};
    out->xltype = xltypeMulti | xlbitDLLFree;
    out->val.array.rows = total_rows;
    out->val.array.columns = kColumns;
    out->val.array.lparray = cells;
    return out;
}

XLOPER12* new_price_grid_result(const engine::PriceGridResult& result) {
    RW total_rows = 0;
    for (const auto& entry : result) total_rows += static_cast<RW>(calc_result_row_count(entry.measures));
    if (total_rows == 0) return new_error(xlerrNA);

    constexpr COL kColumns = 6; // TradeIndex, ModelIndex, MarketIndex, MeasureName, Time, Value
    XLOPER12* cells = new XLOPER12[static_cast<std::size_t>(total_rows) * kColumns]{};
    RW row = 0;
    for (const auto& entry : result) {
        RW rows_for_cell = static_cast<RW>(calc_result_row_count(entry.measures));
        for (RW r = row; r < row + rows_for_cell; ++r) {
            cells[r * kColumns + 0].xltype = xltypeNum;
            cells[r * kColumns + 0].val.num = static_cast<double>(entry.trade_index);
            cells[r * kColumns + 1].xltype = xltypeNum;
            cells[r * kColumns + 1].val.num = static_cast<double>(entry.model_index);
            cells[r * kColumns + 2].xltype = xltypeNum;
            cells[r * kColumns + 2].val.num = static_cast<double>(entry.market_index);
        }
        row = write_calc_result_rows(cells, row, kColumns, 3, entry.measures);
    }

    XLOPER12* out = new XLOPER12{};
    out->xltype = xltypeMulti | xlbitDLLFree;
    out->val.array.rows = total_rows;
    out->val.array.columns = kColumns;
    out->val.array.lparray = cells;
    return out;
}

XLOPER12* new_calibration_result(const engine::CalibrationResult& result) {
    engine::Params rows = result.optimal_params;
    rows.emplace("rmse", result.rmse);
    rows.emplace("iterations", static_cast<double>(result.iterations));
    rows.emplace("converged", result.converged);

    RW n = static_cast<RW>(rows.size());
    if (n == 0) return new_error(xlerrNA);

    XLOPER12* cells = new XLOPER12[static_cast<std::size_t>(n) * 2]{};
    RW row = 0;
    for (const auto& [key, value] : rows) {
        std::vector<XCHAR> key_buf = to_xl_string_buffer(key);
        XCHAR* key_owned = new XCHAR[key_buf.size()];
        std::copy(key_buf.begin(), key_buf.end(), key_owned);
        cells[row * 2 + 0].xltype = xltypeStr;
        cells[row * 2 + 0].val.str = key_owned;

        if (const double* d = std::get_if<double>(&value)) {
            cells[row * 2 + 1].xltype = xltypeNum;
            cells[row * 2 + 1].val.num = *d;
        } else {
            // El único otro caso posible aquí es "converged" (bool) -- optimal_params de
            // HullWhite1FCalibrator solo produce doubles (a/b/sigma/r0), ver calibrator.hpp.
            cells[row * 2 + 1].xltype = xltypeBool;
            cells[row * 2 + 1].val.xbool = std::get<bool>(value) ? 1 : 0;
        }
        ++row;
    }

    XLOPER12* out = new XLOPER12{};
    out->xltype = xltypeMulti | xlbitDLLFree;
    out->val.array.rows = n;
    out->val.array.columns = 2;
    out->val.array.lparray = cells;
    return out;
}

void free_xloper(XLOPER12* p) {
    if (!p) return;
    DWORD t = base_type(*p);
    if (t == xltypeStr) {
        delete[] p->val.str;
    } else if (t == xltypeMulti) {
        long count = static_cast<long>(p->val.array.rows) * static_cast<long>(p->val.array.columns);
        XLOPER12* arr = p->val.array.lparray;
        for (long i = 0; i < count; ++i) {
            if (base_type(arr[i]) == xltypeStr) {
                delete[] arr[i].val.str;
            }
        }
        delete[] arr;
    }
    delete p;
}

} // namespace xlbridge
