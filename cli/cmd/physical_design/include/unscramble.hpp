/**
 * @file unscramble.hpp
 * @brief CLI command for JSON-driven pin unscrambling.
 */

#ifndef FICTION_CMD_UNSCRAMBLE_HPP
#define FICTION_CMD_UNSCRAMBLE_HPP

#include <fiction/algorithms/physical_design/pin_unscrambling.hpp>
#include <fiction/io/pin_unscrambling_spec.hpp>

#include <alice/alice.hpp>
#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

namespace alice
{

/**
 * @brief Performs pin unscrambling for an existing routed FGL based on a JSON specification.
 */
class unscramble_command final : public command
{
  public:
    /**
     * @brief Standard constructor.
     *
     * @param e alice::environment that specifies stores etc.
     */
    explicit unscramble_command(const environment::ptr& e);

  protected:
    /**
     * @brief Executes the pin unscrambling workflow.
     */
    void execute() override;

    /**
     * @brief Logs command statistics.
     *
     * @return JSON object with runtime and mapping metadata.
     */
    nlohmann::json log() const override;

  private:
    /**
     * @brief Optional JSON specification file path.
     */
    std::string spec_file{};
    /**
     * @brief Last parsed specification.
     */
    fiction::pin_unscrambling_spec spec{};
    /**
     * @brief Last resolved runtime configuration.
     */
    fiction::pin_unscrambling_configuration cfg{};
    /**
     * @brief Last run report.
     */
    fiction::pin_unscrambling_report report{};
    /**
     * @brief CLI override for comma-separated input order.
     */
    std::string input_order{};
    /**
     * @brief CLI override for alias-to-semantic input mappings.
     */
    std::vector<std::string> input_mapping_args{};
    /**
     * @brief CLI override for comma-separated output order.
     */
    std::string output_order{};
    /**
     * @brief CLI override for alias-to-semantic output mappings.
     */
    std::vector<std::string> output_mapping_args{};
    /**
     * @brief CLI override for report file path.
     */
    std::string report_file{};
    /**
     * @brief Whether strict full order checking should be disabled.
     */
    bool no_strict_full_order{false};
};

}  // namespace alice

#endif  // FICTION_CMD_UNSCRAMBLE_HPP
