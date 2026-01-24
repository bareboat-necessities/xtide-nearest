/*
  Copyright 2026, Mikhail Grushinskiy
*/
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace xtide_nearest {
namespace fs = std::filesystem;

// --------------------
// Types
// --------------------

enum class Kind { Tide, Current };
std::string_view KindStr(Kind k);

struct Station {
  std::string name;
  double lat_deg{};
  double lon_deg{};
  Kind kind{};
  std::string units;      // e.g. "meters", "feet", "knots"
  int min_dir_deg{361};   // 0..360 or 361 = none
  int max_dir_deg{361};   // 0..360 or 361 = none
  fs::path tcd_path;      // absolute path to the .tcd file it came from
};

struct Candidate {
  Station st;
  double distance_km{};
};

// Config file contents
struct TcdConfig {
  std::vector<fs::path> sources;  // files or dirs
};

// Cache record
struct CacheRecord {
  int version = 1;

  int64_t epoch_s = 0;
  double lat = 0.0;
  double lon = 0.0;

  std::string fingerprint;

  bool has_tide = false;
  Station tide;

  bool has_current = false;
  Station current;
};

// --------------------
// Core utilities / resolving dataset
// --------------------

double HaversineKm(double lat1, double lon1, double lat2, double lon2);

std::optional<fs::path> DefaultConfigPathGuess();
std::optional<TcdConfig> ReadTcdConfig(const fs::path& cfg_path);

// Default search dirs (platform-dependent)
std::vector<fs::path> DefaultTcdDirs();

// Discover .tcd files from sources (dirs scanned up to depth=2). Optionally include defaults.
std::vector<fs::path> DiscoverTcdFiles(const std::vector<fs::path>& sources, bool include_default_dirs);

// Dataset fingerprint (paths + size + mtime)
std::string TcdFingerprint(const std::vector<fs::path>& tcd_files);

// Nearest selection
std::vector<Candidate> Nearest(const std::vector<Station>& stations,
                               double lat, double lon,
                               std::optional<Kind> kind_filter,
                               size_t top_n);

// --------------------
// Cache
// --------------------

fs::path DefaultCachePath();

bool LoadCache(const fs::path& cache_path, CacheRecord& rec);
void SaveCache(const fs::path& cache_path, const CacheRecord& rec);

bool CacheIsUsable(const CacheRecord& rec,
                   const std::string& fingerprint,
                   double cur_lat, double cur_lon);

// Compute best tide+current stations and update cache.
void UpdateCacheFromStations(const fs::path& cache_path,
                             const std::string& fingerprint,
                             const std::vector<Station>& stations,
                             double lat, double lon);

// Cache policy constants (exposed for JSON/meta)
int64_t CacheTtlSec();
double  CacheMaxSpeedKt();
double  CacheMinRadiusKm();

// --------------------
// Stations (libtcd)
// --------------------

std::vector<Station> LoadStations(const std::vector<fs::path>& tcd_files);

// --------------------
// tide(1) runner
// --------------------

struct TideRunRequest {
  std::string tide_bin = "tide";
  std::vector<fs::path> tcd_files; // discovered .tcd files
  Station station;

  std::string begin;  // "YYYY-MM-DD HH:MM"
  std::string end;    // "YYYY-MM-DD HH:MM"
  std::string step = "00:10";
  std::string mode = "r";
  std::string format = "c";
  bool utc = false;

  bool include_sunmoon = false;
  bool omit_units = false;
};

struct TideRunResult {
  int rc = 0;                 // 0 ok, 2 spawn error, 3 nonzero exit
  std::string stdout_text;    // captured stdout (always filled if capture=true)
};

TideRunResult RunTideCapture(const TideRunRequest& req);

// --------------------
// Minimal JSON helpers (for CLI)
// --------------------

std::string JsonEscape(std::string_view s);

// Convenience: convert paths to UTF-8-ish string via path::string().
std::string PathString(const fs::path& p);

} // namespace xtide_nearest
