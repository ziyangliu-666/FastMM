#include "fastmm/sim/recorded_latency.hpp"

#include <algorithm>

namespace fastmm::sim {

RecordedLatency::RecordedLatency(const std::vector<Sample>& samples) {
  for (const Sample& s : samples) by_path_[static_cast<std::size_t>(s.path)].push_back(s);
  for (std::vector<Sample>& v : by_path_) {
    std::stable_sort(
        v.begin(), v.end(), [](const Sample& a, const Sample& b) { return a.t_ns < b.t_ns; });
  }
}

const RecordedLatency::Sample* RecordedLatency::nearest(const std::vector<Sample>& v,
                                                        Timestamp t,
                                                        VenueId venue,
                                                        std::uint32_t instrument,
                                                        bool by_instrument) const {
  if (v.empty()) return nullptr;
  const auto it = std::lower_bound(
      v.begin(), v.end(), t.ns, [](const Sample& s, std::int64_t x) { return s.t_ns < x; });
  const Sample* best = nullptr;
  std::int64_t best_d = 0;
  const auto consider = [&](const Sample& s) {
    if (s.venue != venue) return;
    if (by_instrument && s.instrument != instrument) return;
    const std::int64_t d = s.t_ns > t.ns ? s.t_ns - t.ns : t.ns - s.t_ns;
    if (best == nullptr || d < best_d) {
      best = &s;
      best_d = d;
    }
  };
  const std::int64_t w = (by_instrument ? twin_window : window).ns;
  for (auto r = it; r != v.end() && r->t_ns - t.ns <= w; ++r) consider(*r);
  for (auto l = it; l != v.begin();) {
    --l;
    if (t.ns - l->t_ns > w) break;
    consider(*l);
  }
  return best;
}

Duration RecordedLatency::out(Path path,
                              VenueId venue,
                              std::uint32_t instrument,
                              Timestamp t) const {
  const std::vector<Sample>& v = by_path_[static_cast<std::size_t>(path)];
  const Sample* s = nearest(v, t, venue, instrument, true);
  if (s == nullptr) s = nearest(v, t, venue, instrument, false);
  return s == nullptr ? Duration{} : Duration{s->took_ns};
}

Duration RecordedLatency::in(Path path, VenueId venue, Timestamp t) const {
  const Sample* s = nearest(by_path_[static_cast<std::size_t>(path)], t, venue, 0, false);
  if (s == nullptr) {
    const Path other = path == Path::Fill ? Path::Ack : Path::Fill;
    s = nearest(by_path_[static_cast<std::size_t>(other)], t, venue, 0, false);
  }
  return s == nullptr ? Duration{} : Duration{s->took_ns};
}

}  // namespace fastmm::sim
