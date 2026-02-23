//
// Created by marcel on 23.02.26.
//

#ifndef FICTION_CMD_RETURNPATH_VIZ_HPP
#define FICTION_CMD_RETURNPATH_VIZ_HPP

#include <fiction/algorithms/physical_design/route_return_path.hpp>

#include <alice/alice.hpp>

#include <string>

namespace alice
{

/**
 * Visualizes return-path routing overlays in dedicated DOT files.
 *
 * The command routes PO-to-PI return paths on the current gate-level layout and writes a DOT plot in which newly
 * added return-path tiles and edges are highlighted on top of the legal routed layout.
 */
class returnpath_viz_command final : public command
{
  public:
    /**
     * Standard constructor. Adds options and flags.
     *
     * @param e alice::environment that specifies stores etc.
     */
    explicit returnpath_viz_command(const environment::ptr& e);

  protected:
    /**
     * Executes return-path visualization.
     */
    void execute() override;

  private:
    /**
     * Output DOT filename (single mode) or base filename (both mode).
     */
    std::string filename{};
    /**
     * Routing/plot mode: `relaxed`, `strict`, or `both`.
     */
    std::string mode{"relaxed"};
    /**
     * Optional comma-separated pair index order.
     */
    std::string pin_routing_order{};
    /**
     * Optional comma-separated whitelist coordinates in `x:y` or `x:y:z` format.
     */
    std::string routing_whitelist{};
    /**
     * Return-path routing parameters.
     */
    fiction::route_return_path_params ps{};
};

}  // namespace alice

#endif  // FICTION_CMD_RETURNPATH_VIZ_HPP
