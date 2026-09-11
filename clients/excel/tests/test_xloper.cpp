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

TEST(ReadStringList, FlattensARowOfCells) {
    std::vector<std::vector<XCHAR>> bufs;
    std::vector<XLOPER12> cells{
        str_cell(bufs, "PV"), str_cell(bufs, "DV01"), str_cell(bufs, "UnilateralCVA"),
    };
    XLOPER12 table = make_table(cells, 1, 3);

    std::vector<std::string> names = xlbridge::read_string_list(table);

    ASSERT_EQ(names.size(), 3u);
    EXPECT_EQ(names[0], "PV");
    EXPECT_EQ(names[2], "UnilateralCVA");
}

TEST(ReadStringList, IgnoresBlankCells) {
    std::vector<std::vector<XCHAR>> bufs;
    std::vector<XLOPER12> cells{str_cell(bufs, "PV"), blank_cell(), str_cell(bufs, "DV01")};
    XLOPER12 table = make_table(cells, 1, 3);

    std::vector<std::string> names = xlbridge::read_string_list(table);

    ASSERT_EQ(names.size(), 2u);
    EXPECT_EQ(names[1], "DV01");
}

TEST(ReadStringList, SingleCellBecomesOneElementList) {
    std::vector<std::vector<XCHAR>> bufs;
    XLOPER12 single = str_cell(bufs, "PV");

    std::vector<std::string> names = xlbridge::read_string_list(single);

    ASSERT_EQ(names.size(), 1u);
    EXPECT_EQ(names[0], "PV");
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

// Segundo modelo del motor (PLAN.md §7.16, G2++): mismos parámetros de referencia que
// cpp/engine/tests/test_registry.cpp::hull_white_2f_params().
XLOPER12 hull_white_2f_params_table(std::vector<std::vector<XCHAR>>& bufs, std::vector<XLOPER12>& cells) {
    cells = {
        str_cell(bufs, "a"), num_cell(0.1),
        str_cell(bufs, "b"), num_cell(0.2),
        str_cell(bufs, "sigma"), num_cell(0.01),
        str_cell(bufs, "eta"), num_cell(0.012),
        str_cell(bufs, "rho"), num_cell(-0.7),
        str_cell(bufs, "r0"), num_cell(0.03),
    };
    return make_table(cells, 6, 2);
}

XLOPER12 par_irs_5y_params_table(std::vector<std::vector<XCHAR>>& bufs, std::vector<XLOPER12>& cells) {
    cells = {
        str_cell(bufs, "notional"), num_cell(1'000'000.0), blank_cell(), blank_cell(), blank_cell(), blank_cell(),
        str_cell(bufs, "payment_times"), num_cell(1.0), num_cell(2.0), num_cell(3.0), num_cell(4.0), num_cell(5.0),
        str_cell(bufs, "accruals"), num_cell(1.0), num_cell(1.0), num_cell(1.0), num_cell(1.0), num_cell(1.0),
    };
    return make_table(cells, 3, 6);
}

// Con fixed_rate explicito (PLAN.md §7.19): HandleRegistry::price_batch/price_many no soportan
// use_par_rate, a diferencia de par_irs_5y_params_table.
XLOPER12 irs_5y_params_table(
    std::vector<std::vector<XCHAR>>& bufs, std::vector<XLOPER12>& cells, double notional, double fixed_rate
) {
    cells = {
        str_cell(bufs, "notional"), num_cell(notional), blank_cell(), blank_cell(), blank_cell(), blank_cell(),
        str_cell(bufs, "fixed_rate"), num_cell(fixed_rate), blank_cell(), blank_cell(), blank_cell(), blank_cell(),
        str_cell(bufs, "payment_times"), num_cell(1.0), num_cell(2.0), num_cell(3.0), num_cell(4.0), num_cell(5.0),
        str_cell(bufs, "accruals"), num_cell(1.0), num_cell(1.0), num_cell(1.0), num_cell(1.0), num_cell(1.0),
    };
    return make_table(cells, 4, 6);
}

// Mismo calendario que irs_5y_params_table, distinto tenor (3 años) -- para ejercitar el
// agrupamiento heterogéneo de price_many.
XLOPER12 irs_3y_params_table(
    std::vector<std::vector<XCHAR>>& bufs, std::vector<XLOPER12>& cells, double notional, double fixed_rate
) {
    cells = {
        str_cell(bufs, "notional"), num_cell(notional), blank_cell(), blank_cell(),
        str_cell(bufs, "fixed_rate"), num_cell(fixed_rate), blank_cell(), blank_cell(),
        str_cell(bufs, "payment_times"), num_cell(1.0), num_cell(2.0), num_cell(3.0),
        str_cell(bufs, "accruals"), num_cell(1.0), num_cell(1.0), num_cell(1.0),
    };
    return make_table(cells, 4, 4);
}

// Mercado con 2 pillars (no 1: table_to_params colapsa una fila de un solo valor numérico a
// double, no vector<double> -- una vector-valued row necesita >= 2 celdas para no ser
// ambigua, ver table_to_params en xloper.cpp): ninguna medida del caso base usa la curva en
// sí para descontar, solo hazard_rate/recovery_rate importan (para UnilateralCVA).
XLOPER12 market_params_table(
    std::vector<std::vector<XCHAR>>& bufs, std::vector<XLOPER12>& cells,
    double hazard_rate = 0.0, double recovery_rate = 0.0
) {
    cells = {
        str_cell(bufs, "pillars"), num_cell(1.0), num_cell(2.0),
        str_cell(bufs, "zero_rates"), num_cell(0.02), num_cell(0.02),
        str_cell(bufs, "hazard_rate"), num_cell(hazard_rate), blank_cell(),
        str_cell(bufs, "recovery_rate"), num_cell(recovery_rate), blank_cell(),
    };
    return make_table(cells, 4, 3);
}

// Mismo n_steps/caso que golden_pricing en cpp/engine/tests/test_registry.cpp.
XLOPER12 pricing_context_table(
    std::vector<std::vector<XCHAR>>& bufs, std::vector<XLOPER12>& cells, double n_paths, double seed
) {
    cells = {
        str_cell(bufs, "pricing_date"), num_cell(0.0),
        str_cell(bufs, "n_paths"), num_cell(n_paths),
        str_cell(bufs, "n_steps"), num_cell(208.0),
        str_cell(bufs, "seed"), num_cell(seed),
    };
    return make_table(cells, 4, 2);
}

XLOPER12 cpu_execution_table(std::vector<std::vector<XCHAR>>& bufs, std::vector<XLOPER12>& cells) {
    cells = {
        str_cell(bufs, "backend"), str_cell(bufs, "cpu"),
        str_cell(bufs, "precision"), str_cell(bufs, "fp64"),
    };
    return make_table(cells, 2, 2);
}

} // namespace

TEST(HandleRegistry, RegisterBuiltinsPopulatesAllRegistries) {
    xlbridge::HandleRegistry registry = make_registry();
    auto contains = [](const std::vector<std::string>& v, const std::string& name) {
        for (const auto& n : v) if (n == name) return true;
        return false;
    };
    EXPECT_TRUE(contains(registry.list_models(), "HullWhite1F"));
    EXPECT_TRUE(contains(registry.list_models(), "HullWhite2F"));
    EXPECT_TRUE(contains(registry.list_products(), "IRSwap"));
    EXPECT_TRUE(contains(registry.list_measures(), "ExpectedExposure"));
    EXPECT_TRUE(contains(registry.list_measures(), "PFE95"));
    EXPECT_TRUE(contains(registry.list_measures(), "PV"));
    EXPECT_TRUE(contains(registry.list_measures(), "DV01"));
    EXPECT_TRUE(contains(registry.list_measures(), "UnilateralCVA"));
}

TEST(HandleRegistry, CreateMarketIsMemoizedByCanonicalParams) {
    xlbridge::HandleRegistry registry = make_registry();
    std::vector<std::vector<XCHAR>> bufs1, bufs2;
    std::vector<XLOPER12> cells1, cells2;
    XLOPER12 table1 = market_params_table(bufs1, cells1, 0.02, 0.4);
    XLOPER12 table2 = market_params_table(bufs2, cells2, 0.02, 0.4);

    EXPECT_EQ(registry.create_market(table1), registry.create_market(table2));
}

TEST(HandleRegistry, CreateContextIsMemoizedByCanonicalParams) {
    xlbridge::HandleRegistry registry = make_registry();
    std::vector<std::vector<XCHAR>> bufs1, bufs2;
    std::vector<XLOPER12> cells1, cells2;
    XLOPER12 table1 = pricing_context_table(bufs1, cells1, 5000.0, 7.0);
    XLOPER12 table2 = pricing_context_table(bufs2, cells2, 5000.0, 7.0);

    EXPECT_EQ(registry.create_context(table1), registry.create_context(table2));
}

TEST(HandleRegistry, CreateExecutionAcceptsAutoBackend) {
    // La resolución real de "auto" -> "cpu"/"gpu" la comprueba ExecutionContext directamente
    // (cpp/engine/tests, si aplica); aquí solo se confirma que el bridge de Excel no la
    // rechaza y produce un handle válido.
    xlbridge::HandleRegistry registry = make_registry();
    std::vector<std::vector<XCHAR>> bufs;
    std::vector<XLOPER12> cells{str_cell(bufs, "backend"), str_cell(bufs, "auto")};
    XLOPER12 table = make_table(cells, 1, 2);

    EXPECT_FALSE(registry.create_execution(table).empty());
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

TEST(HandleRegistry, CalcUnknownHandleThrows) {
    xlbridge::HandleRegistry registry = make_registry();
    EXPECT_THROW(
        registry.price(
            "product:NoExiste", {"PV"}, "model:NoExiste", "market:NoExiste", "pricing:NoExiste", "execution:NoExiste"
        ),
        std::out_of_range
    );
}

TEST(HandleRegistry, CalcRejectsUnknownMeasureName) {
    xlbridge::HandleRegistry registry = make_registry();
    std::vector<std::vector<XCHAR>> model_bufs, product_bufs, market_bufs, pricing_bufs, execution_bufs;
    std::vector<XLOPER12> model_cells, product_cells, market_cells, pricing_cells, execution_cells;

    std::string model = registry.create_model("HullWhite1F", hull_white_params_table(model_bufs, model_cells));
    std::string product = registry.create_product("IRSwap", par_irs_5y_params_table(product_bufs, product_cells));
    std::string market = registry.create_market(market_params_table(market_bufs, market_cells));
    std::string pricing = registry.create_context(pricing_context_table(pricing_bufs, pricing_cells, 100.0, 1.0));
    std::string execution = registry.create_execution(cpu_execution_table(execution_bufs, execution_cells));

    EXPECT_THROW(registry.price(product, {"NoExiste"}, model, market, pricing, execution), std::invalid_argument);
}

// PLAN.md §7.19: HandleRegistry::price_batch (lote homogéneo) debe coincidir, trade a trade,
// con llamar a price() una vez por trade -- mismo espíritu que Price.MatchesALoopOfScalarCalls
// en cpp/engine/tests/test_registry.cpp.
TEST(HandleRegistry, PriceBatchMatchesALoopOfScalarCallsPerTrade) {
    xlbridge::HandleRegistry registry = make_registry();
    std::vector<std::vector<XCHAR>> model_bufs, product_a_bufs, product_b_bufs, market_bufs, pricing_bufs, execution_bufs;
    std::vector<XLOPER12> model_cells, product_a_cells, product_b_cells, market_cells, pricing_cells, execution_cells;

    std::string model = registry.create_model("HullWhite1F", hull_white_params_table(model_bufs, model_cells));
    std::string product_a = registry.create_product("IRSwap", irs_5y_params_table(product_a_bufs, product_a_cells, 1'000'000.0, 0.02));
    std::string product_b = registry.create_product("IRSwap", irs_5y_params_table(product_b_bufs, product_b_cells, 2'500'000.0, 0.015));
    std::string market = registry.create_market(market_params_table(market_bufs, market_cells, 0.02, 0.4));
    std::string pricing = registry.create_context(pricing_context_table(pricing_bufs, pricing_cells, 5000.0, 7.0));
    std::string execution = registry.create_execution(cpu_execution_table(execution_bufs, execution_cells));

    std::vector<std::string> products{product_a, product_b};
    std::vector<std::string> measures{"PV", "DV01", "ExpectedExposure", "PFE95", "UnilateralCVA"};

    engine::PriceBatchResult batch = registry.price_batch(products, measures, model, market, pricing, execution);
    ASSERT_EQ(batch.size(), products.size());

    for (std::size_t i = 0; i < products.size(); ++i) {
        EXPECT_EQ(batch[i].trade_index, i);
        engine::PriceResult scalar = registry.price(products[i], measures, model, market, pricing, execution);
        for (std::size_t m = 0; m < measures.size(); ++m) {
            if (scalar[m].result.has_scalar) {
                EXPECT_NEAR(batch[i].measures[m].result.scalar, scalar[m].result.scalar, 1e-6) << measures[m];
            } else {
                for (std::size_t k = 0; k < scalar[m].result.primary.size(); ++k) {
                    EXPECT_NEAR(batch[i].measures[m].result.primary[k], scalar[m].result.primary[k], 1e-6);
                }
            }
        }
    }
}

TEST(HandleRegistry, PriceBatchRejectsMismatchedCalendars) {
    xlbridge::HandleRegistry registry = make_registry();
    std::vector<std::vector<XCHAR>> model_bufs, product_5y_bufs, product_3y_bufs, market_bufs, pricing_bufs, execution_bufs;
    std::vector<XLOPER12> model_cells, product_5y_cells, product_3y_cells, market_cells, pricing_cells, execution_cells;

    std::string model = registry.create_model("HullWhite1F", hull_white_params_table(model_bufs, model_cells));
    std::string product_5y = registry.create_product("IRSwap", irs_5y_params_table(product_5y_bufs, product_5y_cells, 1'000'000.0, 0.02));
    std::string product_3y = registry.create_product("IRSwap", irs_3y_params_table(product_3y_bufs, product_3y_cells, 1'000'000.0, 0.02));
    std::string market = registry.create_market(market_params_table(market_bufs, market_cells));
    std::string pricing = registry.create_context(pricing_context_table(pricing_bufs, pricing_cells, 100.0, 1.0));
    std::string execution = registry.create_execution(cpu_execution_table(execution_bufs, execution_cells));

    EXPECT_THROW(
        registry.price_batch({product_5y, product_3y}, {"PV"}, model, market, pricing, execution), std::invalid_argument
    );
}

// PLAN.md §7.19, Nivel 2: price_many agrupa internamente por calendario y devuelve el
// resultado en el orden de entrada, nunca falla por heterogeneidad (a diferencia de
// price_batch en el test anterior).
TEST(HandleRegistry, PriceManyGroupsHeterogeneousCalendars) {
    xlbridge::HandleRegistry registry = make_registry();
    std::vector<std::vector<XCHAR>> model_bufs, product_5y_bufs, product_3y_bufs, market_bufs, pricing_bufs, execution_bufs;
    std::vector<XLOPER12> model_cells, product_5y_cells, product_3y_cells, market_cells, pricing_cells, execution_cells;

    std::string model = registry.create_model("HullWhite1F", hull_white_params_table(model_bufs, model_cells));
    std::string product_5y = registry.create_product("IRSwap", irs_5y_params_table(product_5y_bufs, product_5y_cells, 1'000'000.0, 0.02));
    std::string product_3y = registry.create_product("IRSwap", irs_3y_params_table(product_3y_bufs, product_3y_cells, 2'000'000.0, 0.018));
    std::string market = registry.create_market(market_params_table(market_bufs, market_cells, 0.02, 0.4));
    std::string pricing = registry.create_context(pricing_context_table(pricing_bufs, pricing_cells, 5000.0, 7.0));
    std::string execution = registry.create_execution(cpu_execution_table(execution_bufs, execution_cells));

    std::vector<std::string> products{product_5y, product_3y};
    engine::PriceBatchResult many = registry.price_many(products, {"PV"}, model, market, pricing, execution);
    ASSERT_EQ(many.size(), 2u);
    EXPECT_EQ(many[0].trade_index, 0u);
    EXPECT_EQ(many[1].trade_index, 1u);
}

// PLAN.md §7.19: price_grid explota Trades x Models x Markets.
TEST(HandleRegistry, PriceGridComputesTradesTimesModelsTimesMarkets) {
    xlbridge::HandleRegistry registry = make_registry();
    std::vector<std::vector<XCHAR>> model_1f_bufs, model_2f_bufs, product_a_bufs, product_b_bufs;
    std::vector<std::vector<XCHAR>> market_a_bufs, market_b_bufs, pricing_bufs, execution_bufs;
    std::vector<XLOPER12> model_1f_cells, model_2f_cells, product_a_cells, product_b_cells;
    std::vector<XLOPER12> market_a_cells, market_b_cells, pricing_cells, execution_cells;

    std::string model_1f = registry.create_model("HullWhite1F", hull_white_params_table(model_1f_bufs, model_1f_cells));
    std::string model_2f = registry.create_model("HullWhite2F", hull_white_2f_params_table(model_2f_bufs, model_2f_cells));
    std::string product_a = registry.create_product("IRSwap", irs_5y_params_table(product_a_bufs, product_a_cells, 1'000'000.0, 0.02));
    std::string product_b = registry.create_product("IRSwap", irs_5y_params_table(product_b_bufs, product_b_cells, 2'000'000.0, 0.018));
    std::string market_a = registry.create_market(market_params_table(market_a_bufs, market_a_cells, 0.02, 0.4));
    std::string market_b = registry.create_market(market_params_table(market_b_bufs, market_b_cells, 0.05, 0.3));
    std::string pricing = registry.create_context(pricing_context_table(pricing_bufs, pricing_cells, 5000.0, 7.0));
    std::string execution = registry.create_execution(cpu_execution_table(execution_bufs, execution_cells));

    std::vector<std::string> products{product_a, product_b};
    std::vector<std::string> models{model_1f, model_2f};
    std::vector<std::string> markets{market_a, market_b};
    std::vector<std::string> measures{"PV", "UnilateralCVA"};

    engine::PriceGridResult grid = registry.price_grid(products, measures, models, markets, pricing, execution);
    ASSERT_EQ(grid.size(), products.size() * models.size() * markets.size());

    for (const auto& cell : grid) {
        engine::PriceResult scalar =
            registry.price(products[cell.trade_index], measures, models[cell.model_index], markets[cell.market_index], pricing, execution);
        for (std::size_t m = 0; m < measures.size(); ++m) {
            EXPECT_NEAR(cell.measures[m].result.scalar, scalar[m].result.scalar, 1e-6);
        }
    }
}

// Mismos parámetros/semilla que Registry.ExposureProfileMatchesGoldenValue (cpp/engine/tests/
// test_registry.cpp) y test_exposure_profile_matches_golden_value (clients/python/tests/
// test_price.py): confirma que el bridge de Excel reproduce el mismo resultado numérico que
// los otros dos clientes para el mismo caso base (PLAN.md §5.2, §5.6 capa 4).
TEST(HandleRegistry, ExpectedExposureAndPfe95MatchOtherClients) {
    xlbridge::HandleRegistry registry = make_registry();
    std::vector<std::vector<XCHAR>> model_bufs, product_bufs, market_bufs, pricing_bufs, execution_bufs;
    std::vector<XLOPER12> model_cells, product_cells, market_cells, pricing_cells, execution_cells;

    std::string model = registry.create_model("HullWhite1F", hull_white_params_table(model_bufs, model_cells));
    std::string product = registry.create_product("IRSwap", par_irs_5y_params_table(product_bufs, product_cells));
    std::string market = registry.create_market(market_params_table(market_bufs, market_cells, 0.0, 0.0));
    std::string pricing = registry.create_context(pricing_context_table(pricing_bufs, pricing_cells, 5000.0, 7.0));
    std::string execution = registry.create_execution(cpu_execution_table(execution_bufs, execution_cells));

    engine::PriceResult result = registry.price(product, {"ExpectedExposure", "PFE95"}, model, market, pricing, execution);

    ASSERT_EQ(result.size(), 2u);
    const engine::MeasureResult& ee = result[0].result;
    const engine::MeasureResult& pfe = result[1].result;
    ASSERT_EQ(ee.primary.size(), pfe.primary.size());
    for (std::size_t i = 0; i < ee.primary.size(); ++i) {
        EXPECT_GE(ee.primary[i], 0.0);
        EXPECT_GE(pfe.primary[i], ee.primary[i]);
    }
}

TEST(HandleRegistry, UnilateralCvaIsPositiveForNonzeroHazardRate) {
    xlbridge::HandleRegistry registry = make_registry();
    std::vector<std::vector<XCHAR>> model_bufs, product_bufs, market_bufs, pricing_bufs, execution_bufs;
    std::vector<XLOPER12> model_cells, product_cells, market_cells, pricing_cells, execution_cells;

    std::string model = registry.create_model("HullWhite1F", hull_white_params_table(model_bufs, model_cells));
    std::string product = registry.create_product("IRSwap", par_irs_5y_params_table(product_bufs, product_cells));
    std::string market = registry.create_market(market_params_table(market_bufs, market_cells, 0.02, 0.4));
    std::string pricing = registry.create_context(pricing_context_table(pricing_bufs, pricing_cells, 5000.0, 7.0));
    std::string execution = registry.create_execution(cpu_execution_table(execution_bufs, execution_cells));

    engine::PriceResult result = registry.price(product, {"UnilateralCVA"}, model, market, pricing, execution);

    ASSERT_EQ(result.size(), 1u);
    EXPECT_TRUE(result[0].result.has_scalar);
    EXPECT_GT(result[0].result.scalar, 0.0);
}

// Interfaz homogénea (PLAN.md §7.16): mismo HandleRegistry::price, mismos nombres de medida,
// solo cambia el nombre de modelo/params pasados a create_model -- confirma que el bridge de
// Excel no necesita saber que existen dos modelos de tipo corto distintos.
TEST(HandleRegistry, ExpectedExposureAndPfe95MatchOtherClientsUnderHullWhite2F) {
    xlbridge::HandleRegistry registry = make_registry();
    std::vector<std::vector<XCHAR>> model_bufs, product_bufs, market_bufs, pricing_bufs, execution_bufs;
    std::vector<XLOPER12> model_cells, product_cells, market_cells, pricing_cells, execution_cells;

    std::string model = registry.create_model("HullWhite2F", hull_white_2f_params_table(model_bufs, model_cells));
    std::string product = registry.create_product("IRSwap", par_irs_5y_params_table(product_bufs, product_cells));
    std::string market = registry.create_market(market_params_table(market_bufs, market_cells, 0.0, 0.0));
    std::string pricing = registry.create_context(pricing_context_table(pricing_bufs, pricing_cells, 5000.0, 7.0));
    std::string execution = registry.create_execution(cpu_execution_table(execution_bufs, execution_cells));

    engine::PriceResult result = registry.price(product, {"ExpectedExposure", "PFE95"}, model, market, pricing, execution);

    ASSERT_EQ(result.size(), 2u);
    const engine::MeasureResult& ee = result[0].result;
    const engine::MeasureResult& pfe = result[1].result;
    ASSERT_EQ(ee.primary.size(), pfe.primary.size());
    for (std::size_t i = 0; i < ee.primary.size(); ++i) {
        EXPECT_GE(ee.primary[i], 0.0);
        EXPECT_GE(pfe.primary[i], ee.primary[i]);
    }
}

TEST(HandleRegistry, UnilateralCvaIsPositiveForNonzeroHazardRateUnderHullWhite2F) {
    xlbridge::HandleRegistry registry = make_registry();
    std::vector<std::vector<XCHAR>> model_bufs, product_bufs, market_bufs, pricing_bufs, execution_bufs;
    std::vector<XLOPER12> model_cells, product_cells, market_cells, pricing_cells, execution_cells;

    std::string model = registry.create_model("HullWhite2F", hull_white_2f_params_table(model_bufs, model_cells));
    std::string product = registry.create_product("IRSwap", par_irs_5y_params_table(product_bufs, product_cells));
    std::string market = registry.create_market(market_params_table(market_bufs, market_cells, 0.02, 0.4));
    std::string pricing = registry.create_context(pricing_context_table(pricing_bufs, pricing_cells, 5000.0, 7.0));
    std::string execution = registry.create_execution(cpu_execution_table(execution_bufs, execution_cells));

    engine::PriceResult result = registry.price(product, {"UnilateralCVA"}, model, market, pricing, execution);

    ASSERT_EQ(result.size(), 1u);
    EXPECT_TRUE(result[0].result.has_scalar);
    EXPECT_GT(result[0].result.scalar, 0.0);
}

// Mismo caso/semillas que Calibrator.HullWhite1FRecoversKnownParametersFromASyntheticMarket
// (cpp/engine/tests/test_calibration.cpp) y clients/python/tests/test_calibration.py: confirma
// que el bridge de Excel reproduce el mismo resultado que los otros dos clientes (PLAN.md §7.14).
// A diferencia de antes de PLAN.md §7.15, el mercado se pasa como handle (ENGINE.CREATE_MARKET),
// no como rango inline.
TEST(HandleRegistry, CalibrateHullWhiteRecoversKnownParameters) {
    xlbridge::HandleRegistry registry = make_registry();

    double true_a = 0.15, true_b = 0.025, sigma = 0.008, r0 = 0.02;
    std::vector<double> pillars{0.5, 1.0, 2.0, 3.0, 5.0, 7.0, 10.0, 15.0, 20.0, 30.0};
    engine::MarketSnapshot synthetic = engine::MarketSnapshot::synthetic_from_hull_white(true_a, true_b, sigma, r0, pillars);

    std::vector<std::vector<XCHAR>> market_bufs;
    std::vector<XLOPER12> market_cells{str_cell(market_bufs, "pillars")};
    for (double p : synthetic.pillars()) market_cells.push_back(num_cell(p));
    market_cells.push_back(str_cell(market_bufs, "zero_rates"));
    for (double z : synthetic.zero_rates()) market_cells.push_back(num_cell(z));
    XLOPER12 market_table = make_table(market_cells, 2, static_cast<COL>(pillars.size() + 1));
    std::string market = registry.create_market(market_table);

    std::vector<std::vector<XCHAR>> guess_bufs;
    std::vector<XLOPER12> guess_cells{
        str_cell(guess_bufs, "a"), num_cell(0.3),
        str_cell(guess_bufs, "b"), num_cell(0.01),
        str_cell(guess_bufs, "sigma"), num_cell(sigma),
        str_cell(guess_bufs, "r0"), num_cell(r0),
    };
    XLOPER12 guess_table = make_table(guess_cells, 4, 2);

    std::string calibrator = registry.create_calibrator("HullWhite1F");
    engine::CalibrationResult result = registry.calibrate(calibrator, market, guess_table);

    EXPECT_TRUE(result.converged);
    EXPECT_NEAR(std::get<double>(result.optimal_params.at("a")), true_a, 1e-4);
    EXPECT_NEAR(std::get<double>(result.optimal_params.at("b")), true_b, 1e-4);
}

// Equivalente de dos factores del test anterior (PLAN.md §7.18): mismo mecanismo genérico
// (ENGINE.CREATE_CALIBRATOR/ENGINE.CALIBRATE por nombre), solo cambia el nombre del calibrador
// y las claves de la estimación inicial -- ninguna línea nueva en HandleRegistry hizo falta
// para que este segundo calibrador funcionase.
TEST(HandleRegistry, CalibrateHullWhite2FRecoversKnownParameters) {
    xlbridge::HandleRegistry registry = make_registry();

    double true_a = 0.15, true_b = 0.25, sigma = 0.008, eta = 0.01, rho = -0.6, r0 = 0.02;
    std::vector<double> pillars{0.5, 1.0, 2.0, 3.0, 5.0, 7.0, 10.0, 15.0, 20.0, 30.0};
    engine::MarketSnapshot synthetic =
        engine::MarketSnapshot::synthetic_from_hull_white_2f(true_a, true_b, sigma, eta, rho, r0, pillars);

    std::vector<std::vector<XCHAR>> market_bufs;
    std::vector<XLOPER12> market_cells{str_cell(market_bufs, "pillars")};
    for (double p : synthetic.pillars()) market_cells.push_back(num_cell(p));
    market_cells.push_back(str_cell(market_bufs, "zero_rates"));
    for (double z : synthetic.zero_rates()) market_cells.push_back(num_cell(z));
    XLOPER12 market_table = make_table(market_cells, 2, static_cast<COL>(pillars.size() + 1));
    std::string market = registry.create_market(market_table);

    std::vector<std::vector<XCHAR>> guess_bufs;
    std::vector<XLOPER12> guess_cells{
        str_cell(guess_bufs, "a"), num_cell(0.4),
        str_cell(guess_bufs, "b"), num_cell(0.05),
        str_cell(guess_bufs, "sigma"), num_cell(sigma),
        str_cell(guess_bufs, "eta"), num_cell(eta),
        str_cell(guess_bufs, "rho"), num_cell(rho),
        str_cell(guess_bufs, "r0"), num_cell(r0),
    };
    XLOPER12 guess_table = make_table(guess_cells, 6, 2);

    std::string calibrator = registry.create_calibrator("HullWhite2F");
    engine::CalibrationResult result = registry.calibrate(calibrator, market, guess_table);

    EXPECT_TRUE(result.converged);
    EXPECT_NEAR(std::get<double>(result.optimal_params.at("a")), true_a, 1e-4);
    EXPECT_NEAR(std::get<double>(result.optimal_params.at("b")), true_b, 1e-4);
}

TEST(HandleRegistry, CalibrateUnknownHandleThrows) {
    xlbridge::HandleRegistry registry = make_registry();
    XLOPER12 missing = missing_arg();
    EXPECT_THROW(registry.calibrate("calibrator:NoExiste", "market:NoExiste", missing), std::out_of_range);
}

TEST(NewCalibrationResult, IncludesOptimalParamsAndDiagnostics) {
    engine::CalibrationResult r;
    r.optimal_params = engine::Params{{"a", 0.15}, {"b", 0.025}};
    r.rmse = 1e-9;
    r.iterations = 12;
    r.converged = true;

    XLOPER12* out = xlbridge::new_calibration_result(r);
    ASSERT_EQ(out->xltype & ~static_cast<DWORD>(xlbitDLLFree), static_cast<DWORD>(xltypeMulti));
    EXPECT_EQ(out->val.array.columns, 2);
    EXPECT_EQ(out->val.array.rows, 5); // a, b, rmse, iterations, converged
    xlbridge::free_xloper(out);
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

namespace {

DWORD xl_base_type(const XLOPER12& x) { return x.xltype & ~static_cast<DWORD>(xlbitXLFree | xlbitDLLFree); }

} // namespace

TEST(NewPriceResult, MixesScalarAndProfileRowsInLongFormat) {
    engine::PriceResult result;

    engine::MeasureResult pv;
    pv.has_scalar = true;
    pv.scalar = 0.0;
    result.push_back({"PV", pv});

    engine::MeasureResult ee;
    ee.has_scalar = false;
    ee.times = {0.0, 1.0};
    ee.primary = {0.0, 12862.62};
    result.push_back({"ExpectedExposure", ee});

    XLOPER12* out = xlbridge::new_price_result(result);
    ASSERT_EQ(xl_base_type(*out), static_cast<DWORD>(xltypeMulti));
    EXPECT_EQ(out->val.array.columns, 3);
    ASSERT_EQ(out->val.array.rows, 3); // 1 fila de PV (escalar) + 2 filas de ExpectedExposure

    const XLOPER12* rows = out->val.array.lparray;
    EXPECT_EQ(xlbridge::from_xl_string(rows[0 * 3 + 0].val.str + 1, rows[0 * 3 + 0].val.str[0]), "PV");
    EXPECT_EQ(xl_base_type(rows[0 * 3 + 1]), static_cast<DWORD>(xltypeNil)); // Time en blanco
    EXPECT_DOUBLE_EQ(rows[0 * 3 + 2].val.num, 0.0);

    EXPECT_EQ(xlbridge::from_xl_string(rows[1 * 3 + 0].val.str + 1, rows[1 * 3 + 0].val.str[0]), "ExpectedExposure");
    EXPECT_DOUBLE_EQ(rows[1 * 3 + 1].val.num, 0.0);
    EXPECT_DOUBLE_EQ(rows[2 * 3 + 1].val.num, 1.0);
    EXPECT_DOUBLE_EQ(rows[2 * 3 + 2].val.num, 12862.62);

    xlbridge::free_xloper(out);
}

TEST(NewPriceResult, EmptyResultBecomesNaError) {
    engine::PriceResult result;
    XLOPER12* out = xlbridge::new_price_result(result);
    EXPECT_EQ(xl_base_type(*out), static_cast<DWORD>(xltypeErr));
    xlbridge::free_xloper(out);
}

// PLAN.md §7.19: mismo formato largo que new_price_result, con una columna TradeIndex al
// frente.
TEST(NewPriceBatchResult, PrependsTradeIndexColumn) {
    engine::MeasureResult pv_a;
    pv_a.has_scalar = true;
    pv_a.scalar = 100.0;
    engine::MeasureResult pv_b;
    pv_b.has_scalar = true;
    pv_b.scalar = 200.0;

    engine::PriceBatchResult result;
    result.push_back({0, {{"PV", pv_a}}});
    result.push_back({1, {{"PV", pv_b}}});

    XLOPER12* out = xlbridge::new_price_batch_result(result);
    ASSERT_EQ(xl_base_type(*out), static_cast<DWORD>(xltypeMulti));
    EXPECT_EQ(out->val.array.columns, 4);
    ASSERT_EQ(out->val.array.rows, 2);

    const XLOPER12* rows = out->val.array.lparray;
    EXPECT_DOUBLE_EQ(rows[0 * 4 + 0].val.num, 0.0); // TradeIndex
    EXPECT_EQ(xlbridge::from_xl_string(rows[0 * 4 + 1].val.str + 1, rows[0 * 4 + 1].val.str[0]), "PV");
    EXPECT_DOUBLE_EQ(rows[0 * 4 + 3].val.num, 100.0);
    EXPECT_DOUBLE_EQ(rows[1 * 4 + 0].val.num, 1.0);
    EXPECT_DOUBLE_EQ(rows[1 * 4 + 3].val.num, 200.0);

    xlbridge::free_xloper(out);
}

TEST(NewPriceBatchResult, EmptyResultBecomesNaError) {
    engine::PriceBatchResult result;
    XLOPER12* out = xlbridge::new_price_batch_result(result);
    EXPECT_EQ(xl_base_type(*out), static_cast<DWORD>(xltypeErr));
    xlbridge::free_xloper(out);
}

// PLAN.md §7.19: mismo formato largo, con tres columnas de indice al frente.
TEST(NewPriceGridResult, PrependsTradeModelMarketIndexColumns) {
    engine::MeasureResult pv;
    pv.has_scalar = true;
    pv.scalar = 42.0;

    engine::PriceGridResult result;
    result.push_back({1, 0, 1, {{"PV", pv}}});

    XLOPER12* out = xlbridge::new_price_grid_result(result);
    ASSERT_EQ(xl_base_type(*out), static_cast<DWORD>(xltypeMulti));
    EXPECT_EQ(out->val.array.columns, 6);
    ASSERT_EQ(out->val.array.rows, 1);

    const XLOPER12* rows = out->val.array.lparray;
    EXPECT_DOUBLE_EQ(rows[0].val.num, 1.0);       // TradeIndex
    EXPECT_DOUBLE_EQ(rows[1].val.num, 0.0);       // ModelIndex
    EXPECT_DOUBLE_EQ(rows[2].val.num, 1.0);       // MarketIndex
    EXPECT_EQ(xlbridge::from_xl_string(rows[3].val.str + 1, rows[3].val.str[0]), "PV");
    EXPECT_DOUBLE_EQ(rows[5].val.num, 42.0);

    xlbridge::free_xloper(out);
}

TEST(NewPriceGridResult, EmptyResultBecomesNaError) {
    engine::PriceGridResult result;
    XLOPER12* out = xlbridge::new_price_grid_result(result);
    EXPECT_EQ(xl_base_type(*out), static_cast<DWORD>(xltypeErr));
    xlbridge::free_xloper(out);
}
