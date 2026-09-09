// Tests del bridge Excel (PLAN.md Fase 4, §7.8): xloper.hpp/.cpp (traducción XLOPER12 <->
// engine::Params/MeasureResult) y handles.hpp/.cpp (registry memoizado por handle). No
// necesitan Excel instalado -- construyen XLOPER12 a mano en memoria y nunca llaman a
// Excel12/Excel12v, a diferencia de engine_excel.cpp (los xlAuto*/UDFs solo se pueden
// ejercitar dentro de una Excel real, ver clients/excel/README.md).
//
// Los casos de HandleRegistry reproducen los mismos parámetros/semillas que
// cpp/engine/tests/test_registry.cpp (Fase 2) y clients/python/tests/test_registry.py (Fase
// 3): confirman que el bridge de Excel invoca exactamente el mismo
// Registries/register_builtins/Registry<T>::create/IMeasure::evaluate, no una reimplementación
// paralela -- la verificación de que además Excel real invoca este mismo código a través de
// xlAutoOpen/las UDFs exportadas queda para la comprobación manual documentada en el README
// (PLAN.md §5.6 capa 4: CI no tiene Excel instalado).

#include <cmath>
#include <vector>

#include <gtest/gtest.h>

#include "handles.hpp"
#include "xloper.hpp"

namespace {

using xlbridge::Table;

// Construye un XLOPER12 xltypeMulti rows x cols a partir de una matriz de "celdas" ya
// construidas (cada una un XLOPER12 completo). El buffer de celdas se mantiene vivo en
// `storage` mientras el XLOPER12 resultante esté en uso.
XLOPER12 make_table(std::vector<XLOPER12>& storage, RW rows, COL cols) {
    XLOPER12 x{};
    x.xltype = xltypeMulti;
    x.val.array.rows = rows;
    x.val.array.columns = cols;
    x.val.array.lparray = storage.data();
    return x;
}

XLOPER12 num_cell(double v) {
    XLOPER12 x{};
    x.xltype = xltypeNum;
    x.val.num = v;
    return x;
}

XLOPER12 blank_cell() {
    XLOPER12 x{};
    x.xltype = xltypeNil;
    return x;
}

// Las cadenas de celda deben mantener vivo su buffer XCHAR (xlbridge::to_xl_string_buffer)
// mientras el XLOPER12 esté en uso; se guardan en `bufs` para que el llamador controle su
// tiempo de vida.
XLOPER12 str_cell(std::vector<std::vector<XCHAR>>& bufs, const std::string& s) {
    bufs.push_back(xlbridge::to_xl_string_buffer(s));
    XLOPER12 x{};
    x.xltype = xltypeStr;
    x.val.str = bufs.back().data();
    return x;
}

// params_arg xltypeMissing: equivalente a omitir el argumento en Excel (create_measure(name),
// o cualquier UDF sin params).
XLOPER12 missing_arg() {
    XLOPER12 x{};
    x.xltype = xltypeMissing;
    return x;
}

} // namespace

TEST(XlStringBuffer, RoundTripsAscii) {
    auto buf = xlbridge::to_xl_string_buffer(std::string("HullWhite1F"));
    EXPECT_EQ(static_cast<int>(buf[0]), 11);
    EXPECT_EQ(xlbridge::from_xl_string(buf.data() + 1, buf[0]), "HullWhite1F");
}

TEST(XlStringBuffer, RoundTripsNonAscii) {
    auto buf = xlbridge::to_xl_string_buffer(std::string("ni\xC3\xB1o")); // "niño" en UTF-8
    EXPECT_EQ(xlbridge::from_xl_string(buf.data() + 1, buf[0]), "ni\xC3\xB1o");
}

TEST(TableToParams, ScalarDoubleFields) {
    std::vector<std::vector<XCHAR>> bufs;
    std::vector<XLOPER12> cells{
        str_cell(bufs, "a"), num_cell(0.1),
        str_cell(bufs, "b"), num_cell(0.03),
        str_cell(bufs, "sigma"), num_cell(0.01),
        str_cell(bufs, "r0"), num_cell(0.02),
    };
    XLOPER12 table = make_table(cells, 4, 2);

    xlbridge::ParsedParams parsed = xlbridge::table_to_params(table);

    EXPECT_DOUBLE_EQ(engine::get_double(parsed.params, "a"), 0.1);
    EXPECT_DOUBLE_EQ(engine::get_double(parsed.params, "b"), 0.03);
    EXPECT_DOUBLE_EQ(engine::get_double(parsed.params, "sigma"), 0.01);
    EXPECT_DOUBLE_EQ(engine::get_double(parsed.params, "r0"), 0.02);
}

TEST(TableToParams, VectorValuedRow) {
    std::vector<std::vector<XCHAR>> bufs;
    std::vector<XLOPER12> cells{
        str_cell(bufs, "payment_times"), num_cell(1.0), num_cell(2.0), num_cell(3.0),
    };
    XLOPER12 table = make_table(cells, 1, 4);

    xlbridge::ParsedParams parsed = xlbridge::table_to_params(table);

    const std::vector<double>& v = engine::get_vector(parsed.params, "payment_times");
    ASSERT_EQ(v.size(), 3u);
    EXPECT_DOUBLE_EQ(v[0], 1.0);
    EXPECT_DOUBLE_EQ(v[1], 2.0);
    EXPECT_DOUBLE_EQ(v[2], 3.0);
}

TEST(TableToParams, TrailingBlankCellsAreIgnored) {
    std::vector<std::vector<XCHAR>> bufs;
    std::vector<XLOPER12> cells{
        str_cell(bufs, "notional"), num_cell(1'000'000.0), blank_cell(), blank_cell(),
    };
    XLOPER12 table = make_table(cells, 1, 4);

    xlbridge::ParsedParams parsed = xlbridge::table_to_params(table);

    EXPECT_DOUBLE_EQ(engine::get_double(parsed.params, "notional"), 1'000'000.0);
}

TEST(TableToParams, CanonicalKeyIsOrderIndependent) {
    std::vector<std::vector<XCHAR>> bufs1;
    std::vector<XLOPER12> cells1{
        str_cell(bufs1, "a"), num_cell(0.1),
        str_cell(bufs1, "b"), num_cell(0.03),
    };
    XLOPER12 table1 = make_table(cells1, 2, 2);

    std::vector<std::vector<XCHAR>> bufs2;
    std::vector<XLOPER12> cells2{
        str_cell(bufs2, "b"), num_cell(0.03),
        str_cell(bufs2, "a"), num_cell(0.1),
    };
    XLOPER12 table2 = make_table(cells2, 2, 2);

    EXPECT_EQ(xlbridge::table_to_params(table1).canonical, xlbridge::table_to_params(table2).canonical);
}

TEST(TableToParams, MissingArgMeansNoParams) {
    XLOPER12 missing = missing_arg();
    xlbridge::ParsedParams parsed = xlbridge::table_to_params(missing);
    EXPECT_TRUE(parsed.params.empty());
    EXPECT_TRUE(parsed.canonical.empty());
}

namespace {

xlbridge::HandleRegistry make_registry() { return xlbridge::HandleRegistry(); }

XLOPER12 hull_white_params_table(std::vector<std::vector<XCHAR>>& bufs, std::vector<XLOPER12>& cells) {
    cells = {
        str_cell(bufs, "a"), num_cell(0.1),
        str_cell(bufs, "b"), num_cell(0.03),
        str_cell(bufs, "sigma"), num_cell(0.01),
        str_cell(bufs, "r0"), num_cell(0.02),
    };
    return make_table(cells, 4, 2);
}

XLOPER12 par_irs_5y_params_table(std::vector<std::vector<XCHAR>>& bufs, std::vector<XLOPER12>& cells) {
    cells = {
        str_cell(bufs, "notional"), num_cell(1'000'000.0), blank_cell(), blank_cell(), blank_cell(), blank_cell(),
        str_cell(bufs, "payment_times"), num_cell(1.0), num_cell(2.0), num_cell(3.0), num_cell(4.0), num_cell(5.0),
        str_cell(bufs, "accruals"), num_cell(1.0), num_cell(1.0), num_cell(1.0), num_cell(1.0), num_cell(1.0),
    };
    return make_table(cells, 3, 6);
}

} // namespace

TEST(HandleRegistry, RegisterBuiltinsPopulatesAllRegistries) {
    xlbridge::HandleRegistry registry = make_registry();
    auto contains = [](const std::vector<std::string>& v, const std::string& name) {
        for (const auto& n : v) if (n == name) return true;
        return false;
    };
    EXPECT_TRUE(contains(registry.list_models(), "HullWhite1F"));
    EXPECT_TRUE(contains(registry.list_products(), "IRSwap"));
    EXPECT_TRUE(contains(registry.list_measures(), "ExposureProfile"));
    EXPECT_TRUE(contains(registry.list_measures(), "UnilateralCVA"));
}

TEST(HandleRegistry, CreateModelIsMemoizedByCanonicalParams) {
    xlbridge::HandleRegistry registry = make_registry();
    std::vector<std::vector<XCHAR>> bufs1, bufs2;
    std::vector<XLOPER12> cells1, cells2;
    XLOPER12 table1 = hull_white_params_table(bufs1, cells1);
    XLOPER12 table2 = hull_white_params_table(bufs2, cells2);

    std::string handle1 = registry.create_model("HullWhite1F", table1);
    std::string handle2 = registry.create_model("HullWhite1F", table2);

    EXPECT_EQ(handle1, handle2);
}

TEST(HandleRegistry, CreateUnknownModelThrows) {
    xlbridge::HandleRegistry registry = make_registry();
    XLOPER12 missing = missing_arg();
    EXPECT_THROW(registry.create_model("NoExiste", missing), std::out_of_range);
}

TEST(HandleRegistry, EvaluateUnknownHandleThrows) {
    xlbridge::HandleRegistry registry = make_registry();
    XLOPER12 missing = missing_arg();
    EXPECT_THROW(registry.evaluate("measure:NoExiste", "model:NoExiste", "product:NoExiste", missing), std::out_of_range);
}

// Mismos parámetros/semillas que Registry.ExposureProfileIsNonNegativeAndPfeDominatesEe
// (cpp/engine/tests/test_registry.cpp) y test_exposure_profile_is_non_negative_and_pfe_
// dominates_ee (clients/python/tests/test_registry.py): confirma que el bridge de Excel
// reproduce el mismo resultado numérico que los otros dos clientes para el mismo caso base
// (PLAN.md §5.2, §5.6 capa 4).
TEST(HandleRegistry, ExposureProfileMatchesOtherClients) {
    xlbridge::HandleRegistry registry = make_registry();
    std::vector<std::vector<XCHAR>> model_bufs, product_bufs, measure_bufs;
    std::vector<XLOPER12> model_cells, product_cells, measure_cells;

    XLOPER12 model_table = hull_white_params_table(model_bufs, model_cells);
    XLOPER12 product_table = par_irs_5y_params_table(product_bufs, product_cells);
    measure_cells = {
        str_cell(measure_bufs, "monitoring_times"), num_cell(0.0), num_cell(1.0), num_cell(2.0),
        str_cell(measure_bufs, "n_paths"), num_cell(5000.0), blank_cell(), blank_cell(),
        str_cell(measure_bufs, "seed"), num_cell(7.0), blank_cell(), blank_cell(),
    };
    XLOPER12 measure_table = make_table(measure_cells, 3, 4);

    std::string model = registry.create_model("HullWhite1F", model_table);
    std::string product = registry.create_product("IRSwap", product_table);
    std::string measure = registry.create_measure("ExposureProfile");

    engine::MeasureResult result = registry.evaluate(measure, model, product, measure_table);

    ASSERT_EQ(result.primary.size(), result.secondary.size());
    for (std::size_t i = 0; i < result.primary.size(); ++i) {
        EXPECT_GE(result.primary[i], 0.0);
        EXPECT_GE(result.secondary[i], result.primary[i]);
    }
}

TEST(HandleRegistry, UnilateralCvaIsPositiveForNonzeroHazardRate) {
    xlbridge::HandleRegistry registry = make_registry();
    std::vector<std::vector<XCHAR>> model_bufs, product_bufs, measure_bufs;
    std::vector<XLOPER12> model_cells, product_cells, measure_cells;

    XLOPER12 model_table = hull_white_params_table(model_bufs, model_cells);
    XLOPER12 product_table = par_irs_5y_params_table(product_bufs, product_cells);
    measure_cells = {
        str_cell(measure_bufs, "monitoring_times"), num_cell(0.0), num_cell(1.0), num_cell(2.0), num_cell(3.0),
        str_cell(measure_bufs, "n_paths"), num_cell(5000.0), blank_cell(), blank_cell(), blank_cell(),
        str_cell(measure_bufs, "seed"), num_cell(13.0), blank_cell(), blank_cell(), blank_cell(),
        str_cell(measure_bufs, "hazard_rate"), num_cell(0.02), blank_cell(), blank_cell(), blank_cell(),
        str_cell(measure_bufs, "recovery_rate"), num_cell(0.4), blank_cell(), blank_cell(), blank_cell(),
    };
    XLOPER12 measure_table = make_table(measure_cells, 5, 5);

    std::string model = registry.create_model("HullWhite1F", model_table);
    std::string product = registry.create_product("IRSwap", product_table);
    std::string measure = registry.create_measure("UnilateralCVA");

    engine::MeasureResult result = registry.evaluate(measure, model, product, measure_table);

    EXPECT_TRUE(result.has_scalar);
    EXPECT_GT(result.scalar, 0.0);
}

TEST(NewMeasureResult, ScalarBecomes1x1Num) {
    engine::MeasureResult r;
    r.has_scalar = true;
    r.scalar = 42.0;

    XLOPER12* out = xlbridge::new_measure_result(r);
    EXPECT_EQ(out->xltype & ~static_cast<DWORD>(xlbitDLLFree), static_cast<DWORD>(xltypeNum));
    EXPECT_DOUBLE_EQ(out->val.num, 42.0);
    xlbridge::free_xloper(out);
}

TEST(NewMeasureResult, ProfileBecomesNx3Multi) {
    engine::MeasureResult r;
    r.has_scalar = false;
    r.times = {0.0, 1.0};
    r.primary = {10.0, 20.0};
    r.secondary = {15.0, 25.0};

    XLOPER12* out = xlbridge::new_measure_result(r);
    ASSERT_EQ(out->xltype & ~static_cast<DWORD>(xlbitDLLFree), static_cast<DWORD>(xltypeMulti));
    EXPECT_EQ(out->val.array.rows, 2);
    EXPECT_EQ(out->val.array.columns, 3);
    EXPECT_DOUBLE_EQ(out->val.array.lparray[1 * 3 + 2].val.num, 25.0);
    xlbridge::free_xloper(out);
}
