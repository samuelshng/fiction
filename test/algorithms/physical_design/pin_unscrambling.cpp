/**
 * @file pin_unscrambling.cpp
 * @brief Unit tests for pin unscrambling workflow.
 */

#include <catch2/catch_test_macros.hpp>

#include <fiction/algorithms/physical_design/pin_unscrambling.hpp>
#include <fiction/io/pin_unscrambling_spec.hpp>
#include <fiction/layouts/clocked_layout.hpp>
#include <fiction/types.hpp>

#include <mockturtle/io/write_aiger.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
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
 * @brief Writes a simple named two-input AIG to disk.
 *
 * @param filename Output filename.
 */
void write_named_test_aig(const std::filesystem::path& filename)
{
    fiction::aig_nt aig{};

    const auto a = aig.create_pi();
    const auto b = aig.create_pi();

    aig.set_name(a, "a");
    aig.set_name(b, "b");

    const auto f = aig.create_and(a, b);
    aig.create_po(f);
    aig.set_output_name(0u, "f");

    mockturtle::write_aiger(aig, filename.string());
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

}  // namespace

TEST_CASE("Pin unscrambling spec parsing", "[pin-unscrambling]")
{
    std::stringstream spec_stream{};
    spec_stream << R"({
  "aig_file": "/tmp/source.aig",
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

    REQUIRE(spec.aig_file.has_value());
    CHECK(*spec.aig_file == "/tmp/source.aig");
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

TEST_CASE("Pin unscrambling resolves semantic AIG names to generic layout aliases", "[pin-unscrambling]")
{
    const auto test_dir = make_temp_test_dir();

    const auto aig_file    = test_dir / "named.aig";
    const auto report_file = test_dir / "report.json";

    write_named_test_aig(aig_file);

    fiction::pin_unscrambling_configuration cfg{};
    cfg.aig_file          = aig_file.string();
    cfg.input_order       = {"b", "a"};
    cfg.output_order      = {"f"};
    cfg.strict_full_order = true;
    cfg.report_file       = report_file.string();

    const auto result = fiction::run_pin_unscrambling(make_test_layout(), cfg);

    REQUIRE(result.report.input_mappings.size() == 2u);
    CHECK(result.report.input_mappings[0].semantic_name == "b");
    CHECK(result.report.input_mappings[0].canonical_index == 1u);
    CHECK(result.report.input_mappings[0].fgl_alias == "pi01");

    CHECK(result.report.input_mappings[1].semantic_name == "a");
    CHECK(result.report.input_mappings[1].canonical_index == 0u);
    CHECK(result.report.input_mappings[1].fgl_alias == "pi00");

    CHECK(result.report.output_mappings.size() == 1u);
    CHECK(result.report.output_mappings[0].semantic_name == "f");
    CHECK(result.report.output_mappings[0].canonical_index == 0u);
    CHECK(result.report.output_mappings[0].fgl_alias == "po00");

    std::vector<std::string> unscrambled_pi_aliases{};
    unscrambled_pi_aliases.reserve(result.layout.num_pis());

    result.layout.foreach_pi([&result, &unscrambled_pi_aliases](const auto& pi)
                             { unscrambled_pi_aliases.push_back(result.layout.get_name(pi)); });

    REQUIRE(unscrambled_pi_aliases.size() == 2u);
    CHECK(unscrambled_pi_aliases[0] == "pi01");
    CHECK(unscrambled_pi_aliases[1] == "pi00");

    CHECK(std::filesystem::exists(report_file));
    std::filesystem::remove_all(test_dir);
}

TEST_CASE("Pin unscrambling supports alias mappings without AIG", "[pin-unscrambling]")
{
    const auto test_dir    = make_temp_test_dir();
    const auto report_file = test_dir / "mapping_only_report.json";

    fiction::pin_unscrambling_configuration cfg{};
    cfg.input_mappings    = {{"pi01", "activation[1]"}, {"pi00", "activation[0]"}};
    cfg.output_mappings   = {{"po00", "result[0]"}};
    cfg.strict_full_order = true;
    cfg.report_file       = report_file.string();

    const auto result = fiction::run_pin_unscrambling(make_test_layout(), cfg);

    REQUIRE(result.report.input_mappings.size() == 2u);
    CHECK(result.report.input_mappings[0].semantic_name == "activation[1]");
    CHECK(result.report.input_mappings[0].canonical_index == std::numeric_limits<uint32_t>::max());
    CHECK(result.report.input_mappings[0].fgl_alias == "pi01");

    CHECK(result.report.input_mappings[1].semantic_name == "activation[0]");
    CHECK(result.report.input_mappings[1].canonical_index == std::numeric_limits<uint32_t>::max());
    CHECK(result.report.input_mappings[1].fgl_alias == "pi00");

    std::vector<std::string> unscrambled_pi_aliases{};
    unscrambled_pi_aliases.reserve(result.layout.num_pis());

    result.layout.foreach_pi([&result, &unscrambled_pi_aliases](const auto& pi)
                             { unscrambled_pi_aliases.push_back(result.layout.get_name(pi)); });

    REQUIRE(unscrambled_pi_aliases.size() == 2u);
    CHECK(unscrambled_pi_aliases[0] == "pi01");
    CHECK(unscrambled_pi_aliases[1] == "pi00");

    std::ifstream report_stream{report_file};
    REQUIRE(report_stream.is_open());

    nlohmann::json report_json{};
    report_stream >> report_json;

    REQUIRE(report_json.contains("input_mappings"));
    REQUIRE(report_json["input_mappings"].is_array());
    REQUIRE_FALSE(report_json["input_mappings"].empty());
    CHECK(report_json["input_mappings"][0]["canonical_index"].is_null());

    REQUIRE(report_json.contains("output_mappings"));
    REQUIRE(report_json["output_mappings"].is_array());
    REQUIRE_FALSE(report_json["output_mappings"].empty());
    CHECK(report_json["output_mappings"][0]["canonical_index"].is_null());

    std::filesystem::remove_all(test_dir);
}

TEST_CASE("Pin unscrambling rejects unknown semantic names in AIG order mode", "[pin-unscrambling]")
{
    const auto test_dir = make_temp_test_dir();

    const auto aig_file = test_dir / "named.aig";

    write_named_test_aig(aig_file);

    fiction::pin_unscrambling_configuration cfg{};
    cfg.aig_file          = aig_file.string();
    cfg.input_order       = {"missing", "a"};
    cfg.output_order      = {"f"};
    cfg.strict_full_order = true;

    CHECK_THROWS_AS(fiction::run_pin_unscrambling(make_test_layout(), cfg), std::invalid_argument);

    std::filesystem::remove_all(test_dir);
}
