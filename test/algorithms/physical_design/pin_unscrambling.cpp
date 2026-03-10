/**
 * @file pin_unscrambling.cpp
 * @brief Unit tests for pin unscrambling workflow.
 */

#include <catch2/catch_test_macros.hpp>

#include <fiction/algorithms/physical_design/pin_unscrambling.hpp>
#include <fiction/io/pin_unscrambling_spec.hpp>
#include <fiction/layouts/clocked_layout.hpp>
#include <fiction/types.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{

/**
 * @brief Creates a unique temporary directory for a test case.
 *
 * @return Path to newly created directory.
 */
std::filesystem::path make_temp_test_dir()
{
    const auto timestamp = static_cast<uint64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count());

    const auto test_dir =
        std::filesystem::temp_directory_path() / ("fiction_pin_unscrambling_test_" + std::to_string(timestamp));

    std::filesystem::create_directories(test_dir);

    return test_dir;
}

/**
 * @brief Creates a small hex-even-row layout with generic PI/PO aliases.
 *
 * @return Layout object.
 */
fiction::hex_even_row_gate_clk_lyt make_test_layout()
{
    using lyt = fiction::hex_even_row_gate_clk_lyt;

    lyt layout{{4, 4, 1}, fiction::row_clocking<lyt>()};

    const auto pi00     = layout.create_pi("pi00", {0, 0});
    const auto pi01     = layout.create_pi("pi01", {1, 0});
    const auto and_gate = layout.create_and(pi00, pi01, {0, 1});
    layout.create_po(and_gate, "po00", {0, 2});

    return layout;
}

/**
 * @brief Creates a small hex-even-row layout with two primary outputs.
 *
 * @return Layout object.
 */
fiction::hex_even_row_gate_clk_lyt make_two_output_test_layout()
{
    using lyt = fiction::hex_even_row_gate_clk_lyt;

    lyt layout{{4, 4, 1}, fiction::row_clocking<lyt>()};

    const auto pi00     = layout.create_pi("pi00", {0, 0});
    const auto pi01     = layout.create_pi("pi01", {1, 0});
    const auto and_gate = layout.create_and(pi00, pi01, {0, 1});

    layout.create_po(and_gate, "po00", {0, 2});
    layout.create_po(pi00, "po01", {1, 2});

    return layout;
}

/**
 * @brief Creates a small hex-even-row layout whose two POs originate from the same 2-output gate.
 *
 * @return Layout object.
 */
fiction::hex_even_row_gate_clk_lyt make_multioutput_gate_test_layout()
{
    using lyt = fiction::hex_even_row_gate_clk_lyt;

    lyt layout{{4, 4, 1}, fiction::row_clocking<lyt>()};

    const auto pi00 = layout.create_pi("pi00", {0, 0});
    const auto pi01 = layout.create_pi("pi01", {1, 0});
    const auto ha   = layout.create_ha(pi00, pi01, {0, 1});

    const auto carry = ha;
    auto       sum   = ha;
    sum.output       = 1u;

    layout.create_po(carry, "po00", {0, 2});
    layout.create_po(sum, "po01", {1, 2});

    return layout;
}

/**
 * @brief Collects PI aliases sorted by physical x-coordinate.
 *
 * @tparam Lyt Gate-level layout type.
 * @param lyt Layout to inspect.
 * @return PI aliases ordered from left to right.
 */
template <typename Lyt>
std::vector<std::string> collect_pi_aliases_sorted_by_x(const Lyt& lyt)
{
    std::vector<std::pair<fiction::tile<Lyt>, std::string>> pins{};
    pins.reserve(lyt.num_pis());

    lyt.foreach_pi([&lyt, &pins](const auto& pi) { pins.emplace_back(lyt.get_tile(pi), lyt.get_name(pi)); });

    std::sort(pins.begin(), pins.end(),
              [](const auto& lhs, const auto& rhs)
              {
                  if (lhs.first.x != rhs.first.x)
                  {
                      return lhs.first.x < rhs.first.x;
                  }

                  if (lhs.first.y != rhs.first.y)
                  {
                      return lhs.first.y < rhs.first.y;
                  }

                  return lhs.first.z < rhs.first.z;
              });

    std::vector<std::string> ordered_aliases{};
    ordered_aliases.reserve(pins.size());

    for (const auto& [coord, alias] : pins)
    {
        static_cast<void>(coord);
        ordered_aliases.push_back(alias);
    }

    return ordered_aliases;
}

/**
 * @brief Collects PO aliases sorted by physical x-coordinate.
 *
 * @tparam Lyt Gate-level layout type.
 * @param lyt Layout to inspect.
 * @return PO aliases ordered from left to right.
 */
template <typename Lyt>
std::vector<std::string> collect_po_aliases_sorted_by_x(const Lyt& lyt)
{
    std::vector<std::pair<fiction::tile<Lyt>, std::string>> outputs{};
    outputs.reserve(lyt.num_pos());

    uint32_t po_index = 0u;
    lyt.foreach_po([&lyt, &outputs, &po_index](const auto& po)
                   { outputs.emplace_back(static_cast<fiction::tile<Lyt>>(po), lyt.get_output_name(po_index++)); });

    std::sort(outputs.begin(), outputs.end(),
              [](const auto& lhs, const auto& rhs)
              {
                  if (lhs.first.x != rhs.first.x)
                  {
                      return lhs.first.x < rhs.first.x;
                  }

                  if (lhs.first.y != rhs.first.y)
                  {
                      return lhs.first.y < rhs.first.y;
                  }

                  return lhs.first.z < rhs.first.z;
              });

    std::vector<std::string> ordered_aliases{};
    ordered_aliases.reserve(outputs.size());

    for (const auto& [coord, alias] : outputs)
    {
        static_cast<void>(coord);
        ordered_aliases.push_back(alias);
    }

    return ordered_aliases;
}

}  // namespace

TEST_CASE("Pin unscrambling spec parsing", "[pin-unscrambling]")
{
    std::stringstream spec_stream{};
    spec_stream << R"({
  "input_order": ["a", "b"],
  "input_mappings": [
    {"fgl_alias": "pi00", "semantic_name": "a"}
  ],
  "output_order": ["f"],
  "output_mappings": [
    {"fgl_alias": "po00", "semantic_name": "f"}
  ],
  "strict_full_order": false,
  "report_file": "/tmp/report.json"
})";

    const auto spec = fiction::read_pin_unscrambling_spec(spec_stream);

    CHECK(spec.input_order == std::vector<std::string>{"a", "b"});
    REQUIRE(spec.input_mappings.size() == 1u);
    CHECK(spec.input_mappings[0].fgl_alias == "pi00");
    CHECK(spec.input_mappings[0].semantic_name == "a");
    CHECK(spec.output_order == std::vector<std::string>{"f"});
    REQUIRE(spec.output_mappings.size() == 1u);
    CHECK(spec.output_mappings[0].fgl_alias == "po00");
    CHECK(spec.output_mappings[0].semantic_name == "f");
    REQUIRE(spec.strict_full_order.has_value());
    CHECK_FALSE(*spec.strict_full_order);
    REQUIRE(spec.report_file.has_value());
    CHECK(*spec.report_file == "/tmp/report.json");
}

TEST_CASE("Pin unscrambling uses FGL aliases directly when no mappings are provided", "[pin-unscrambling]")
{
    fiction::pin_unscrambling_configuration cfg{};
    cfg.input_order       = {"pi01", "pi00"};
    cfg.output_order      = {"po00"};
    cfg.strict_full_order = true;

    const auto result = fiction::run_pin_unscrambling(make_test_layout(), cfg);

    REQUIRE(result.report.input_mappings.size() == 2u);
    CHECK(result.report.input_mappings[0].semantic_name == "pi01");
    CHECK(result.report.input_mappings[0].fgl_alias == "pi01");

    CHECK(result.report.input_mappings[1].semantic_name == "pi00");
    CHECK(result.report.input_mappings[1].fgl_alias == "pi00");

    CHECK(result.report.output_mappings.size() == 1u);
    CHECK(result.report.output_mappings[0].semantic_name == "po00");
    CHECK(result.report.output_mappings[0].fgl_alias == "po00");

    std::vector<std::string> unscrambled_pi_aliases{};
    unscrambled_pi_aliases.reserve(result.layout.num_pis());

    result.layout.foreach_pi([&result, &unscrambled_pi_aliases](const auto& pi)
                             { unscrambled_pi_aliases.push_back(result.layout.get_name(pi)); });

    REQUIRE(unscrambled_pi_aliases.size() == 2u);
    CHECK(unscrambled_pi_aliases[0] == "pi01");
    CHECK(unscrambled_pi_aliases[1] == "pi00");
    CHECK(collect_pi_aliases_sorted_by_x(result.layout) == std::vector<std::string>{"pi01", "pi00"});
}

TEST_CASE("Pin unscrambling supports alias mappings as rename-only metadata", "[pin-unscrambling]")
{
    const auto test_dir    = make_temp_test_dir();
    const auto report_file = test_dir / "mapping_only_report.json";

    fiction::pin_unscrambling_configuration cfg{};
    cfg.input_mappings    = {{"pi01", "activation[1]"}, {"pi00", "activation[0]"}};
    cfg.input_order       = {"activation[1]", "activation[0]"};
    cfg.output_mappings   = {{"po00", "result[0]"}};
    cfg.output_order      = {"result[0]"};
    cfg.strict_full_order = true;
    cfg.report_file       = report_file.string();

    const auto result = fiction::run_pin_unscrambling(make_test_layout(), cfg);

    REQUIRE(result.report.input_mappings.size() == 2u);
    CHECK(result.report.input_mappings[0].semantic_name == "activation[1]");
    CHECK(result.report.input_mappings[0].fgl_alias == "pi01");

    CHECK(result.report.input_mappings[1].semantic_name == "activation[0]");
    CHECK(result.report.input_mappings[1].fgl_alias == "pi00");

    std::vector<std::string> unscrambled_pi_aliases{};
    unscrambled_pi_aliases.reserve(result.layout.num_pis());

    result.layout.foreach_pi([&result, &unscrambled_pi_aliases](const auto& pi)
                             { unscrambled_pi_aliases.push_back(result.layout.get_name(pi)); });

    REQUIRE(unscrambled_pi_aliases.size() == 2u);
    CHECK(unscrambled_pi_aliases[0] == "pi01");
    CHECK(unscrambled_pi_aliases[1] == "pi00");
    CHECK(collect_pi_aliases_sorted_by_x(result.layout) == std::vector<std::string>{"pi01", "pi00"});

    std::ifstream report_stream{report_file};
    REQUIRE(report_stream.is_open());

    nlohmann::json report_json{};
    report_stream >> report_json;

    REQUIRE(report_json.contains("input_mappings"));
    REQUIRE(report_json["input_mappings"].is_array());
    REQUIRE_FALSE(report_json["input_mappings"].empty());
    CHECK(report_json["input_mappings"][0]["semantic_name"] == "activation[1]");
    CHECK_FALSE(report_json["input_mappings"][0].contains("canonical_index"));

    REQUIRE(report_json.contains("output_mappings"));
    REQUIRE(report_json["output_mappings"].is_array());
    REQUIRE_FALSE(report_json["output_mappings"].empty());
    CHECK(report_json["output_mappings"][0]["semantic_name"] == "result[0]");
    CHECK_FALSE(report_json["output_mappings"][0].contains("canonical_index"));

    std::filesystem::remove_all(test_dir);
}

TEST_CASE("Pin unscrambling supports alias mappings combined with semantic order without AIG", "[pin-unscrambling]")
{
    fiction::pin_unscrambling_configuration cfg{};
    cfg.input_mappings    = {{"pi00", "a"}, {"pi01", "b"}};
    cfg.input_order       = {"b", "a"};
    cfg.output_mappings   = {{"po00", "f"}, {"po01", "g"}};
    cfg.output_order      = {"g", "f"};
    cfg.strict_full_order = true;

    const auto result = fiction::run_pin_unscrambling(make_two_output_test_layout(), cfg);

    REQUIRE(result.report.input_mappings.size() == 2u);
    CHECK(result.report.input_mappings[0].semantic_name == "b");
    CHECK(result.report.input_mappings[0].fgl_alias == "pi01");
    CHECK(result.report.input_mappings[1].semantic_name == "a");
    CHECK(result.report.input_mappings[1].fgl_alias == "pi00");

    REQUIRE(result.report.output_mappings.size() == 2u);
    CHECK(result.report.output_mappings[0].semantic_name == "g");
    CHECK(result.report.output_mappings[0].fgl_alias == "po01");
    CHECK(result.report.output_mappings[1].semantic_name == "f");
    CHECK(result.report.output_mappings[1].fgl_alias == "po00");

    CHECK(collect_pi_aliases_sorted_by_x(result.layout) == std::vector<std::string>{"pi01", "pi00"});
    CHECK(collect_po_aliases_sorted_by_x(result.layout) == std::vector<std::string>{"po01", "po00"});
}

TEST_CASE("Pin unscrambling rejects unknown semantic names in resolved order mode", "[pin-unscrambling]")
{
    fiction::pin_unscrambling_configuration cfg{};
    cfg.input_mappings    = {{"pi00", "a"}, {"pi01", "b"}};
    cfg.input_order       = {"missing", "a"};
    cfg.output_mappings   = {{"po00", "f"}};
    cfg.output_order      = {"f"};
    cfg.strict_full_order = true;

    CHECK_THROWS_AS(fiction::run_pin_unscrambling(make_test_layout(), cfg), std::invalid_argument);
}

TEST_CASE("Pin unscrambling appends unspecified semantic order entries in non-strict mode", "[pin-unscrambling]")
{
    fiction::pin_unscrambling_configuration cfg{};
    cfg.input_mappings    = {{"pi00", "a"}, {"pi01", "b"}};
    cfg.input_order       = {"b"};
    cfg.output_mappings   = {{"po00", "f"}};
    cfg.output_order      = {"f"};
    cfg.strict_full_order = false;

    const auto result = fiction::run_pin_unscrambling(make_test_layout(), cfg);

    REQUIRE(result.report.input_mappings.size() == 2u);
    CHECK(result.report.input_mappings[0].semantic_name == "b");
    CHECK(result.report.input_mappings[0].fgl_alias == "pi01");
    CHECK(result.report.input_mappings[1].semantic_name == "a");
    CHECK(result.report.input_mappings[1].fgl_alias == "pi00");

    CHECK(collect_pi_aliases_sorted_by_x(result.layout) == std::vector<std::string>{"pi01", "pi00"});
}

TEST_CASE("Pin unscrambling appends unspecified aliases in non-strict mode without mappings", "[pin-unscrambling]")
{
    fiction::pin_unscrambling_configuration cfg{};
    cfg.input_order       = {"pi01"};
    cfg.output_order      = {"po00"};
    cfg.strict_full_order = false;

    const auto result = fiction::run_pin_unscrambling(make_test_layout(), cfg);

    REQUIRE(result.report.input_mappings.size() == 2u);
    CHECK(result.report.input_mappings[0].semantic_name == "pi01");
    CHECK(result.report.input_mappings[0].fgl_alias == "pi01");
    CHECK(result.report.input_mappings[1].semantic_name == "pi00");
    CHECK(result.report.input_mappings[1].fgl_alias == "pi00");
}

TEST_CASE("Pin unscrambling rejects invalid alias mapping definitions", "[pin-unscrambling]")
{
    SECTION("unknown alias")
    {
        fiction::pin_unscrambling_configuration cfg{};
        cfg.input_mappings    = {{"pi99", "activation[1]"}, {"pi00", "activation[0]"}};
        cfg.input_order       = {"activation[1]", "activation[0]"};
        cfg.output_mappings   = {{"po00", "result[0]"}};
        cfg.output_order      = {"result[0]"};
        cfg.strict_full_order = true;

        CHECK_THROWS_AS(fiction::run_pin_unscrambling(make_test_layout(), cfg), std::invalid_argument);
    }

    SECTION("duplicate alias")
    {
        fiction::pin_unscrambling_configuration cfg{};
        cfg.input_mappings    = {{"pi01", "activation[1]"}, {"pi01", "activation[0]"}};
        cfg.input_order       = {"activation[1]", "activation[0]"};
        cfg.output_mappings   = {{"po00", "result[0]"}};
        cfg.output_order      = {"result[0]"};
        cfg.strict_full_order = true;

        CHECK_THROWS_AS(fiction::run_pin_unscrambling(make_test_layout(), cfg), std::invalid_argument);
    }

    SECTION("duplicate semantic name")
    {
        fiction::pin_unscrambling_configuration cfg{};
        cfg.input_mappings    = {{"pi01", "activation[0]"}, {"pi00", "activation[0]"}};
        cfg.input_order       = {"activation[0]", "activation[1]"};
        cfg.output_mappings   = {{"po00", "result[0]"}};
        cfg.output_order      = {"result[0]"};
        cfg.strict_full_order = true;

        CHECK_THROWS_AS(fiction::run_pin_unscrambling(make_test_layout(), cfg), std::invalid_argument);
    }

    SECTION("strict order size mismatch")
    {
        fiction::pin_unscrambling_configuration cfg{};
        cfg.input_mappings    = {{"pi01", "activation[1]"}};
        cfg.input_order       = {"activation[1]"};
        cfg.output_mappings   = {{"po00", "result[0]"}};
        cfg.output_order      = {"result[0]"};
        cfg.strict_full_order = true;

        CHECK_THROWS_AS(fiction::run_pin_unscrambling(make_test_layout(), cfg), std::invalid_argument);
    }
}

TEST_CASE("Pin unscrambling rejects missing required order", "[pin-unscrambling]")
{
    SECTION("missing input order")
    {
        fiction::pin_unscrambling_configuration cfg{};
        cfg.output_order      = {"po00"};
        cfg.strict_full_order = true;

        CHECK_THROWS_AS(fiction::run_pin_unscrambling(make_test_layout(), cfg), std::invalid_argument);
    }

    SECTION("missing output order")
    {
        fiction::pin_unscrambling_configuration cfg{};
        cfg.input_order       = {"pi00", "pi01"};
        cfg.strict_full_order = true;

        CHECK_THROWS_AS(fiction::run_pin_unscrambling(make_test_layout(), cfg), std::invalid_argument);
    }

    SECTION("mappings without order")
    {
        fiction::pin_unscrambling_configuration cfg{};
        cfg.input_mappings    = {{"pi01", "activation[1]"}, {"pi00", "activation[0]"}};
        cfg.output_mappings   = {{"po00", "result[0]"}};
        cfg.strict_full_order = true;

        CHECK_THROWS_AS(fiction::run_pin_unscrambling(make_test_layout(), cfg), std::invalid_argument);
    }
}

TEST_CASE("Pin unscrambling reorders multiple outputs in semantic order mode", "[pin-unscrambling]")
{
    fiction::pin_unscrambling_configuration cfg{};
    cfg.input_mappings    = {{"pi00", "a"}, {"pi01", "b"}};
    cfg.input_order       = {"a", "b"};
    cfg.output_mappings   = {{"po00", "f"}, {"po01", "g"}};
    cfg.output_order      = {"g", "f"};
    cfg.strict_full_order = true;

    const auto result = fiction::run_pin_unscrambling(make_two_output_test_layout(), cfg);

    REQUIRE(result.report.output_mappings.size() == 2u);
    CHECK(result.report.output_mappings[0].semantic_name == "g");
    CHECK(result.report.output_mappings[0].fgl_alias == "po01");
    CHECK(result.report.output_mappings[1].semantic_name == "f");
    CHECK(result.report.output_mappings[1].fgl_alias == "po00");

    std::vector<std::string> unscrambled_po_aliases{};
    unscrambled_po_aliases.reserve(result.layout.num_pos());

    uint32_t po_index = 0u;
    result.layout.foreach_po([&result, &unscrambled_po_aliases, &po_index](const auto&)
                             { unscrambled_po_aliases.push_back(result.layout.get_output_name(po_index++)); });

    REQUIRE(unscrambled_po_aliases.size() == 2u);
    CHECK(unscrambled_po_aliases[0] == "po01");
    CHECK(unscrambled_po_aliases[1] == "po00");
    CHECK(collect_po_aliases_sorted_by_x(result.layout) == std::vector<std::string>{"po01", "po00"});
}

TEST_CASE("Pin unscrambling reorders multi-output gate pins in semantic order mode", "[pin-unscrambling]")
{
    fiction::pin_unscrambling_configuration cfg{};
    cfg.input_mappings    = {{"pi00", "a"}, {"pi01", "b"}};
    cfg.input_order       = {"a", "b"};
    cfg.output_mappings   = {{"po00", "carry"}, {"po01", "sum"}};
    cfg.output_order      = {"sum", "carry"};
    cfg.strict_full_order = true;

    const auto layout = make_multioutput_gate_test_layout();
    const auto result = fiction::run_pin_unscrambling(layout, cfg);

    REQUIRE(result.report.output_mappings.size() == 2u);
    CHECK(result.report.output_mappings[0].semantic_name == "sum");
    CHECK(result.report.output_mappings[0].fgl_alias == "po01");
    CHECK(result.report.output_mappings[1].semantic_name == "carry");
    CHECK(result.report.output_mappings[1].fgl_alias == "po00");

    std::vector<std::string> unscrambled_po_aliases{};
    unscrambled_po_aliases.reserve(result.layout.num_pos());

    uint32_t po_index = 0u;
    result.layout.foreach_po([&result, &unscrambled_po_aliases, &po_index](const auto&)
                             { unscrambled_po_aliases.push_back(result.layout.get_output_name(po_index++)); });

    REQUIRE(unscrambled_po_aliases.size() == 2u);
    CHECK(unscrambled_po_aliases[0] == "po01");
    CHECK(unscrambled_po_aliases[1] == "po00");
    CHECK(collect_po_aliases_sorted_by_x(result.layout) == std::vector<std::string>{"po01", "po00"});
}

TEST_CASE("Pin unscrambling spec parsing rejects malformed JSON fields", "[pin-unscrambling]")
{
    SECTION("input_order must be an array")
    {
        std::stringstream spec_stream{};
        spec_stream << R"({"input_order":"a"})";

        CHECK_THROWS_AS(fiction::read_pin_unscrambling_spec(spec_stream), std::invalid_argument);
    }

    SECTION("mapping entry must provide fgl_alias")
    {
        std::stringstream spec_stream{};
        spec_stream << R"({"input_mappings":[{"semantic_name":"a"}]})";

        CHECK_THROWS_AS(fiction::read_pin_unscrambling_spec(spec_stream), std::invalid_argument);
    }

    SECTION("aig_file is no longer supported")
    {
        std::stringstream spec_stream{};
        spec_stream << R"({"aig_file":"/tmp/source.aig","input_order":["pi00"],"output_order":["po00"]})";

        CHECK_THROWS_AS(fiction::read_pin_unscrambling_spec(spec_stream), std::invalid_argument);
    }

    SECTION("strict_full_order must be a boolean")
    {
        std::stringstream spec_stream{};
        spec_stream << R"({"strict_full_order":"true"})";

        CHECK_THROWS_AS(fiction::read_pin_unscrambling_spec(spec_stream), std::invalid_argument);
    }
}
