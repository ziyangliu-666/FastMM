#include "fastmm/core/latency.hpp"

#include <fmt/format.h>

namespace fastmm {

std::string format_latency_text(const LatencySnapshot& s) {
  std::string out;
  fmt::format_to(std::back_inserter(out),
                 "{:<14}{:>10}{:>10}{:>10}{:>10}{:>10}{:>12}\n",
                 "interval",
                 "count",
                 "p50_ns",
                 "p90_ns",
                 "p99_ns",
                 "p99.9_ns",
                 "max_ns");
  for (std::size_t i = 0; i < static_cast<std::size_t>(LatencyInterval::Count); ++i) {
    const auto& st = s.interval[i];
    fmt::format_to(std::back_inserter(out),
                   "{:<14}{:>10}{:>10}{:>10}{:>10}{:>10}{:>12}\n",
                   to_string(static_cast<LatencyInterval>(i)),
                   st.count,
                   st.p50,
                   st.p90,
                   st.p99,
                   st.p999,
                   st.max);
  }
  return out;
}

std::string format_latency_csv(const LatencySnapshot& s, bool header) {
  std::string out;
  if (header) out += "ts_ns,interval,count,p50_ns,p90_ns,p99_ns,p999_ns,max_ns,mean_ns\n";
  for (std::size_t i = 0; i < static_cast<std::size_t>(LatencyInterval::Count); ++i) {
    const auto& st = s.interval[i];
    fmt::format_to(std::back_inserter(out),
                   "{},{},{},{},{},{},{},{},{}\n",
                   s.ts_ns,
                   to_string(static_cast<LatencyInterval>(i)),
                   st.count,
                   st.p50,
                   st.p90,
                   st.p99,
                   st.p999,
                   st.max,
                   st.mean);
  }
  return out;
}

std::string format_latency_prometheus(const LatencySnapshot& s, const char* prefix) {
  std::string out;
  for (std::size_t i = 0; i < static_cast<std::size_t>(LatencyInterval::Count); ++i) {
    const auto& st = s.interval[i];
    const char* name = to_string(static_cast<LatencyInterval>(i));
    fmt::format_to(std::back_inserter(out),
                   "{}_latency_ns{{interval=\"{}\",quantile=\"0.5\"}} {}\n",
                   prefix,
                   name,
                   st.p50);
    fmt::format_to(std::back_inserter(out),
                   "{}_latency_ns{{interval=\"{}\",quantile=\"0.9\"}} {}\n",
                   prefix,
                   name,
                   st.p90);
    fmt::format_to(std::back_inserter(out),
                   "{}_latency_ns{{interval=\"{}\",quantile=\"0.99\"}} {}\n",
                   prefix,
                   name,
                   st.p99);
    fmt::format_to(std::back_inserter(out),
                   "{}_latency_ns{{interval=\"{}\",quantile=\"0.999\"}} {}\n",
                   prefix,
                   name,
                   st.p999);
    fmt::format_to(
        std::back_inserter(out), "{}_latency_ns_max{{interval=\"{}\"}} {}\n", prefix, name, st.max);
    fmt::format_to(std::back_inserter(out),
                   "{}_latency_ns_count{{interval=\"{}\"}} {}\n",
                   prefix,
                   name,
                   st.count);
  }
  return out;
}

}  // namespace fastmm
