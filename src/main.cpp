#include <CLI/CLI.hpp>

#include <tide_db.h>   // libtcd (XTide)

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(_WIN32)
  #include <process.h>
  #define popen  _popen
  #define pclose _pclose
#endif

namespace fs = std::filesystem;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kEarthRadiusM = 6371000.0;

enum class Product { Tide, Current, Both };

struct Station {
  std::string name;
  double lat_deg = 0.0;
  double lon_deg = 0.0;
  bool is_current = false;

  // For current stations, nominal directions (degrees true).
  // libtcd uses 361 as "null" for direction fields in the TCD schema.
  int ebb_dir_deg = 361;    // usually min_direction
  int flood_dir_deg = 361;  // usually max_direction
};

struct TideDbGuard {
  explicit TideDbGuard(const std::string& path) {
    // open_tide_db returns non-zero on error in common libtcd builds.
    if (open_tide_db(const_cast<char*>(path.c_str())) != 0) {
      throw std::runtime_error("open_tide_db failed for: " + path);
    }
  }
  ~TideDbGuard() {
    close_tide_db();
  }
  TideDbGuard(const TideDbGuard&) = delete;
  TideDbGuard& operator=(const TideDbGuard&) = delete;
};

static inline double deg2rad(double d) { return d * kPi / 180.0; }

double haversine_m(double lat1_deg, double lon1_deg, double lat2_deg, double lon2_deg) {
  const double lat1 = deg2rad(lat1_deg);
  const double lon1 = deg2rad(lon1_deg);
  const double lat2 = deg2rad(lat2_deg);
  const double lon2 = deg2rad(lon2_deg);

  const double dlat = lat2 - lat1;
  const double dlon = lon2 - lon1;

  const double a = std::pow(std::sin(dlat / 2.0), 2) +
                   std::cos(lat1) * std::cos(lat2) * std::pow(std::sin(dlon / 2.0), 2);
  const double c = 2.0 * std::atan2(std::sqrt(a), std::sqrt(std::max(0.0, 1.0 - a)));
  return kEarthRadiusM * c;
}

std::string trim_cstr(const char* s) {
  if (!s) return {};
  std::string out(s);
  while (!out.empty() && (out.back() == '\0' || out.back() == ' ' || out.back() == '\n' || out.back() == '\r' || out.back() == '\t'))
    out.pop_back();
  size_t i = 0;
  while (i < out.size() && (out[i] == ' ' || out[i] == '\n' || out[i] == '\r' || out[i] == '\t'))
    ++i;
  return out.substr(i);
}

// Very small CSV splitter for XTide raw CSV (no quotes expected)
std::vector<std::string_view> split_csv(std::string_view line) {
  std::vector<std::string_view> cols;
  size_t start = 0;
  while (start <= line.size()) {
    size_t comma = line.find(',', start);
    if (comma == std::string_view::npos) {
      cols.emplace_back(line.substr(start));
      break;
    }
    cols.emplace_back(line.substr(start, comma - start));
    start = comma + 1;
  }
  // trim spaces around columns
  for (auto& c : cols) {
    while (!c.empty() && (c.front() == ' ' || c.front() == '\t')) c.remove_prefix(1);
    while (!c.empty() && (c.back() == ' '  || c.back() == '\t' || c.back() == '\r')) c.remove_suffix(1);
  }
  return cols;
}

template <class T>
bool from_chars_any(std::string_view sv, T& out) {
  // for integers: from_chars
  if constexpr (std::is_integral_v<T>) {
    const char* b = sv.data();
    const char* e = sv.data() + sv.size();
    auto [p, ec] = std::from_chars(b, e, out);
    return ec == std::errc{} && p == e;
  } else {
    // floating: strtod
    std::string tmp(sv);
    char* end = nullptr;
    out = static_cast<T>(std::strtod(tmp.c_str(), &end));
    return end && *end == '\0';
  }
}

std::string iso_utc_from_epoch(long long epoch_s) {
  std::time_t t = static_cast<std::time_t>(epoch_s);
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  std::ostringstream oss;
  oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
  return oss.str();
}

// Best-effort quoting for shell command arguments
std::string shell_quote(std::string_view s) {
#if defined(_WIN32)
  // Windows cmd quoting is messy; simplest: wrap in double-quotes and escape internal quotes.
  std::string out;
  out.push_back('"');
  for (char c : s) {
    if (c == '"') out += "\\\"";
    else out.push_back(c);
  }
  out.push_back('"');
  return out;
#else
  // POSIX: single-quote, escape embedded single-quotes
  std::string out;
  out.push_back('\'');
  for (char c : s) {
    if (c == '\'') out += "'\\''";
    else out.push_back(c);
  }
  out.push_back('\'');
  return out;
#endif
}

void set_env(const std::string& key, const std::string& value) {
#if defined(_WIN32)
  _putenv_s(key.c_str(), value.c_str());
#else
  setenv(key.c_str(), value.c_str(), 1);
#endif
}

struct Pipe {
  FILE* f = nullptr;
  explicit Pipe(const std::string& cmd) {
    f = popen(cmd.c_str(), "r");
    if (!f) throw std::runtime_error("popen failed for command: " + cmd);
  }
  ~Pipe() {
    if (f) pclose(f);
  }
  Pipe(const Pipe&) = delete;
  Pipe& operator=(const Pipe&) = delete;
};

std::vector<Station> load_stations_from_one_tcd(const fs::path& tcd_path) {
  TideDbGuard db(tcd_path.string());

  DB_HEADER header{};
  if (get_tide_db_header(&header) != 0) {
    throw std::runtime_error("get_tide_db_header failed for: " + tcd_path.string());
  }

  std::vector<Station> stations;
  stations.reserve(static_cast<size_t>(header.number_of_records));

  for (int i = 0; i < header.number_of_records; ++i) {
    TIDE_RECORD rec{};
    if (get_tide_record(i, &rec) != 0) {
      continue;
    }

    Station s;
    s.name = trim_cstr(rec.header.name);
    s.lat_deg = rec.header.latitude;
    s.lon_deg = rec.header.longitude;

    // Unit string comes from header's level_units table.
    // For type-1 records, rec.units is used; for type-2 records, rec.level_units is used.
    // (These are indices into the "level units" array in the database header.)
    int unit_index = 0;
    if (rec.header.record_type == 1) unit_index = rec.units;
    else unit_index = rec.level_units;

    std::string unit_str = trim_cstr(header.level_units[unit_index]);
    // Current stations use "kt" in XTide databases.
    s.is_current = (unit_str == "kt" || unit_str.find("kt") != std::string::npos);

    if (s.is_current) {
      // Nominal current directions (degrees true) are stored in min_direction/max_direction.
      // Using XTide/NOS convention: min ~= ebb, max ~= flood.
      // 361 indicates "null" for direction fields.
      s.ebb_dir_deg = rec.min_direction;
      s.flood_dir_deg = rec.max_direction;
    }

    if (!s.name.empty()) stations.push_back(std::move(s));
  }

  return stations;
}

std::vector<Station> load_stations(const std::vector<fs::path>& tcd_files) {
  // Merge by name (first wins).
  std::unordered_map<std::string, Station> uniq;

  for (const auto& p : tcd_files) {
    auto v = load_stations_from_one_tcd(p);
    for (auto& s : v) {
      if (!uniq.contains(s.name)) uniq.emplace(s.name, std::move(s));
    }
  }

  std::vector<Station> out;
  out.reserve(uniq.size());
  for (auto& kv : uniq) out.push_back(std::move(kv.second));
  return out;
}

std::optional<std::pair<Station, double>> nearest_station(
  const std::vector<Station>& stations, double lat, double lon, bool want_current
) {
  double best = std::numeric_limits<double>::infinity();
  const Station* best_s = nullptr;

  for (const auto& s : stations) {
    if (s.is_current != want_current) continue;
    double d = haversine_m(lat, lon, s.lat_deg, s.lon_deg);
    if (d < best) {
      best = d;
      best_s = &s;
    }
  }
  if (!best_s) return std::nullopt;
  return std::make_pair(*best_s, best);
}

struct PredictArgs {
  std::string station_name;
  std::string start;   // "YYYY-MM-DD HH:MM"
  std::string end;     // "YYYY-MM-DD HH:MM"
  std::string step;    // "HH:MM"
  bool utc = true;
  bool no_units_suffix = true;
  std::optional<std::string> preferred_tide_units; // "ft" or "m"
};

std::string build_tide_command(const PredictArgs& a) {
  // tide options (see man page):
  // -l location, -b begin, -e end, -s step, -f c CSV, -m r raw, -ou y strip unit suffixes, -z y force UTC :contentReference[oaicite:5]{index=5}
  std::ostringstream cmd;
  cmd << "tide"
      << " -f c"
      << " -m r"
      << " -l " << shell_quote(a.station_name)
      << " -b " << shell_quote(a.start)
      << " -e " << shell_quote(a.end)
      << " -s " << shell_quote(a.step);

  if (a.no_units_suffix) cmd << " -ou y";
  if (a.utc) cmd << " -z y";

  if (a.preferred_tide_units.has_value()) {
    cmd << " -u " << *a.preferred_tide_units;
  }
  return cmd.str();
}

void emit_tide_series(
  const Station& st,
  const PredictArgs& args,
  bool print_header
) {
  const std::string cmd = build_tide_command(args);
  Pipe pipe(cmd);

  if (print_header) {
    std::cout << "# product=tide\n";
    std::cout << "# station=" << st.name << "\n";
    std::cout << "# station_lat_deg=" << st.lat_deg << "\n";
    std::cout << "# station_lon_deg=" << st.lon_deg << "\n";
    std::cout << "epoch_s,iso_utc,height\n";
  }

  std::array<char, 8192> buf{};
  while (std::fgets(buf.data(), static_cast<int>(buf.size()), pipe.f)) {
    std::string line(buf.data());
    if (line.empty()) continue;
    if (line[0] == '#') continue;

    auto cols = split_csv(line);
    if (cols.size() < 2) continue;

    long long epoch = 0;
    double h = 0.0;
    if (!from_chars_any(cols[0], epoch)) continue; // skip non-data header lines
    if (!from_chars_any(cols[1], h)) continue;

    std::cout << epoch << "," << iso_utc_from_epoch(epoch) << "," << h << "\n";
  }
}

void emit_current_series(
  const Station& st,
  const PredictArgs& args,
  bool print_header
) {
  const std::string cmd = build_tide_command(args);
  Pipe pipe(cmd);

  const bool have_nominal_dirs = (st.ebb_dir_deg >= 0 && st.ebb_dir_deg <= 360 &&
                                  st.flood_dir_deg >= 0 && st.flood_dir_deg <= 360);

  if (print_header) {
    std::cout << "# product=current\n";
    std::cout << "# station=" << st.name << "\n";
    std::cout << "# station_lat_deg=" << st.lat_deg << "\n";
    std::cout << "# station_lon_deg=" << st.lon_deg << "\n";
    if (have_nominal_dirs) {
      std::cout << "# station_flood_dir_deg_true=" << st.flood_dir_deg << "\n";
      std::cout << "# station_ebb_dir_deg_true=" << st.ebb_dir_deg << "\n";
    }
    std::cout << "epoch_s,iso_utc,speed_kt_signed,speed_kt,dir_deg_true,east_kt,north_kt\n";
  }

  std::array<char, 8192> buf{};
  while (std::fgets(buf.data(), static_cast<int>(buf.size()), pipe.f)) {
    std::string line(buf.data());
    if (line.empty()) continue;
    if (line[0] == '#') continue;

    auto cols = split_csv(line);
    if (cols.size() < 2) continue;

    long long epoch = 0;
    double signed_speed = 0.0;
    if (!from_chars_any(cols[0], epoch)) continue;
    if (!from_chars_any(cols[1], signed_speed)) continue;

    double dir_deg = std::numeric_limits<double>::quiet_NaN();

    // If tide emits a direction column, use it (best case).
    if (cols.size() >= 3) {
      double d = 0.0;
      if (from_chars_any(cols[2], d)) dir_deg = d;
    }

    // Otherwise derive from sign and nominal flood/ebb directions.
    if (!std::isfinite(dir_deg) && have_nominal_dirs) {
      dir_deg = (signed_speed >= 0.0) ? static_cast<double>(st.flood_dir_deg)
                                      : static_cast<double>(st.ebb_dir_deg);
    }

    const double speed = std::abs(signed_speed);

    double east = std::numeric_limits<double>::quiet_NaN();
    double north = std::numeric_limits<double>::quiet_NaN();
    if (std::isfinite(dir_deg)) {
      const double r = deg2rad(dir_deg);
      east = speed * std::sin(r);
      north = speed * std::cos(r);
    }

    std::cout << epoch << ","
              << iso_utc_from_epoch(epoch) << ","
              << signed_speed << ","
              << speed << ",";

    if (std::isfinite(dir_deg)) std::cout << dir_deg;
    std::cout << ",";

    if (std::isfinite(east)) std::cout << east;
    std::cout << ",";

    if (std::isfinite(north)) std::cout << north;
    std::cout << "\n";
  }
}

} // namespace

int main(int argc, char** argv) {
  CLI::App app{"Find nearest XTide station (tide/current) and emit prediction CSV."};

  double lat = std::numeric_limits<double>::quiet_NaN();
  double lon = std::numeric_limits<double>::quiet_NaN();
  std::vector<std::string> tcd_paths;

  std::string start = "2026-01-22 00:00";
  std::string end   = "2026-01-23 00:00";
  std::string step  = "00:10";

  std::string product_s = "both";
  bool utc = true;
  bool header = true;

  std::string tide_units; // optional: "ft" or "m"

  app.add_option("--lat", lat, "Latitude (deg)")->required();
  app.add_option("--lon", lon, "Longitude (deg)")->required();
  app.add_option("--tcd", tcd_paths, "Path to harmonics .tcd file (repeatable)")->required();

  app.add_option("--start", start, "Begin time: \"YYYY-MM-DD HH:MM\" (UTC if --utc)")->default_val(start);
  app.add_option("--end", end, "End time: \"YYYY-MM-DD HH:MM\" (UTC if --utc)")->default_val(end);
  app.add_option("--step", step, "Step interval: \"HH:MM\"")->default_val(step);

  app.add_option("--product", product_s, "tide | current | both")->default_val(product_s);
  app.add_flag("--utc,!--local", utc, "Force UTC for predictions (default: --utc)");
  app.add_flag("--header,!--no-header", header, "Print CSV header and metadata (default: --header)");

  app.add_option("--tide-units", tide_units, "Preferred tide units: ft | m (optional)");

  CLI11_PARSE(app, argc, argv);

  Product product = Product::Both;
  if (product_s == "tide") product = Product::Tide;
  else if (product_s == "current") product = Product::Current;
  else if (product_s == "both") product = Product::Both;
  else throw CLI::ValidationError("--product must be tide|current|both");

  std::vector<fs::path> tcd_files;
  tcd_files.reserve(tcd_paths.size());
  for (const auto& s : tcd_paths) tcd_files.emplace_back(fs::path(s));

  // Ensure `tide` sees the same harmonics databases: HFILE_PATH is a colon-separated list. :contentReference[oaicite:6]{index=6}
  std::ostringstream hfile_path;
  for (size_t i = 0; i < tcd_paths.size(); ++i) {
#if defined(_WIN32)
    // XTide on Windows typically accepts ';' too; but ':' is most common on Unix.
    if (i) hfile_path << ";";
#else
    if (i) hfile_path << ":";
#endif
    hfile_path << tcd_paths[i];
  }
  set_env("HFILE_PATH", hfile_path.str());

  // Load all stations through libtcd.
  auto stations = load_stations(tcd_files);

  auto tide_pick = nearest_station(stations, lat, lon, /*want_current=*/false);
  auto cur_pick  = nearest_station(stations, lat, lon, /*want_current=*/true);

  if (product == Product::Tide || product == Product::Both) {
    if (!tide_pick) {
      std::cerr << "No tide stations found in provided TCD files.\n";
      return 2;
    }
    const auto& [st, dist_m] = *tide_pick;
    std::cerr << "[tide] nearest station: " << st.name
              << " (distance_km=" << (dist_m / 1000.0) << ")\n";

    PredictArgs args;
    args.station_name = st.name;
    args.start = start;
    args.end = end;
    args.step = step;
    args.utc = utc;
    if (!tide_units.empty()) args.preferred_tide_units = tide_units;

    emit_tide_series(st, args, header);
    if (product == Product::Both) std::cout << "\n";
  }

  if (product == Product::Current || product == Product::Both) {
    if (!cur_pick) {
      std::cerr << "No current stations found in provided TCD files.\n";
      return 3;
    }
    const auto& [st, dist_m] = *cur_pick;
    std::cerr << "[current] nearest station: " << st.name
              << " (distance_km=" << (dist_m / 1000.0) << ")\n";

    PredictArgs args;
    args.station_name = st.name;
    args.start = start;
    args.end = end;
    args.step = step;
    args.utc = utc;

    emit_current_series(st, args, header);
  }

  return 0;
}
