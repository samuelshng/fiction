/**
 * @file pin_unscrambling_spec.hpp
 * @brief JSON specification parsing for pin unscrambling.
 */

#ifndef FICTION_PIN_UNSCRAMBLING_SPEC_HPP
#define FICTION_PIN_UNSCRAMBLING_SPEC_HPP

#include <nlohmann/json.hpp>

#include <fstream>
#include <istream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace fiction
{

/**
 * @brief Alias-to-semantic pin mapping entry.
 */
struct pin_alias_semantic_mapping
{
    /**
     * @brief Alias of a PI/PO in the current layout (e.g., `pi14`).
     */
    std::string fgl_alias{};
    /**
     * @brief Desired semantic pin name (e.g., `activation[1]`).
     */
    std::string semantic_name{};
};

/**
 * @brief Optional JSON configuration for pin unscrambling.
 */
struct pin_unscrambling_spec
{
    /**
     * @brief Optional AIG file path used as canonical semantic I/O naming source.
     */
    std::optional<std::string> aig_file{};
    /**
     * @brief Desired semantic PI order.
     */
    std::vector<std::string> input_order{};
    /**
     * @brief Optional alias-to-semantic PI mapping list.
     */
    std::vector<pin_alias_semantic_mapping> input_mappings{};
    /**
     * @brief Desired semantic PO order.
     */
    std::vector<std::string> output_order{};
    /**
     * @brief Optional alias-to-semantic PO mapping list.
     */
    std::vector<pin_alias_semantic_mapping> output_mappings{};
    /**
     * @brief Optional strictness override.
     */
    std::optional<bool> strict_full_order{};
    /**
     * @brief Optional JSON report output path.
     */
    std::optional<std::string> report_file{};
};

namespace detail
{

/**
 * @brief Reads an optional string field from JSON.
 *
 * @param json_payload Parsed JSON.
 * @param field_name Field name.
 * @return Optional string value.
 */
inline std::optional<std::string> get_optional_string_field(const nlohmann::json&   json_payload,
                                                            const std::string_view& field_name)
{
    if (!json_payload.contains(field_name))
    {
        return std::nullopt;
    }

    if (!json_payload[field_name].is_string())
    {
        throw std::invalid_argument("Optional field '" + std::string{field_name} + "' must be a string if present.");
    }

    const auto value = json_payload[field_name].get<std::string>();
    if (value.empty())
    {
        return std::nullopt;
    }

    return value;
}

/**
 * @brief Reads an optional bool field from JSON.
 *
 * @param json_payload Parsed JSON.
 * @param field_name Field name.
 * @return Optional bool value.
 */
inline std::optional<bool> get_optional_bool_field(const nlohmann::json&   json_payload,
                                                   const std::string_view& field_name)
{
    if (!json_payload.contains(field_name))
    {
        return std::nullopt;
    }

    if (!json_payload[field_name].is_boolean())
    {
        throw std::invalid_argument("Field '" + std::string{field_name} + "' must be a boolean.");
    }

    return json_payload[field_name].get<bool>();
}

/**
 * @brief Reads an optional string array field from JSON.
 *
 * @param json_payload Parsed JSON.
 * @param field_name Field name.
 * @return Parsed string vector (empty if field is absent).
 */
inline std::vector<std::string> get_optional_string_array_field(const nlohmann::json&   json_payload,
                                                                const std::string_view& field_name)
{
    if (!json_payload.contains(field_name))
    {
        return {};
    }

    if (!json_payload[field_name].is_array())
    {
        throw std::invalid_argument("Field '" + std::string{field_name} + "' must be an array of strings.");
    }

    std::vector<std::string> values{};
    values.reserve(json_payload[field_name].size());

    for (const auto& element : json_payload[field_name])
    {
        if (!element.is_string())
        {
            throw std::invalid_argument("Field '" + std::string{field_name} + "' must contain strings only.");
        }

        values.push_back(element.get<std::string>());
    }

    return values;
}

/**
 * @brief Reads an optional alias-to-semantic mapping array field from JSON.
 *
 * @param json_payload Parsed JSON.
 * @param field_name Field name.
 * @return Parsed mapping vector (empty if field is absent).
 */
inline std::vector<pin_alias_semantic_mapping> get_optional_mapping_array_field(const nlohmann::json&   json_payload,
                                                                                const std::string_view& field_name)
{
    if (!json_payload.contains(field_name))
    {
        return {};
    }

    if (!json_payload[field_name].is_array())
    {
        throw std::invalid_argument("Field '" + std::string{field_name} + "' must be an array of objects.");
    }

    std::vector<pin_alias_semantic_mapping> values{};
    values.reserve(json_payload[field_name].size());

    for (const auto& element : json_payload[field_name])
    {
        if (!element.is_object())
        {
            throw std::invalid_argument("Field '" + std::string{field_name} + "' must contain objects only.");
        }

        if (!element.contains("fgl_alias") || !element["fgl_alias"].is_string())
        {
            throw std::invalid_argument("Every '" + std::string{field_name} +
                                        "' entry must contain string field "
                                        "'fgl_alias'.");
        }

        if (!element.contains("semantic_name") || !element["semantic_name"].is_string())
        {
            throw std::invalid_argument("Every '" + std::string{field_name} +
                                        "' entry must contain string field "
                                        "'semantic_name'.");
        }

        values.push_back({element["fgl_alias"].get<std::string>(), element["semantic_name"].get<std::string>()});
    }

    return values;
}

}  // namespace detail

/**
 * @brief Parses pin unscrambling specification JSON from a stream.
 *
 * @param is Input JSON stream.
 * @return Parsed specification.
 */
inline pin_unscrambling_spec read_pin_unscrambling_spec(std::istream& is)
{
    nlohmann::json json_payload{};
    is >> json_payload;

    pin_unscrambling_spec spec{};

    spec.aig_file          = detail::get_optional_string_field(json_payload, "aig_file");
    spec.input_order       = detail::get_optional_string_array_field(json_payload, "input_order");
    spec.input_mappings    = detail::get_optional_mapping_array_field(json_payload, "input_mappings");
    spec.output_order      = detail::get_optional_string_array_field(json_payload, "output_order");
    spec.output_mappings   = detail::get_optional_mapping_array_field(json_payload, "output_mappings");
    spec.report_file       = detail::get_optional_string_field(json_payload, "report_file");
    spec.strict_full_order = detail::get_optional_bool_field(json_payload, "strict_full_order");

    return spec;
}

/**
 * @brief Parses pin unscrambling specification JSON from a file.
 *
 * @param filename JSON file path.
 * @return Parsed specification.
 */
inline pin_unscrambling_spec read_pin_unscrambling_spec(const std::string_view& filename)
{
    std::ifstream is{filename.data(), std::ifstream::in};

    if (!is.is_open())
    {
        throw std::invalid_argument("Unable to open spec file: '" + std::string{filename} + "'.");
    }

    const auto spec = read_pin_unscrambling_spec(is);
    is.close();

    return spec;
}

}  // namespace fiction

#endif  // FICTION_PIN_UNSCRAMBLING_SPEC_HPP
