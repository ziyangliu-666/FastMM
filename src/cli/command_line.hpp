#pragma once
// What the FastMM command lines share on top of CLI11 (fastmm_cli11 target, not installed): the
// help layout, --version, the exit-code convention, and the value checks more than one of them
// needs.
//
//   CLI::App app("Backtests a registered strategy.", program);
//   fastmm::cli::setup(app);
//   app.add_option("--config", path, "...")->option_text("<file>");
//   if (const auto rc = fastmm::cli::parse(app, argc, argv)) return *rc;
//
// parse() returns 0 after --help or --version (printed to stdout) and 2 (kUsageError) after a bad
// command line, printed to stderr as "<program>: <message>".
#include "fastmm/version.hpp"

#include <CLI/CLI.hpp>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace fastmm::cli {

inline constexpr int kUsageError = 2;

// CLI11's help layout with "usage: " in front of the usage line and one blank line around it.
class HelpFormatter final : public CLI::Formatter {
 public:
  std::string make_usage(const CLI::App* app, std::string name) const override {
    const std::string u = CLI::Formatter::make_usage(app, std::move(name));
    const std::size_t begin = u.find_first_not_of('\n');
    const std::size_t end = u.find_last_not_of('\n');
    return begin == std::string::npos ? u : "usage: " + u.substr(begin, end - begin + 1) + "\n";
  }
};

// The help layout, -h/--help, and --version printing "<name> <build_info()>" (with_version = false
// leaves --version out). Options print their option_text(), e.g. "<file>", as their value.
inline void setup(CLI::App& app, bool with_version = true) {
  auto fmt = std::make_shared<HelpFormatter>();
  fmt->column_width(30);
  fmt->right_column_width(62);
  fmt->long_option_alignment_ratio(2.0F / 30.0F);  // "  -h, --help", "  --config <file>"
  fmt->enable_footer_formatting(false);            // footers are laid out by hand
  app.formatter(fmt);
  app.set_help_flag("-h,--help", "print this help and exit");
  if (with_version) {
    app.set_version_flag(
        "--version", app.get_name() + " " + build_info(), "print the version and exit");
  }
}

// Prints a usage error the way parse() does and returns kUsageError; for the checks that run after
// parsing (a flag required unless another is given, a missing command, ...).
inline int usage_error(const CLI::App& app, const std::string& message) {
  std::fprintf(stderr,
               "%s: %s\nRun with --help for more information.\n",
               app.get_name().c_str(),
               message.c_str());
  return kUsageError;
}

// A command line's main: runs `body` and reports an exception that escapes it (CLI11 throws while
// the options are being declared, allocation can fail) as "<program>: <what>" with exit code 1.
template <class F>
int guarded_main(const char* program, F&& body) noexcept {
  try {
    return body();
  } catch (const std::exception& e) {
    std::fprintf(stderr, "%s: %s\n", program, e.what());
  } catch (...) {
    std::fprintf(stderr, "%s: unknown exception\n", program);
  }
  return 1;
}

// Parses argv. nullopt: carry on; otherwise the process exit code (see the header comment).
[[nodiscard]] inline std::optional<int> parse(CLI::App& app, int argc, const char* const* argv) {
  try {
    app.parse(argc, argv);
  } catch (const CLI::CallForHelp&) {
    std::fputs(app.help().c_str(), stdout);
    return 0;
  } catch (const CLI::CallForAllHelp&) {
    std::fputs(app.help("", CLI::AppFormatMode::All).c_str(), stdout);
    return 0;
  } catch (const CLI::CallForVersion& e) {
    std::printf("%s\n", e.what());
    return 0;
  } catch (const CLI::ParseError& e) {
    std::string message = e.what();
    const std::string own = app.get_name() + ": ";  // some CLI11 messages name the program
    if (message.starts_with(own)) message.erase(0, own.size());
    return usage_error(app, message);
  }
  return std::nullopt;
}

// "key=value" with a non-empty key (--param).
[[nodiscard]] inline CLI::Validator key_value() {
  return CLI::Validator(
      [](const std::string& v) {
        const std::size_t eq = v.find('=');
        return eq == std::string::npos || eq == 0 ? "expects key=value, got '" + v + "'"
                                                  : std::string();
      },
      "");
}

// A duration in nanoseconds: "1500ms", "60s", "5m", "2h", or a bare number of seconds. Positive,
// or also 0 with zero_ok.
inline CLI::Option* add_duration(CLI::App& app,
                                 const std::string& name,
                                 std::int64_t& ns,
                                 const std::string& description,
                                 bool zero_ok = false) {
  const CLI::Validator to_ns = CLI::AsNumberWithUnit(
      std::map<std::string, std::int64_t>{
          {"ms", 1'000'000}, {"s", 1'000'000'000}, {"m", 60'000'000'000}, {"h", 3'600'000'000'000}},
      CLI::AsNumberWithUnit::CASE_SENSITIVE);
  return app.add_option(name, ns, description)
      ->transform(CLI::Validator(
          [to_ns, zero_ok](std::string& s) {
            if (!s.empty() && std::isdigit(static_cast<unsigned char>(s.back())) != 0) s += 's';
            std::string error = to_ns(s);
            if (error.empty() && (s.front() == '-' || (!zero_ok && s == "0")))
              error = zero_ok ? "must not be negative" : "must be positive";
            return error.empty() ? error : error + " (examples: 60s, 5m, 1500ms)";
          },
          ""))
      ->option_text("<t>");
}

}  // namespace fastmm::cli
