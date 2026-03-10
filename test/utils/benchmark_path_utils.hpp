/**
 * @file benchmark_path_utils.hpp
 * @brief Test utilities for locating benchmark files from different test working directories.
 */

#ifndef FICTION_TEST_UTILS_BENCHMARK_PATH_UTILS_HPP
#define FICTION_TEST_UTILS_BENCHMARK_PATH_UTILS_HPP

#include <filesystem>
#include <initializer_list>
#include <stdexcept>
#include <string>

namespace test::benchmark_path_utils
{

/**
 * @brief Resolves a benchmark path by trying multiple repository-relative candidates.
 *
 * @param relative_path Benchmark path relative to the repository root.
 * @return Existing path string.
 * @throws std::runtime_error If none of the candidate paths exists.
 */
[[nodiscard]] inline std::string resolve(const std::filesystem::path& relative_path)
{
    const std::initializer_list<std::filesystem::path> candidates{
        relative_path,
        std::filesystem::path{".."} / relative_path,
        std::filesystem::path{"../.."} / relative_path,
    };

    for (const auto& candidate : candidates)
    {
        if (std::filesystem::exists(candidate))
        {
            return candidate.string();
        }
    }

    throw std::runtime_error{"unable to locate benchmark file: " + relative_path.string()};
}

}  // namespace test::benchmark_path_utils

#endif  // FICTION_TEST_UTILS_BENCHMARK_PATH_UTILS_HPP
