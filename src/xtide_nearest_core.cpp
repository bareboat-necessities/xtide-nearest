/*
  Copyright 2026, Mikhail Grushinskiy
*/
#include "xtide_nearest/xtide_nearest.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX 1
#endif
#include <windows.h>
#endif

namespace xtide_nearest {
namespace {

constexpr double kEarthRadiusKm = 6371.0088;
constexpr double kPi = 3.141592653589793238462643383279502884;

// Cache policy: keep nearest station selection for "sailboat-ish" movement.
constexpr int64_t kCacheTtlSec_ = 20 * 60;  // 20 min
constexpr double  kCacheMaxSpeedKt_ = 15.0;
constexpr double  kCacheMinRadiusKm_ = 0.5;

double deg2rad(double d) { return d * (kPi / 180.0); }

std::string trim_copy(std::string s) {
  auto is_ws = [](unsigned char c) { return std::isspace(c) != 0; };
  s.erase(s.begin(), std::find_if(s.begin(), s.end(),
                                  [&](char c) { return !is_ws((unsigned char)c); }));
  s.erase(std::find_if(s.rbegin(), s.rend(),
                       [&](char c) { return !is_ws((unsigned char)c); }).base(),
          s.end());
  return s;
}

std::string to_lower_copy(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower((unsigned char)c));
  return s;
}

bool ieq(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
  }
  return true;
}

bool contains_icase(std::string_view hay, std::string_view needle) {
  std::string H(hay), N(needle);
  H = to_lower_copy(std::move(H));
  N = to_lower_copy(std::move(N));
  return H.find(N) != std::string::npos;
}

bool is_tcd_file(const fs::path& p) {
  if (!p.has_extension()) return false;
  return ieq(p.extension().string(), ".tcd");
}

std::optional<std::string> getenv_str(const char* name) {
  if (const char* v = std::getenv(name)) return std::string(v);
  return std::nullopt;
}

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

// Windows: paths are case-insensitive for dedupe/fingerprint stability.
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

  scan_files_in_dir(dir);  // depth 0

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
      scan_files_in_dir(e2.path()); // depth 2
    }
  }
}

// FNV-1a 64
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

int64_t now_epoch_s() {
  using namespace std::chrono;
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// percent encoding for cache kv file
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

void write_kv_cache_atomic(const fs::path& p, const std::unordered_map<std::string, std::string>& kv) {
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);

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
  st.lat_deg = d_from_str(get(prefix + "_lat").value_or("0"), 0.0);
  st.lon_deg = d_from_str(get(prefix + "_lon").value_or("0"), 0.0);
  st.units = get(prefix + "_units").value_or("");
  st.min_dir_deg = i_from_str(get(prefix + "_min_dir").value_or("361"), 361);
  st.max_dir_deg = i_from_str(get(prefix + "_max_dir").value_or("361"), 361);
  st.tcd_path = fs::path(get(prefix + "_tcd").value_or(""));
  return true;
}

} // namespace

std::string_view KindStr(Kind k) {
  return (k == Kind::Current) ? "current" : "tide";
}

double HaversineKm(double lat1, double lon1, double lat2, double lon2) {
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

std::vector<fs::path> DefaultTcdDirs() {
  std::vector<fs::path> dirs = {
      "/etc/tcdata",
      "/usr/share/tcdata",
      "/usr/share/opencpn/tcdata",
      "/usr/local/share/opencpn/tcdata",
  };

#ifdef _WIN32
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

std::optional<fs::path> DefaultConfigPathGuess() {
  if (auto env = getenv_str("XTIDE_NEAREST_CONFIG")) return fs::path(*env);

#ifdef _WIN32
  if (auto appdata = getenv_str("APPDATA")) {
    return fs::path(*appdata) / "xtide-nearest" / "tcd.conf";
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

std::optional<TcdConfig> ReadTcdConfig(const fs::path& cfg_path) {
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

std::vector<fs::path> DiscoverTcdFiles(const std::vector<fs::path>& sources, bool include_default_dirs) {
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
  if (include_default_dirs) for (const auto& d : DefaultTcdDirs()) add_source(d);

  std::sort(out.begin(), out.end(), [](const fs::path& a, const fs::path& b) {
    return a.string() < b.string();
  });
  return out;
}

std::string TcdFingerprint(const std::vector<fs::path>& tcd_files) {
  uint64_t h = 1469598103934665603ull;
  std::error_code ec;

  for (const auto& p : tcd_files) {
    // Use norm_key for stability across Windows path case differences.
    const std::string s = norm_key(p);
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

std::vector<Candidate> Nearest(const std::vector<Station>& stations,
                               double lat, double lon,
                               std::optional<Kind> kind_filter,
                               size_t top_n) {
  std::vector<Candidate> out;
  out.reserve(stations.size());

  for (const auto& st : stations) {
    if (kind_filter && st.kind != *kind_filter) continue;
    Candidate c;
    c.st = st;
    c.distance_km = HaversineKm(lat, lon, st.lat_deg, st.lon_deg);
    out.push_back(std::move(c));
  }

  std::sort(out.begin(), out.end(), [](const Candidate& a, const Candidate& b) {
    return a.distance_km < b.distance_km;
  });

  if (out.size() > top_n) out.resize(top_n);
  return out;
}

fs::path DefaultCachePath() {
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
  // fixed: no double "nearest-nearest"
  return fs::path("/tmp") / "xtide-nearest.cache";
#endif
}

bool LoadCache(const fs::path& cache_path, CacheRecord& rec) {
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

void SaveCache(const fs::path& cache_path, const CacheRecord& rec) {
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

  write_kv_cache_atomic(cache_path, kv);
}

bool CacheIsUsable(const CacheRecord& rec,
                   const std::string& fingerprint,
                   double cur_lat, double cur_lon) {
  if (rec.version != 1) return false;
  if (rec.epoch_s <= 0) return false;
  if (rec.fingerprint != fingerprint) return false;

  const int64_t now = now_epoch_s();
  const int64_t dt = now - rec.epoch_s;
  if (dt < 0 || dt > kCacheTtlSec_) return false;

  const double dt_hr = (double)dt / 3600.0;
  const double max_km = std::max(kCacheMinRadiusKm_, kCacheMaxSpeedKt_ * 1.852 * dt_hr);
  const double moved = HaversineKm(cur_lat, cur_lon, rec.lat, rec.lon);
  return moved <= max_km;
}

void UpdateCacheFromStations(const fs::path& cache_path,
                             const std::string& fingerprint,
                             const std::vector<Station>& stations,
                             double lat, double lon) {
  CacheRecord rec;
  rec.epoch_s = now_epoch_s();
  rec.lat = lat;
  rec.lon = lon;
  rec.fingerprint = fingerprint;

  auto best_t = Nearest(stations, lat, lon, Kind::Tide, 1);
  if (!best_t.empty()) {
    rec.has_tide = true;
    rec.tide = best_t.front().st;
  }
  auto best_c = Nearest(stations, lat, lon, Kind::Current, 1);
  if (!best_c.empty()) {
    rec.has_current = true;
    rec.current = best_c.front().st;
  }

  if (rec.has_tide || rec.has_current) {
    try {
      SaveCache(cache_path, rec);
    } catch (...) {
      // library keeps this silent; CLI can log warnings when calling
    }
  }
}

int64_t CacheTtlSec() { return kCacheTtlSec_; }
double  CacheMaxSpeedKt() { return kCacheMaxSpeedKt_; }
double  CacheMinRadiusKm() { return kCacheMinRadiusKm_; }

std::string PathString(const fs::path& p) {
  // path::string() is fine for “best effort” CLI JSON.
  return p.string();
}

std::string JsonEscape(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (unsigned char c : s) {
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"':  out += "\\\""; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[7];
          std::snprintf(buf, sizeof(buf), "\\u%04x", (unsigned)c);
          out += buf;
        } else {
          out.push_back((char)c);
        }
    }
  }
  return out;
}

} // namespace xtide_nearest
