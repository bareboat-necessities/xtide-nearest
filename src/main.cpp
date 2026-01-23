/*

  Copyright 2026, Mikhail Grushinskiy

*/

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <CLI/CLI.hpp>

extern "C" {
#include <tcd.h>
}

#ifndef _WIN32
#include <sys/wait.h>
#endif

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX 1
#endif
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace {

constexpr double kEarthRadiusKm = 6371.0088;
constexpr double kPi = 3.141592653589793238462643383279502884;

// Cache policy: keep nearest station selection for "sailboat-ish" movement.
constexpr int64_t kCacheTtlSec = 20 * 60;      // 20 min
constexpr double  kCacheMaxSpeedKt = 15.0;     // default sailboat speed bound
constexpr double  kCacheMinRadiusKm = 0.5;     // always allow small jitter even at dt~0

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

bool ieq(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
  }
  return true;
}

bool is_tcd_file(const fs::path& p) {
  if (!p.has_extension()) return false;
  return ieq(p.extension().string(), ".tcd");
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

#ifndef _WIN32
FILE* popen_portable(const char* cmd, const char* mode) { return popen(cmd, mode); }
int pclose_portable(FILE* f) { return pclose(f); }
#endif

std::string shell_quote_double(std::string s) {
  // Wrap in "..." and escape backslashes and quotes.
  // (Good enough for POSIX /bin/sh -c via popen(). Not used on Windows.)
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
  fs::path tcd_path;      // which .tcd file it came from
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

// ------------------------
// Default search locations
// ------------------------

std::vector<fs::path> default_tcd_dirs() {
  // User-requested defaults (Linux-ish paths).
  std::vector<fs::path> dirs = {
      "/etc/tcdata",
      "/usr/share/tcdata",
      "/usr/share/opencpn/tcdata",
      "/usr/local/share/opencpn/tcdata",
  };

#ifdef _WIN32
  // Also search near the executable (common for Windows zip installs).
  wchar_t wbuf[MAX_PATH];
  DWORD n = GetModuleFileNameW(nullptr, wbuf, MAX_PATH);
  if (n > 0 && n < MAX_PATH) {
    fs::path exe = fs::path(wbuf);
    fs::path d = exe.parent_path();
    dirs.push_back(d);
    dirs.push_back(d / "tcdata");
    dirs.push_back(d / "harmonics");
  }
  dirs.push_back(fs::current_path());
#endif
  return dirs;
}

// ------------------------
// Config file (path list)
// ------------------------

struct TcdConfig {
  std::vector<fs::path> sources;  // files or dirs
};

std::optional<fs::path> default_config_path_guess() {
  if (auto env = getenv_str("XTIDE_NEAREST_CONFIG")) return fs::path(*env);

#ifdef _WIN32
  if (auto appdata = getenv_str("APPDATA")) {
    fs::path p = fs::path(*appdata) / "xtide-nearest" / "tcd.conf";
    return p;
  }
  return fs::path("tcd.conf");
#else
  if (auto xdg = getenv_str("XDG_CONFIG_HOME")) {
    return fs::path(*xdg) / "xtide-nearest" / "tcd.conf";
  }
  if (auto home = getenv_str("HOME")) {
    return fs::path(*home) / ".config" / "xtide-nearest" / "tcd.conf";
  }
  return fs::path("/etc") / "xtide-nearest" / "tcd.conf";
#endif
}

std::optional<TcdConfig> read_tcd_config(const fs::path& cfg_path) {
  std::error_code ec;
  if (!fs::exists(cfg_path, ec) || ec) return std::nullopt;

  std::ifstream in(cfg_path);
  if (!in) return std::nullopt;

  TcdConfig cfg;
  const fs::path base = cfg_path.parent_path();

  std::string line;
  while (std::getline(in, line)) {
    line = trim_copy(line);
    if (line.empty()) continue;
    if (line[0] == '#') continue;

    // Allow optional prefixes: "dir:", "dir=", "file:", "file="
    auto parse_pref = [&](std::string_view key) -> std::optional<std::string> {
      if (line.size() < key.size()) return std::nullopt;
      if (!ieq(std::string_view(line).substr(0, key.size()), key)) return std::nullopt;
      std::string rest = trim_copy(line.substr(key.size()));
      if (!rest.empty() && (rest[0] == ':' || rest[0] == '=')) rest = trim_copy(rest.substr(1));
      return rest.empty() ? std::optional<std::string>{} : std::optional<std::string>{rest};
    };

    std::optional<std::string> path_s;
    if (auto r = parse_pref("dir"); r && !r->empty()) path_s = *r;
    else if (auto r = parse_pref("file"); r && !r->empty()) path_s = *r;
    else path_s = line;

    fs::path p(*path_s);
    if (p.is_relative()) p = (base / p).lexically_normal();
    cfg.sources.push_back(p);
  }

  return cfg;
}

// ------------------------
// Discover *.tcd (2 levels)
// ------------------------

fs::path norm_abs(const fs::path& p) {
  std::error_code ec;
  fs::path a = fs::absolute(p, ec);
  if (ec) a = p;
  a = a.lexically_normal();
#ifdef _WIN32
  a.make_preferred();
#endif
  return a;
}

std::string norm_key(const fs::path& p) {
#ifdef _WIN32
  return to_lower_copy(norm_abs(p).string());
#else
  return norm_abs(p).string();
#endif
}

void add_unique_path(std::vector<fs::path>& out,
                     std::unordered_set<std::string>& seen,
                     const fs::path& p) {
  fs::path a = norm_abs(p);
  std::string key = norm_key(a);
  if (seen.insert(key).second) out.push_back(std::move(a));
}

void discover_tcd_from_dir_2levels(const fs::path& dir,
                                  std::vector<fs::path>& out,
                                  std::unordered_set<std::string>& seen) {
  std::error_code ec0;
  if (!fs::exists(dir, ec0) || ec0) return;
  if (!fs::is_directory(dir, ec0) || ec0) return;

  auto scan_files_in_dir = [&](const fs::path& d) {
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(d, fs::directory_options::skip_permission_denied, ec)) {
      if (ec) break;
      if (e.is_regular_file(ec) && !ec && is_tcd_file(e.path())) add_unique_path(out, seen, e.path());
    }
  };

  // depth 0
  scan_files_in_dir(dir);

  // depth 1 and 2
  std::error_code ec1;
  for (const auto& e1 : fs::directory_iterator(dir, fs::directory_options::skip_permission_denied, ec1)) {
    if (ec1) break;
    std::error_code ecA;
    if (!e1.is_directory(ecA) || ecA) continue;
    const fs::path d1 = e1.path();

    scan_files_in_dir(d1);  // depth 1

    std::error_code ec2;
    for (const auto& e2 : fs::directory_iterator(d1, fs::directory_options::skip_permission_denied, ec2)) {
      if (ec2) break;
      std::error_code ecB;
      if (!e2.is_directory(ecB) || ecB) continue;
      const fs::path d2 = e2.path();
      scan_files_in_dir(d2); // depth 2
    }
  }
}

std::vector<fs::path> discover_tcd_files(const std::vector<fs::path>& sources, bool include_default_dirs) {
  std::vector<fs::path> out;
  std::unordered_set<std::string> seen;

  auto add_source = [&](const fs::path& src0) {
    fs::path src = src0;
    std::error_code ec;
    if (!fs::exists(src, ec) || ec) return;

    if (fs::is_regular_file(src, ec) && !ec) {
      if (is_tcd_file(src)) add_unique_path(out, seen, src);
      return;
    }
    if (fs::is_directory(src, ec) && !ec) {
      discover_tcd_from_dir_2levels(src, out, seen);
      return;
    }
  };

  for (const auto& s : sources) add_source(s);

  if (include_default_dirs) {
    for (const auto& d : default_tcd_dirs()) add_source(d);
  }

  std::sort(out.begin(), out.end(),
            [](const fs::path& a, const fs::path& b) { return a.string() < b.string(); });
  return out;
}

// ------------------------
// Fingerprint of tcd set
// ------------------------

uint64_t fnv1a64(uint64_t h, const void* data, size_t n) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < n; ++i) {
    h ^= (uint64_t)p[i];
    h *= 1099511628211ull;
  }
  return h;
}

std::string hex_u64(uint64_t v) {
  std::ostringstream oss;
  oss << std::hex << std::setw(16) << std::setfill('0') << v;
  return oss.str();
}

std::string tcd_fingerprint(const std::vector<fs::path>& tcd_files) {
  uint64_t h = 1469598103934665603ull;
  std::error_code ec;

  for (const auto& p : tcd_files) {
    const std::string s = norm_abs(p).string();
    h = fnv1a64(h, s.data(), s.size());

    auto sz = fs::file_size(p, ec);
    if (!ec) h = fnv1a64(h, &sz, sizeof(sz));
    ec.clear();

    auto ft = fs::last_write_time(p, ec);
    if (!ec) {
      auto cnt = ft.time_since_epoch().count();
      h = fnv1a64(h, &cnt, sizeof(cnt));
    }
    ec.clear();
  }
  return hex_u64(h);
}

int64_t now_epoch_s() {
  using namespace std::chrono;
  return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

// ------------------------
// Cache file (key=value)
// ------------------------

bool is_safe_val_char(unsigned char c) {
  if (std::isalnum(c)) return true;
  switch (c) {
    case '-': case '_': case '.': case '/': case '\\': case ':':
    case ' ': case ',': case '(': case ')':
      return true;
    default:
      return false;
  }
}

std::string pct_encode(const std::string& s) {
  static const char* hex = "0123456789ABCDEF";
  std::string out;
  out.reserve(s.size());
  for (unsigned char c : s) {
    if (c == '%' || !is_safe_val_char(c) || c == '\n' || c == '\r') {
      out.push_back('%');
      out.push_back(hex[(c >> 4) & 0xF]);
      out.push_back(hex[c & 0xF]);
    } else {
      out.push_back((char)c);
    }
  }
  return out;
}

int hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
  if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
  return -1;
}

std::string pct_decode(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size();) {
    if (s[i] == '%' && i + 2 < s.size()) {
      int a = hexval(s[i + 1]);
      int b = hexval(s[i + 2]);
      if (a >= 0 && b >= 0) {
        out.push_back((char)((a << 4) | b));
        i += 3;
        continue;
      }
    }
    out.push_back(s[i++]);
  }
  return out;
}

fs::path default_cache_path() {
  if (auto env = getenv_str("XTIDE_NEAREST_CACHE")) return fs::path(*env);

#ifdef _WIN32
  if (auto lad = getenv_str("LOCALAPPDATA")) {
    return fs::path(*lad) / "xtide-nearest" / "nearest.cache";
  }
  if (auto home = getenv_str("USERPROFILE")) {
    return fs::path(*home) / "AppData" / "Local" / "xtide-nearest" / "nearest.cache";
  }
  return fs::path("nearest.cache");
#else
  if (auto xdg = getenv_str("XDG_CACHE_HOME")) {
    return fs::path(*xdg) / "xtide-nearest" / "nearest.cache";
  }
  if (auto home = getenv_str("HOME")) {
    return fs::path(*home) / ".cache" / "xtide-nearest" / "nearest.cache";
  }
  return fs::path("/tmp") / "xtide-nearest-nearest.cache";
#endif
}

struct CacheRecord {
  int version = 1;

  int64_t epoch_s = 0;   // last successful nearest computation time
  double lat = 0.0;
  double lon = 0.0;

  std::string fingerprint;

  bool has_tide = false;
  Station tide;

  bool has_current = false;
  Station current;
};

bool read_kv_cache(const fs::path& p, std::unordered_map<std::string, std::string>& kv) {
  std::ifstream in(p);
  if (!in) return false;

  std::string line;
  while (std::getline(in, line)) {
    line = trim_copy(line);
    if (line.empty()) continue;
    if (line[0] == '#') continue;
    auto pos = line.find('=');
    if (pos == std::string::npos) continue;
    std::string k = trim_copy(line.substr(0, pos));
    std::string v = trim_copy(line.substr(pos + 1));
    kv[k] = pct_decode(v);
  }
  return true;
}

void write_kv_cache(const fs::path& p, const std::unordered_map<std::string, std::string>& kv) {
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);

  // Write via temp file then rename (best-effort atomic).
  fs::path tmp = p;
  tmp += ".tmp";

  std::ofstream out(tmp, std::ios::trunc);
  if (!out) throw std::runtime_error("Failed to open cache for write: " + p.string());

  out << "# xtide-nearest cache (key=value, percent-encoded)\n";

  std::vector<std::string> keys;
  keys.reserve(kv.size());
  for (const auto& it : kv) keys.push_back(it.first);
  std::sort(keys.begin(), keys.end());

  for (const auto& k : keys) {
    auto it = kv.find(k);
    if (it == kv.end()) continue;
    out << k << "=" << pct_encode(it->second) << "\n";
  }
  out.close();

  fs::rename(tmp, p, ec);
  if (ec) {
    // Fallback.
    ec.clear();
    fs::remove(p, ec);
    ec.clear();
    fs::rename(tmp, p, ec);
  }
}

std::string s_from_double(double x) {
  std::ostringstream oss;
  oss << std::setprecision(17) << x;
  return oss.str();
}

double d_from_str(const std::string& s, double def = 0.0) {
  try { return std::stod(s); } catch (...) { return def; }
}

int64_t i64_from_str(const std::string& s, int64_t def = 0) {
  try { return std::stoll(s); } catch (...) { return def; }
}

int i_from_str(const std::string& s, int def = 0) {
  try { return std::stoi(s); } catch (...) { return def; }
}

void kv_set_station(std::unordered_map<std::string, std::string>& kv, const std::string& prefix, const Station& st) {
  kv[prefix + "_name"] = st.name;
  kv[prefix + "_lat"] = s_from_double(st.lat_deg);
  kv[prefix + "_lon"] = s_from_double(st.lon_deg);
  kv[prefix + "_units"] = st.units;
  kv[prefix + "_min_dir"] = std::to_string(st.min_dir_deg);
  kv[prefix + "_max_dir"] = std::to_string(st.max_dir_deg);
  kv[prefix + "_tcd"] = norm_abs(st.tcd_path).string();
}

bool kv_get_station(const std::unordered_map<std::string, std::string>& kv, const std::string& prefix, Station& st, Kind k) {
  auto get = [&](const std::string& key) -> std::optional<std::string> {
    auto it = kv.find(key);
    if (it == kv.end()) return std::nullopt;
    return it->second;
  };

  auto name = get(prefix + "_name");
  if (!name || name->empty()) return false;

  st.kind = k;
  st.name = *name;
  st.lat_deg = d_from_str(get(prefix + "_lat").value_or("0"));
  st.lon_deg = d_from_str(get(prefix + "_lon").value_or("0"));
  st.units = get(prefix + "_units").value_or("");
  st.min_dir_deg = i_from_str(get(prefix + "_min_dir").value_or("361"), 361);
  st.max_dir_deg = i_from_str(get(prefix + "_max_dir").value_or("361"), 361);
  st.tcd_path = fs::path(get(prefix + "_tcd").value_or(""));
  return true;
}

bool load_cache(const fs::path& cache_path, CacheRecord& rec) {
  std::unordered_map<std::string, std::string> kv;
  if (!read_kv_cache(cache_path, kv)) return false;

  auto itv = kv.find("version");
  rec.version = (itv == kv.end()) ? 1 : i_from_str(itv->second, 1);

  auto ite = kv.find("epoch_s");
  rec.epoch_s = (ite == kv.end()) ? 0 : i64_from_str(ite->second, 0);

  auto itlat = kv.find("lat");
  rec.lat = (itlat == kv.end()) ? 0.0 : d_from_str(itlat->second, 0.0);

  auto itlon = kv.find("lon");
  rec.lon = (itlon == kv.end()) ? 0.0 : d_from_str(itlon->second, 0.0);

  auto itf = kv.find("fingerprint");
  rec.fingerprint = (itf == kv.end()) ? "" : itf->second;

  rec.has_tide = (kv.find("has_tide") != kv.end() && kv.at("has_tide") == "1");
  rec.has_current = (kv.find("has_current") != kv.end() && kv.at("has_current") == "1");

  if (rec.has_tide) rec.has_tide = kv_get_station(kv, "tide", rec.tide, Kind::Tide);
  if (rec.has_current) rec.has_current = kv_get_station(kv, "current", rec.current, Kind::Current);

  return true;
}

void save_cache(const fs::path& cache_path, const CacheRecord& rec) {
  std::unordered_map<std::string, std::string> kv;
  kv["version"] = std::to_string(rec.version);
  kv["epoch_s"] = std::to_string(rec.epoch_s);
  kv["lat"] = s_from_double(rec.lat);
  kv["lon"] = s_from_double(rec.lon);
  kv["fingerprint"] = rec.fingerprint;

  kv["has_tide"] = rec.has_tide ? "1" : "0";
  kv["has_current"] = rec.has_current ? "1" : "0";

  if (rec.has_tide) kv_set_station(kv, "tide", rec.tide);
  if (rec.has_current) kv_set_station(kv, "current", rec.current);

  write_kv_cache(cache_path, kv);
}

bool cache_is_usable(const CacheRecord& rec,
                     const std::string& fingerprint,
                     double cur_lat, double cur_lon) {
  if (rec.version != 1) return false;
  if (rec.epoch_s <= 0) return false;
  if (rec.fingerprint != fingerprint) return false;

  const int64_t now = now_epoch_s();
  const int64_t dt = now - rec.epoch_s;
  if (dt < 0 || dt > kCacheTtlSec) return false;

  const double dt_hr = (double)dt / 3600.0;
  const double max_km = std::max(kCacheMinRadiusKm, kCacheMaxSpeedKt * 1.852 * dt_hr);
  const double moved = haversine_km(cur_lat, cur_lon, rec.lat, rec.lon);
  return moved <= max_km;
}

// ------------------------
// Station load + nearest
// ------------------------

std::vector<Station> load_stations_from_one_tcd(const fs::path& tcd_path) {
  TideDbGuard db(tcd_path);

  const DB_HEADER_PUBLIC header = get_tide_db_header();

  std::vector<Station> stations;
  stations.reserve(static_cast<size_t>(header.number_of_records));

  for (NV_U_INT32 i = 0; i < header.number_of_records; ++i) {
    TIDE_RECORD rec{};
    const NV_INT32 got = read_tide_record(static_cast<NV_INT32>(i), &rec);
    if (got < 0) continue;

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
    st.tcd_path = norm_abs(tcd_path);

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

// XTide expects HFILE_PATH to be a path list of *directories* that contain .tcd files.
// We also put the chosen station's own .tcd directory FIRST (important if names collide).
std::string join_hfile_path(const std::vector<fs::path>& tcd_files, const fs::path& prefer_dir) {
#ifdef _WIN32
  const char sep = ';';
#else
  const char sep = ':';
#endif

  std::vector<std::string> dirs;
  dirs.reserve(tcd_files.size() + 1);

  auto norm_dir = [&](fs::path p) -> fs::path {
    if (p.has_extension() && is_tcd_file(p)) p = p.parent_path();
    if (p.empty()) p = fs::path(".");
    return norm_abs(p);
  };

  auto push_unique = [&](const fs::path& d) {
    std::string ds = d.string();
#ifdef _WIN32
    std::string key = to_lower_copy(ds);
#else
    std::string key = ds;
#endif
    for (const auto& existing : dirs) {
#ifdef _WIN32
      if (to_lower_copy(existing) == key) return;
#else
      if (existing == key) return;
#endif
    }
    dirs.push_back(std::move(ds));
  };

  if (!prefer_dir.empty()) push_unique(norm_dir(prefer_dir));

  for (const auto& p : tcd_files) {
    push_unique(norm_dir(p));
  }

  std::ostringstream oss;
  for (size_t i = 0; i < dirs.size(); ++i) {
    if (i) oss << sep;
    oss << dirs[i];
  }
  return oss.str();
}

#ifdef _WIN32

// UTF-8 -> UTF-16
std::wstring widen_utf8(const std::string& s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  if (n <= 0) throw std::runtime_error("MultiByteToWideChar failed");
  std::wstring w(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  return w;
}

std::string win_errstr(DWORD err) {
  LPWSTR buf = nullptr;
  DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
  DWORD n = FormatMessageW(flags, nullptr, err, 0, (LPWSTR)&buf, 0, nullptr);
  std::string out;
  if (n && buf) {
    int m = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
    if (m > 0) {
      out.resize(static_cast<size_t>(m - 1));
      WideCharToMultiByte(CP_UTF8, 0, buf, -1, out.data(), m, nullptr, nullptr);
    }
  }
  if (buf) LocalFree(buf);
  if (out.empty()) out = "Windows error " + std::to_string(err);
  return trim_copy(out);
}

// Quote argv element for Windows CreateProcess command-line parsing rules.
std::wstring quote_arg_win(std::wstring_view s) {
  const bool need_quotes =
      s.empty() || s.find_first_of(L" \t\n\v\"") != std::wstring_view::npos;
  if (!need_quotes) return std::wstring(s);

  std::wstring out;
  out.push_back(L'"');

  size_t backslashes = 0;
  for (wchar_t ch : s) {
    if (ch == L'\\') { ++backslashes; continue; }
    if (ch == L'"') {
      out.append(backslashes * 2 + 1, L'\\');
      out.push_back(L'"');
      backslashes = 0;
      continue;
    }
    if (backslashes) out.append(backslashes, L'\\');
    backslashes = 0;
    out.push_back(ch);
  }
  if (backslashes) out.append(backslashes * 2, L'\\');
  out.push_back(L'"');
  return out;
}

std::wstring build_cmdline_win(const std::wstring& exe, const std::vector<std::wstring>& args) {
  std::wstring cmd = quote_arg_win(exe);
  for (const auto& a : args) {
    cmd.push_back(L' ');
    cmd += quote_arg_win(a);
  }
  return cmd;
}

// Run tide.exe without cmd.exe parsing. Stream stdout to this process stdout.
int run_tide_win_stream(const std::string& tide_bin_utf8,
                        const std::vector<std::wstring>& args_w,
                        const std::string& debug_cmd_utf8) {
  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;

  HANDLE childStdoutRead = nullptr;
  HANDLE childStdoutWrite = nullptr;
  if (!CreatePipe(&childStdoutRead, &childStdoutWrite, &sa, 0)) {
    std::cerr << "ERROR: CreatePipe failed: " << win_errstr(GetLastError()) << "\n";
    std::cerr << "CMD: " << debug_cmd_utf8 << "\n";
    return 2;
  }
  SetHandleInformation(childStdoutRead, HANDLE_FLAG_INHERIT, 0);

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  si.hStdOutput = childStdoutWrite;
  si.hStdError = GetStdHandle(STD_ERROR_HANDLE);

  PROCESS_INFORMATION pi{};

  std::wstring exe_w = widen_utf8(tide_bin_utf8);
  std::wstring cmdline = build_cmdline_win(exe_w, args_w);
  std::vector<wchar_t> cmdBuf(cmdline.begin(), cmdline.end());
  cmdBuf.push_back(L'\0');

  BOOL ok = CreateProcessW(
      nullptr,
      cmdBuf.data(),
      nullptr,
      nullptr,
      TRUE,
      CREATE_NO_WINDOW,
      nullptr,
      nullptr,
      &si,
      &pi);

  CloseHandle(childStdoutWrite);

  if (!ok) {
    DWORD e = GetLastError();
    CloseHandle(childStdoutRead);
    std::cerr << "ERROR: CreateProcessW failed: " << win_errstr(e) << "\n";
    std::cerr << "CMD: " << debug_cmd_utf8 << "\n";
    return 2;
  }

  std::array<char, 8192> buf{};
  DWORD nread = 0;
  while (ReadFile(childStdoutRead, buf.data(), static_cast<DWORD>(buf.size()), &nread, nullptr) && nread > 0) {
    std::fwrite(buf.data(), 1, static_cast<size_t>(nread), stdout);
    std::fflush(stdout);
  }
  CloseHandle(childStdoutRead);

  WaitForSingleObject(pi.hProcess, INFINITE);

  DWORD exit_code = 0;
  GetExitCodeProcess(pi.hProcess, &exit_code);

  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);

  if (exit_code != 0) {
    std::cerr << "WARNING: tide exited with code " << static_cast<int>(exit_code) << "\n"
              << "CMD: " << debug_cmd_utf8 << "\n";
  }
  return (exit_code == 0) ? 0 : 3;
}

#endif // _WIN32

int run_tide_and_stream(const std::string& tide_bin,
                        const std::vector<fs::path>& tcd_files,
                        const Station& st,
                        const std::string& begin,
                        const std::string& end,
                        const std::string& step_hhmm,
                        const std::string& mode,
                        const std::string& format,
                        bool utc,
                        bool suppress_sunmoon,
                        bool omit_units,
                        bool emit_metadata,
                        double dist_km) {
  const auto old_hfile = getenv_str("HFILE_PATH");
  setenv_portable("HFILE_PATH", join_hfile_path(tcd_files, st.tcd_path));

#ifdef _WIN32
  std::vector<std::wstring> args_w;
  args_w.emplace_back(L"-l");  args_w.emplace_back(widen_utf8(st.name));
  args_w.emplace_back(L"-b");  args_w.emplace_back(widen_utf8(begin));
  args_w.emplace_back(L"-e");  args_w.emplace_back(widen_utf8(end));
  args_w.emplace_back(L"-m");  args_w.emplace_back(widen_utf8(mode));
  args_w.emplace_back(L"-f");  args_w.emplace_back(widen_utf8(format));
  args_w.emplace_back(L"-s");  args_w.emplace_back(widen_utf8(step_hhmm));
  args_w.emplace_back(L"-z");  args_w.emplace_back(utc ? L"y" : L"n");

  if (suppress_sunmoon) { args_w.emplace_back(L"-em"); args_w.emplace_back(L"pSsMm"); }
  if (omit_units) { args_w.emplace_back(L"-ou"); args_w.emplace_back(L"y"); }

  std::ostringstream dbg;
  dbg << shell_quote_double(tide_bin)
      << " -l " << shell_quote_double(st.name)
      << " -b " << shell_quote_double(begin)
      << " -e " << shell_quote_double(end)
      << " -m " << shell_quote_double(mode)
      << " -f " << shell_quote_double(format)
      << " -s " << shell_quote_double(step_hhmm)
      << " -z " << (utc ? "y" : "n");
  if (suppress_sunmoon) dbg << " -em pSsMm";
  if (omit_units) dbg << " -ou y";
  const std::string cmd_str = dbg.str();

  if (emit_metadata) {
    std::cout << "# kind=" << kind_str(st.kind)
              << " station=" << st.name
              << " distance_km=" << std::fixed << std::setprecision(3) << dist_km
              << " begin=" << begin
              << " end=" << end
              << " step=" << step_hhmm
              << " utc=" << (utc ? "true" : "false")
              << "\n";
  }

  const int rc = run_tide_win_stream(tide_bin, args_w, cmd_str);

  if (old_hfile) setenv_portable("HFILE_PATH", *old_hfile);
  else unsetenv_portable("HFILE_PATH");

  return rc;

#else
  std::ostringstream cmd;
  cmd << shell_quote_double(tide_bin)
      << " -l " << shell_quote_double(st.name)
      << " -b " << shell_quote_double(begin)
      << " -e " << shell_quote_double(end)
      << " -m " << shell_quote_double(mode)
      << " -f " << shell_quote_double(format)
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
    std::cout << "# kind=" << kind_str(st.kind)
              << " station=" << st.name
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

  const int status = pclose_portable(pipe);
  int exit_code = status;
  if (WIFEXITED(status)) exit_code = WEXITSTATUS(status);

  if (old_hfile) setenv_portable("HFILE_PATH", *old_hfile);
  else unsetenv_portable("HFILE_PATH");

  if (exit_code != 0) {
    std::cerr << "WARNING: tide exited with code " << exit_code << "\n"
              << "CMD: " << cmd_str << "\n";
  }
  return (exit_code == 0) ? 0 : 3;
#endif
}

// Compute best tide+current stations once and write cache.
void update_cache_from_stations(const fs::path& cache_path,
                                const std::string& fingerprint,
                                const std::vector<Station>& stations,
                                double lat, double lon) {
  CacheRecord rec;
  rec.epoch_s = now_epoch_s();
  rec.lat = lat;
  rec.lon = lon;
  rec.fingerprint = fingerprint;

  auto best_t = nearest(stations, lat, lon, Kind::Tide, 1);
  if (!best_t.empty()) {
    rec.has_tide = true;
    rec.tide = best_t.front().st;
  }
  auto best_c = nearest(stations, lat, lon, Kind::Current, 1);
  if (!best_c.empty()) {
    rec.has_current = true;
    rec.current = best_c.front().st;
  }

  if (rec.has_tide || rec.has_current) {
    try {
      save_cache(cache_path, rec);
    } catch (const std::exception& e) {
      std::cerr << "WARNING: failed to write cache: " << e.what() << "\n";
    }
  }
}

} // namespace

int main(int argc, char** argv) {
  CLI::App app{"Find nearest XTide stations (tides + currents) and optionally stream predictions via tide(1)."};
  app.require_subcommand(1);

  double lat = 0.0, lon = 0.0;

  // --tcd can be omitted if config/default paths find data.
  std::vector<std::string> tcd_in;
  std::string tcd_config_path;
  bool no_default_paths = false;

  std::string cache_file;
  bool no_cache = false;

  app.add_option("--lat", lat, "Latitude (deg)")->required();
  app.add_option("--lon", lon, "Longitude (deg)")->required();

  app.add_option("--tcd", tcd_in,
                 "TCD source (repeatable): a .tcd file OR a directory (searched up to 2 levels)");

  app.add_option("--tcd-config", tcd_config_path,
                 "Config file listing .tcd files and/or directories to search (one per line, # comments). "
                 "If omitted, uses $XTIDE_NEAREST_CONFIG or platform default.")->default_val("");

  app.add_flag("--no-default-tcd-paths", no_default_paths,
               "Disable built-in default TCD search dirs (e.g. /usr/share/tcdata, /usr/share/opencpn/tcdata)");

  app.add_option("--cache-file", cache_file,
                 "Nearest-station cache file (default: $XTIDE_NEAREST_CACHE or OS cache dir)")->default_val("");
  app.add_flag("--no-cache", no_cache, "Disable nearest-station cache");

  auto* cmd_nearest = app.add_subcommand("nearest", "List nearest stations (tide/current) and distances.");
  bool want_tide = true;
  bool want_current = true;
  size_t top_n = 5;
  cmd_nearest->add_flag("--tide,!--no-tide", want_tide, "Include tide stations (default on)")->default_val(true);
  cmd_nearest->add_flag("--current,!--no-current", want_current, "Include current stations (default on)")->default_val(true);
  cmd_nearest->add_option("--top", top_n, "How many results per kind")->default_val(5);

  auto* cmd_predict = app.add_subcommand("predict", "Find nearest station of a kind and stream predictions to stdout.");
  std::string kind_s = "current";
  std::string begin;
  std::string end;
  std::string step = "00:10";
  std::string tide_bin = "tide";
  std::string mode = "r";
  std::string format = "c";
  bool utc = false;
  bool include_sunmoon = false;      // default: suppress for clean CSV
  bool omit_units = false;
  bool emit_metadata = true;

  cmd_predict->add_option("--kind", kind_s, "tide | current")->default_val("current");
  cmd_predict->add_option("--begin", begin, "Begin time: \"YYYY-MM-DD HH:MM\"")->required();
  cmd_predict->add_option("--end", end, "End time: \"YYYY-MM-DD HH:MM\"")->required();
  cmd_predict->add_option("--step", step, "Step interval for raw mode: \"HH:MM\"")->default_val("00:10");
  cmd_predict->add_option("--tide-bin", tide_bin, "Path to tide(1) executable")->default_val("tide");
  cmd_predict->add_option("--mode", mode, "tide -m mode (default r)")->default_val("r");
  cmd_predict->add_option("--format", format, "tide -f format (default c)")->default_val("c");
  cmd_predict->add_flag("--utc", utc, "Coerce timestamps to UTC (tide -z y)")->default_val(false);

  cmd_predict->add_flag("--sunmoon,!--no-sunmoon", include_sunmoon,
                        "Include sun/moon events (default: suppressed)")->default_val(false);

  cmd_predict->add_flag("--omit-units", omit_units,
                        "Omit unit suffix in numeric fields (tide -ou y)")->default_val(false);

  cmd_predict->add_flag("--meta,!--no-meta", emit_metadata,
                        "Emit metadata header line (default on)")->default_val(true);

  CLI11_PARSE(app, argc, argv);

  auto parse_kind = [&](const std::string& s) -> Kind {
    const auto ls = to_lower_copy(s);
    if (ls == "tide") return Kind::Tide;
    if (ls == "current") return Kind::Current;
    throw std::runtime_error("Invalid --kind. Use 'tide' or 'current'.");
  };

  // Build sources list: config + CLI + defaults.
  std::vector<fs::path> sources;

  std::optional<fs::path> cfg_path;
  if (!tcd_config_path.empty()) cfg_path = fs::path(tcd_config_path);
  else cfg_path = default_config_path_guess();

  if (cfg_path) {
    if (auto cfg = read_tcd_config(*cfg_path)) {
      for (const auto& p : cfg->sources) sources.push_back(p);
    }
  }

  for (const auto& s : tcd_in) sources.emplace_back(fs::path(s));

  const bool include_defaults = !no_default_paths;
  const auto tcd_files = discover_tcd_files(sources, include_defaults);

  if (tcd_files.empty()) {
    std::cerr << "ERROR: No .tcd files found.\n"
              << "Provide --tcd (file/dir) and/or --tcd-config, or install tcdata under a default search dir.\n";
    return 2;
  }

  const std::string fp = tcd_fingerprint(tcd_files);

  const fs::path cache_path = cache_file.empty() ? default_cache_path() : fs::path(cache_file);
  CacheRecord cache;
  const bool cache_loaded = (!no_cache) ? load_cache(cache_path, cache) : false;
  const bool cache_ok = (!no_cache && cache_loaded && cache_is_usable(cache, fp, lat, lon));

  if (*cmd_nearest) {
    if (!want_tide && !want_current) {
      std::cerr << "Nothing to do: both --no-tide and --no-current set.\n";
      return 2;
    }

    // Fast path: if top_n == 1 and cache is valid, emit from cache without scanning.
    if (top_n == 1 && cache_ok) {
      auto print_one = [&](Kind k, const Station& st) {
        const double dk = haversine_km(lat, lon, st.lat_deg, st.lon_deg);
        std::cout << "kind=" << kind_str(k) << " results=1\n";
        std::cout << "distance_km,name,lat,lon,units,min_dir_deg,max_dir_deg,tcd\n";
        std::cout << std::fixed << std::setprecision(3)
                  << dk << ","
                  << shell_quote_double(st.name) << ","
                  << std::setprecision(6) << st.lat_deg << ","
                  << std::setprecision(6) << st.lon_deg << ","
                  << shell_quote_double(st.units) << ","
                  << st.min_dir_deg << ","
                  << st.max_dir_deg << ","
                  << shell_quote_double(st.tcd_path.string())
                  << "\n\n";
      };

      bool served_all = true;
      if (want_tide) {
        if (cache.has_tide) print_one(Kind::Tide, cache.tide);
        else served_all = false;
      }
      if (want_current) {
        if (cache.has_current) print_one(Kind::Current, cache.current);
        else served_all = false;
      }
      if (served_all) return 0;
      // else fall through to full scan for missing entries
    }

    std::vector<Station> stations;
    try {
      stations = load_stations(tcd_files);
    } catch (const std::exception& e) {
      std::cerr << "ERROR: failed to load stations: " << e.what() << "\n";
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

    if (!no_cache) update_cache_from_stations(cache_path, fp, stations, lat, lon);
    return 0;
  }

  if (*cmd_predict) {
    const Kind k = parse_kind(kind_s);

    // Use cache to avoid scanning if valid.
    if (cache_ok) {
      const Station* st = nullptr;
      if (k == Kind::Tide && cache.has_tide) st = &cache.tide;
      if (k == Kind::Current && cache.has_current) st = &cache.current;

      if (st) {
        const double dk = haversine_km(lat, lon, st->lat_deg, st->lon_deg);
        return run_tide_and_stream(
          tide_bin, tcd_files, *st, begin, end, step, mode, format, utc,
          /*suppress_sunmoon=*/!include_sunmoon,
          omit_units, emit_metadata, dk
        );
      }
    }

    // Cache miss: scan all stations.
    std::vector<Station> stations;
    try {
      stations = load_stations(tcd_files);
    } catch (const std::exception& e) {
      std::cerr << "ERROR: failed to load stations: " << e.what() << "\n";
      return 2;
    }

    const auto cand = nearest(stations, lat, lon, k, 1);
    if (cand.empty()) {
      std::cerr << "No stations found for kind=" << kind_str(k) << "\n";
      return 2;
    }

    if (!no_cache) update_cache_from_stations(cache_path, fp, stations, lat, lon);

    const auto& best = cand.front();
    return run_tide_and_stream(
      tide_bin,
      tcd_files,
      best.st,
      begin,
      end,
      step,
      mode,
      format,
      utc,
      /*suppress_sunmoon=*/!include_sunmoon,
      omit_units,
      emit_metadata,
      best.distance_km
    );
  }

  return 0;
}
