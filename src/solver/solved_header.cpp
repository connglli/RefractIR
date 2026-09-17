#include "solver/solved_header.hpp"

#include <algorithm>

#include "ast/ast.hpp"

namespace refractir {

  std::string formatModelValue(const SymbolicExecutor::Result::ModelVal &v) {
    if (std::holds_alternative<std::int64_t>(v))
      return std::to_string(std::get<std::int64_t>(v));
    // formatDouble rather than a decimal of our own: it round-trips through
    // parseFloatLiteral, keeps a signed zero signed, and does not lose a
    // subnormal. See its comment in ast.hpp.
    return formatDouble(std::get<double>(v));
  }

  void writeSolvedHeader(
      std::ostream &out, const std::vector<std::pair<std::string, std::string>> &paramValues,
      const std::string &retText
  ) {
    if (paramValues.empty() && retText.empty())
      return;
    out << "// SOLVED:";
    bool first = true;
    for (const auto &[name, val]: paramValues) {
      out << (first ? " " : ", ") << name << "=" << val;
      first = false;
    }
    if (!retText.empty())
      out << (first ? " " : ", ") << "ret=" << retText;
    out << "\n";
  }

  void writeSolvedHeader(
      std::ostream &out,
      const std::unordered_map<std::string, SymbolicExecutor::Result::ModelVal> &paramModel,
      const std::string &retText
  ) {
    // Value format via formatModelValue; line shape via the overload above.
    // Sorted by name: unordered_map iteration order is not declaration order,
    // so without this the line's field order varies run to run.
    std::vector<std::pair<std::string, std::string>> pairs;
    pairs.reserve(paramModel.size());
    for (const auto &[name, val]: paramModel)
      pairs.emplace_back(name, formatModelValue(val));
    std::sort(pairs.begin(), pairs.end(), [](const auto &a, const auto &b) {
      return a.first < b.first;
    });
    writeSolvedHeader(out, pairs, retText);
  }

  std::unordered_map<std::string, std::string> parseSolvedHeader(std::string_view src) {
    std::unordered_map<std::string, std::string> kv;
    constexpr std::string_view kTag = "// SOLVED:";
    const std::size_t pos = src.find(kTag);
    if (pos == std::string_view::npos)
      return kv;
    const std::size_t eol = src.find('\n', pos);
    std::string_view line = src.substr(pos + kTag.size(), eol - pos - kTag.size());

    auto trim = [](std::string_view v) {
      const std::size_t b = v.find_first_not_of(" \t");
      const std::size_t e = v.find_last_not_of(" \t");
      return b == std::string_view::npos ? std::string_view{} : v.substr(b, e - b + 1);
    };
    while (!line.empty()) {
      const std::size_t comma = line.find(',');
      std::string_view part = line.substr(0, comma);
      line = comma == std::string_view::npos ? std::string_view{} : line.substr(comma + 1);
      const std::size_t eq = part.find('=');
      if (eq == std::string_view::npos)
        continue;
      kv.emplace(std::string(trim(part.substr(0, eq))), std::string(trim(part.substr(eq + 1))));
    }
    return kv;
  }

} // namespace refractir
