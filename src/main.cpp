/*

  Copyright 2026, Mikhail Grushinskiy

*/

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <CLI/CLI.hpp>

extern "C" {
#include <tcd.h>
}

namespace fs = std::filesystem;

namespace {

constexpr double kEarthRadiusKm = 6371.0088;
constexpr double kPi = 3.141592653589793238462643383279502884;

double deg2rad(double d) { return d * (kPi / 180.0); }

double haversine_km(double lat1, double lon1, double lat2, double lon2) {
  const double p1 = deg2rad(lat1);
  const double p2 = deg2rad(lat2);
  const double dp = deg2rad(lat2 - lat1);
  const double dl = deg2rad(lon2 - lon1);

  const double sdp = std::sin(dp * 0.5);
  const double sdl = std::sin(dl * 0.5);
  const double a = sdp * sdp + std::cos(p1) * std::cos(p2) * (sdl * sdl);
  const double c = 2.0 * std::atan2(std::sqrt(a), std::sqrt(std::max(0.0, 1.0 - a)));
  return kEarthRadiusKm * c;
}

std::string trim_copy(std::string s) {
  auto is_ws = [](unsigned char c) { return std::isspace(c) != 0; };
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), [&](char c) { return !is_ws((unsigned char)c); }));
  s.erase(std::find_if(s.rbegin(), s.rend(), [&](char c) { return !is_ws((unsigned char)c); }).base(), s.end());
  return s;
}

std::string to_lower_copy(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool contains_icase(std::string_view hay, std::string_view needle) {
  std::string H(hay);
  std::string N(needle);
  H = to_lower_copy(std::move(H));
  N = to_lower_copy(std::move(N));
  return H.find(N) != std::string::npos;
}

enum class Kind { Tide, Current };

std::string kind_str(Kind k) { return (k == Kind::Current) ? "current" : "tide"; }

struct Station {
  std::string name;
  double lat_deg{};
  double lon_deg{};
  Kind kind{};
  std::string units;      // e.g. "meters", "feet", "knots"
  int min_dir_deg{361};   // 0..360 or 361 = none
  int max_dir_deg{361};   // 0..360 or 361 = none
  fs::path tcd_path;      // which harmonics file it came from
};

struct Candidate {
  Station st;
  double distance_km{};
};

struct TideDbGuard {
  explicit TideDbGuard(const fs::path& p) : path(p), buf(p.string()) {
    // libtcd expects NV_CHAR* (mutable). We keep storage stable in buf.
    if (!open_tide_db(buf.data())) {
      throw std::runtime_error("open_tide_db failed for: " + path.string());
    }
  }
  ~TideDbGuard() { close_tide_db(); }

  fs::path path;
  std::string buf;
};

bool is_current_units(const std::string& units) {
  // Common current units: knots, kt, kts, m/s
  if (contains_icase(units, "knot")) return true;
  if (contains_icase(units, "kt")) return true;
  if (contains_icase(units, "m/s")) return true;
  return false;
}

std::optional<std::string> getenv_str(const char* name) {
  if (const char* v = std::getenv(name)) return std::string(v);
  return std::nullopt;
}

void setenv_portable(const std::string& key, const std::string& val) {
#ifdef _WIN32
  _putenv_s(key.c_str(), val.c_str());
#else
  ::setenv(key.c_str(), val.c_str(), 1);
#endif
}

void unsetenv_portable(const std::string& key) {
#ifdef _WIN32
  _putenv_s(key.c_str(), "");
#else
  ::unsetenv(key.c_str());
#endif
}

#ifdef _WIN32
FILE* popen_portable(const char* cmd, const char* mode) { return _popen(cmd, mode); }
int pclose_portable(FILE* f) { return _pclose(f); }
#else
FILE* popen_portable(const char* cmd, const char* mode) { return popen(cmd, mode); }
int pclose_portable(FILE* f) { return pclose(f); }
#endif

std::string shell_quote_double(std::string s) {
  // Wrap in "..." and escape backslashes and quotes.
  std::string out;
  out.reserve(s.size() + 2);
  out.push_back('"');
  for (char c : s) {
    if (c == '\\' || c == '"') out.push_back('\\');
    out.push_back(c);
  }
  out.push_back('"');
  return out;
}

std::vector<Station> load_stations_from_one_tcd(const fs::path& tcd_path) {
  TideDbGuard db(tcd_path);

  const DB_HEADER_PUBLIC header = get_tide_db_header();

  std::vector<Station> stations;
  stations.reserve(static_cast<size_t>(header.number_of_records));

  for (NV_U_INT32 i = 0; i < header.number_of_records; ++i) {
    TIDE_RECORD rec{};
    const NV_INT32 got = read_tide_record(static_cast<NV_INT32>(i), &rec);
    if (got < 0) continue;

    // rec.header.name is a fixed-size array, not a pointer.
    std::string name = trim_copy(std::string(rec.header.name));
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
    st.tcd_path = tcd_path;

    stations.push_back(std::move(st));
  }

  return stations;
}

std::vector<Station> load_stations(const std::vector<fs::path>& tcd_files) {
  std::vector<Station> all;
  for (const auto& p : tcd_files) {
    auto v = load_stations_from_one_tcd(p);
    all.insert(all.end(), std::make_move_iterator(v.begin()), std::make_move_iterator(v.end()));
  }
  return all;
}

std::vector<Candidate> nearest(const std::vector<Station>& stations,
                               double lat, double lon,
                               std::optional<Kind> kind_filter,
                               size_t top_n) {
  std::vector<Candidate> out;
  out.reserve(stations.size());

  for (const auto& st : stations) {
    if (kind_filter && st.kind != *kind_filter) continue;
    Candidate c;
    c.st = st;
    c.distance_km = haversine_km(lat, lon, st.lat_deg, st.lon_deg);
    out.push_back(std::move(c));
  }

  std::sort(out.begin(), out.end(), [](const Candidate& a, const Candidate& b) {
    return a.distance_km < b.distance_km;
  });

  if (out.size() > top_n) out.resize(top_n);
  return out;
}

std::string join_hfile_path(const std::vector<fs::path>& tcd_files) {
#ifdef _WIN32
  const char sep = ';';
#else
  const char sep = ':';
#endif
  std::ostringstream oss;
  for (size_t i = 0; i < tcd_files.size(); ++i) {
    if (i) oss << sep;
    oss << tcd_files[i].string();
  }
  return oss.str();
}

int run_tide_and_stream(const std::string& tide_bin,
                        const std::vector<fs::path>& tcd_files,
                        const std::string& station_name,
                        const std::string& begin,
                        const std::string& end,
                        const std::string& step_hhmm,
                        bool utc,
                        bool suppress_sunmoon,
                        bool omit_units,
                        bool emit_metadata,
                        double dist_km,
                        Kind kind) {
  const auto old_hfile = getenv_str("HFILE_PATH");
  setenv_portable("HFILE_PATH", join_hfile_path(tcd_files));

  std::ostringstream cmd;
  cmd << shell_quote_double(tide_bin)
      << " -l " << shell_quote_double(station_name)
      << " -b " << shell_quote_double(begin)
      << " -e " << shell_quote_double(end)
      << " -m r -f c"
      << " -s " << shell_quote_double(step_hhmm)
      << " -z " << (utc ? "y" : "n");

  if (suppress_sunmoon) cmd << " -em pSsMm";
  if (omit_units) cmd << " -ou y";

  const std::string cmd_str = cmd.str();

  FILE* pipe = popen_portable(cmd_str.c_str(), "r");
  if (!pipe) {
    if (old_hfile) setenv_portable("HFILE_PATH", *old_hfile);
    else unsetenv_portable("HFILE_PATH");
    std::cerr << "ERROR: failed to run tide\nCMD: " << cmd_str << "\n";
    return 2;
  }

  if (emit_metadata) {
    std::cout << "# kind=" << kind_str(kind)
              << " station=" << station_name
              << " distance_km=" << std::fixed << std::setprecision(3) << dist_km
              << " begin=" << begin
              << " end=" << end
              << " step=" << step_hhmm
              << " utc=" << (utc ? "true" : "false")
              << "\n";
  }

  std::array<char, 8192> buf{};
  while (std::fgets(buf.data(), static_cast<int>(buf.size()), pipe)) {
    std::cout << buf.data();
  }

  const int rc = pclose_portable(pipe);

  if (old_hfile) setenv_portable("HFILE_PATH", *old_hfile);
  else unsetenv_portable("HFILE_PATH");

  if (rc != 0) {
    std::cerr << "WARNING: tide exited with code " << rc << "\n"
              << "CMD: " << cmd_str << "\n";
  }
  return (rc == 0) ? 0 : 3;
}

} // namespace

int main(int argc, char** argv) {
  CLI::App app{"Find nearest XTide stations (tides + currents) and optionally stream predictions via tide(1)."};

  app.require_subcommand(1);

  double lat = 0.0, lon = 0.0;
  std::vector<std::string> tcd_in;

  app.add_option("--lat", lat, "Latitude (deg)")->required();
  app.add_option("--lon", lon, "Longitude (deg)")->required();
  app.add_option("--tcd", tcd_in, "Path to harmonics .tcd file (repeatable)")->required();

  auto* cmd_nearest = app.add_subcommand("nearest", "List nearest stations (tide/current) and distances.");
  bool want_tide = true;
  bool want_current = true;
  size_t top_n = 5;
  cmd_nearest->add_flag("--tide,!--no-tide", want_tide, "Include tide stations (default on)");
  cmd_nearest->add_flag("--current,!--no-current", want_current, "Include current stations (default on)");
  cmd_nearest->add_option("--top", top_n, "How many results per kind")->default_val(5);

  auto* cmd_predict = app.add_subcommand("predict", "Find nearest station of a kind and stream CSV predictions to stdout.");
  std::string kind_s = "current";
  std::string begin;
  std::string end;
  std::string step = "00:10";
  std::string tide_bin = "tide";
  bool utc = false;
  bool suppress_sunmoon = true;
  bool omit_units = false;
  bool emit_metadata = true;

  cmd_predict->add_option("--kind", kind_s, "tide | current")->default_val("current");
  cmd_predict->add_option("--begin", begin, "Begin time: \"YYYY-MM-DD HH:MM\"")->required();
  cmd_predict->add_option("--end", end, "End time: \"YYYY-MM-DD HH:MM\"")->required();
  cmd_predict->add_option("--step", step, "Step interval for raw mode: \"HH:MM\"")->default_val("00:10");
  cmd_predict->add_option("--tide-bin", tide_bin, "Path to tide(1) executable")->default_val("tide");
  cmd_predict->add_flag("--utc", utc, "Coerce timestamps to UTC (tide -z y)");
  cmd_predict->add_flag("--no-sunmoon", suppress_sunmoon, "Suppress sun/moon events (default on)")->default_val(true);
  cmd_predict->add_flag("--omit-units", omit_units, "Omit unit suffix in numeric fields (tide -ou y)");
  cmd_predict->add_flag("--no-meta", emit_metadata, "Disable metadata header line")->default_val(true);

  CLI11_PARSE(app, argc, argv);

  std::vector<fs::path> tcd_files;
  tcd_files.reserve(tcd_in.size());
  for (const auto& s : tcd_in) tcd_files.emplace_back(fs::path(s));

  const auto stations = load_stations(tcd_files);

  auto parse_kind = [&](const std::string& s) -> Kind {
    const auto ls = to_lower_copy(s);
    if (ls == "tide") return Kind::Tide;
    if (ls == "current") return Kind::Current;
    throw std::runtime_error("Invalid --kind. Use 'tide' or 'current'.");
  };

  if (*cmd_nearest) {
    if (!want_tide && !want_current) {
      std::cerr << "Nothing to do: both --no-tide and --no-current set.\n";
      return 2;
    }

    auto print_block = [&](Kind k) {
      const auto cand = nearest(stations, lat, lon, k, top_n);
      std::cout << "kind=" << kind_str(k) << " results=" << cand.size() << "\n";
      std::cout << "distance_km,name,lat,lon,units,min_dir_deg,max_dir_deg,tcd\n";
      for (const auto& c : cand) {
        std::cout << std::fixed << std::setprecision(3)
                  << c.distance_km << ","
                  << shell_quote_double(c.st.name) << ","
                  << std::setprecision(6) << c.st.lat_deg << ","
                  << std::setprecision(6) << c.st.lon_deg << ","
                  << shell_quote_double(c.st.units) << ","
                  << c.st.min_dir_deg << ","
                  << c.st.max_dir_deg << ","
                  << shell_quote_double(c.st.tcd_path.string())
                  << "\n";
      }
      std::cout << "\n";
    };

    if (want_tide) print_block(Kind::Tide);
    if (want_current) print_block(Kind::Current);
    return 0;
  }

  if (*cmd_predict) {
    const Kind k = parse_kind(kind_s);
    const auto cand = nearest(stations, lat, lon, k, 1);
    if (cand.empty()) {
      std::cerr << "No stations found for kind=" << kind_str(k) << "\n";
      return 2;
    }

    const auto& best = cand.front();
    return run_tide_and_stream(
      tide_bin,
      tcd_files,
      best.st.name,
      begin,
      end,
      step,
      utc,
      suppress_sunmoon,
      omit_units,
      emit_metadata,
      best.distance_km,
      k
    );
  }

  return 0;
}
