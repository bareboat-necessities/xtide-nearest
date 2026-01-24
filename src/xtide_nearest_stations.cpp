/*
  Copyright 2026, Mikhail Grushinskiy
*/
#include "xtide_nearest/xtide_nearest.h"

#include <algorithm>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" {
#include <tcd.h>
}

namespace xtide_nearest {
namespace {

std::string trim_copy_local(std::string s) {
  auto is_ws = [](unsigned char c) { return std::isspace(c) != 0; };
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), [&](char c) { return !is_ws((unsigned char)c); }));
  s.erase(std::find_if(s.rbegin(), s.rend(), [&](char c) { return !is_ws((unsigned char)c); }).base(), s.end());
  return s;
}

bool contains_icase_local(std::string_view hay, std::string_view needle) {
  auto lower = [](std::string_view x) {
    std::string s(x);
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
  };
  auto H = lower(hay);
  auto N = lower(needle);
  return H.find(N) != std::string::npos;
}

bool is_current_units(const std::string& units) {
  if (contains_icase_local(units, "knot")) return true;
  if (contains_icase_local(units, "kt")) return true;
  if (contains_icase_local(units, "m/s")) return true;
  return false;
}

struct TideDbGuard {
  explicit TideDbGuard(const fs::path& p) : path(p), buf(p.string()) {
    if (!open_tide_db(buf.data())) {
      throw std::runtime_error("open_tide_db failed for: " + path.string());
    }
  }
  ~TideDbGuard() { close_tide_db(); }

  fs::path path;
  std::string buf;  // mutable storage for libtcd
};

// libtcd DB access is effectively global; serialize to be safe in server contexts.
std::mutex g_tcd_mutex;

fs::path norm_abs_local(const fs::path& p) {
  std::error_code ec;
  fs::path a = fs::absolute(p, ec);
  if (ec) a = p;
  a = a.lexically_normal();
#ifdef _WIN32
  a.make_preferred();
#endif
  return a;
}

std::vector<Station> load_stations_from_one_tcd(const fs::path& tcd_path) {
  std::lock_guard<std::mutex> lk(g_tcd_mutex);

  TideDbGuard db(tcd_path);
  const DB_HEADER_PUBLIC header = get_tide_db_header();

  std::vector<Station> stations;
  stations.reserve(static_cast<size_t>(header.number_of_records));

  for (NV_U_INT32 i = 0; i < header.number_of_records; ++i) {
    TIDE_RECORD rec{};
    const NV_INT32 got = read_tide_record(static_cast<NV_INT32>(i), &rec);
    if (got < 0) continue;

    std::string name = trim_copy_local(std::string(rec.header.name));
    if (name.empty()) continue;

    std::string units;
    if (const char* u = get_level_units(rec.level_units)) units = u;

    const bool is_current = is_current_units(units);

    Station st;
    st.name = std::move(name);
    st.lat_deg = rec.header.latitude;
    st.lon_deg = rec.header.longitude;
    st.kind = is_current ? Kind::Current : Kind::Tide;
    st.units = std::move(units);
    st.min_dir_deg = static_cast<int>(rec.min_direction);
    st.max_dir_deg = static_cast<int>(rec.max_direction);
    st.tcd_path = norm_abs_local(tcd_path);

    stations.push_back(std::move(st));
  }

  return stations;
}

} // namespace

std::vector<Station> LoadStations(const std::vector<fs::path>& tcd_files) {
  std::vector<Station> all;
  for (const auto& p : tcd_files) {
    auto v = load_stations_from_one_tcd(p);
    all.insert(all.end(), std::make_move_iterator(v.begin()), std::make_move_iterator(v.end()));
  }
  return all;
}

} // namespace xtide_nearest
