/*
  Copyright 2026, Mikhail Grushinskiy
*/
#include "xtide_nearest/xtide_nearest.h"

#include <CLI/CLI.hpp>

#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace xtide_nearest {
namespace {

// Proper CSV quoting (fixes previous invalid backslash quoting).
std::string csv_quote(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 2);
  out.push_back('"');
  for (char c : s) {
    if (c == '"') out.push_back('"'); // double it
    out.push_back(c);
  }
  out.push_back('"');
  return out;
}

Kind parse_kind(std::string s) {
  for (auto& c : s) c = (char)std::tolower((unsigned char)c);
  if (s == "tide") return Kind::Tide;
  if (s == "current") return Kind::Current;
  throw std::runtime_error("Invalid --kind. Use 'tide' or 'current'.");
}

// Detect --json early so parse errors can be returned as JSON too.
bool argv_has_flag(int argc, char** argv, std::string_view flag) {
  for (int i = 1; i < argc; ++i) {
    if (std::string_view(argv[i]) == flag) return true;
  }
  return false;
}

bool argv_has_help(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    std::string_view a(argv[i]);
    if (a == "-h" || a == "--help") return true;
  }
  return false;
}

void json_write_station(std::ostream& os, const Station& st) {
  os << "{"
     << "\"name\":\"" << JsonEscape(st.name) << "\","
     << "\"lat_deg\":" << std::setprecision(17) << st.lat_deg << ","
     << "\"lon_deg\":" << std::setprecision(17) << st.lon_deg << ","
     << "\"kind\":\"" << KindStr(st.kind) << "\","
     << "\"units\":\"" << JsonEscape(st.units) << "\","
     << "\"min_dir_deg\":" << st.min_dir_deg << ","
     << "\"max_dir_deg\":" << st.max_dir_deg << ","
     << "\"tcd_path\":\"" << JsonEscape(PathString(st.tcd_path)) << "\""
     << "}";
}

void json_write_candidate(std::ostream& os, const Candidate& c) {
  os << "{"
     << "\"distance_km\":" << std::fixed << std::setprecision(6) << c.distance_km << ","
     << "\"station\":";
  json_write_station(os, c.st);
  os << "}";
}

} // namespace
} // namespace xtide_nearest

int main(int argc, char** argv) {
  using namespace xtide_nearest;

  const bool json_requested_early = argv_has_flag(argc, argv, "--json");
  const bool help_requested = argv_has_help(argc, argv);

  CLI::App app{"Find nearest XTide stations (tides + currents) and optionally stream predictions via tide(1)."};
  app.require_subcommand(1);

  bool json = false;
  app.add_flag("--json", json, "Emit machine-readable JSON");

  double lat = 0.0, lon = 0.0;

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
  cmd_nearest->add_flag("--tide,!--no-tide", want_tide, "Include tide stations (default on)");
  cmd_nearest->add_flag("--current,!--no-current", want_current, "Include current stations (default on)");
  cmd_nearest->add_option("--top", top_n, "How many results per kind")->default_val(5);

  auto* cmd_predict = app.add_subcommand("predict", "Find nearest station of a kind and output predictions.");
  std::string kind_s = "current";
  std::string begin;
  std::string end;
  std::string step = "00:10";
  std::string tide_bin = "tide";
  std::string mode = "r";
  std::string format = "c";
  bool utc = false;
  bool include_sunmoon = false;
  bool omit_units = false;
  bool emit_metadata = true;

  cmd_predict->add_option("--kind", kind_s, "tide | current")->default_val("current");
  cmd_predict->add_option("--begin", begin, "Begin time: \"YYYY-MM-DD HH:MM\"")->required();
  cmd_predict->add_option("--end", end, "End time: \"YYYY-MM-DD HH:MM\"")->required();
  cmd_predict->add_option("--step", step, "Step interval for raw mode: \"HH:MM\"")->default_val("00:10");
  cmd_predict->add_option("--tide-bin", tide_bin, "Path to tide(1) executable")->default_val("tide");
  cmd_predict->add_option("--mode", mode, "tide -m mode (default r)")->default_val("r");
  cmd_predict->add_option("--format", format, "tide -f format (default c)")->default_val("c");
  cmd_predict->add_flag("--utc", utc, "Coerce timestamps to UTC (tide -z y)");
  cmd_predict->add_flag("--sunmoon,!--no-sunmoon", include_sunmoon,
                        "Include sun/moon events (default: suppressed)");
  cmd_predict->add_flag("--omit-units", omit_units, "Omit unit suffix in numeric fields (tide -ou y)");
  cmd_predict->add_flag("--meta,!--no-meta", emit_metadata,
                        "Emit metadata header line in non-JSON mode (default on)");

  try {
    app.parse(argc, argv);
  } catch (const CLI::ParseError& e) {
    if (json_requested_early && !help_requested) {
      std::cout << "{"
                << "\"ok\":false,"
                << "\"error\":\"" << JsonEscape(e.what()) << "\""
                << "}\n";
      return e.get_exit_code();
    }
    return app.exit(e);
  }

  // Build sources: config + CLI
  std::vector<fs::path> sources;

  std::optional<fs::path> cfg_path;
  if (!tcd_config_path.empty()) cfg_path = fs::path(tcd_config_path);
  else cfg_path = DefaultConfigPathGuess();

  std::optional<fs::path> cfg_used;
  if (cfg_path) {
    if (auto cfg = ReadTcdConfig(*cfg_path)) {
      cfg_used = *cfg_path;
      for (const auto& p : cfg->sources) sources.push_back(p);
    }
  }
  for (const auto& s : tcd_in) sources.emplace_back(fs::path(s));

  const bool include_defaults = !no_default_paths;
  const auto tcd_files = DiscoverTcdFiles(sources, include_defaults);

  if (tcd_files.empty()) {
    if (json) {
      std::cout << "{"
                << "\"ok\":false,"
                << "\"error\":\"No .tcd files found\","
                << "\"hint\":\"Provide --tcd (file/dir) and/or --tcd-config, or install tcdata under a default search dir.\""
                << "}\n";
      return 2;
    }
    std::cerr << "ERROR: No .tcd files found.\n"
              << "Provide --tcd (file/dir) and/or --tcd-config, or install tcdata under a default search dir.\n";
    return 2;
  }

  const std::string fp = TcdFingerprint(tcd_files);

  const fs::path cache_path = cache_file.empty() ? DefaultCachePath() : fs::path(cache_file);
  CacheRecord cache;
  const bool cache_loaded = (!no_cache) ? LoadCache(cache_path, cache) : false;
  const bool cache_ok = (!no_cache && cache_loaded && CacheIsUsable(cache, fp, lat, lon));

  if (*cmd_nearest) {
    if (!want_tide && !want_current) {
      if (json) {
        std::cout << "{"
                  << "\"ok\":false,"
                  << "\"error\":\"Nothing to do: both tide and current disabled\""
                  << "}\n";
        return 2;
      }
      std::cerr << "Nothing to do: both --no-tide and --no-current set.\n";
      return 2;
    }

    // Fast path for top=1: use cache if possible, else scan.
    bool need_scan = true;
    std::vector<Candidate> tide_out, cur_out;

    if (top_n == 1 && cache_ok) {
      need_scan = false;
      if (want_tide) {
        if (cache.has_tide) {
          Candidate c;
          c.st = cache.tide;
          c.distance_km = HaversineKm(lat, lon, c.st.lat_deg, c.st.lon_deg);
          tide_out = {c};
        } else {
          need_scan = true;
        }
      }
      if (want_current) {
        if (cache.has_current) {
          Candidate c;
          c.st = cache.current;
          c.distance_km = HaversineKm(lat, lon, c.st.lat_deg, c.st.lon_deg);
          cur_out = {c};
        } else {
          need_scan = true;
        }
      }
    }

    std::vector<Station> stations;
    if (need_scan) {
      try {
        stations = LoadStations(tcd_files);
      } catch (const std::exception& ex) {
        if (json) {
          std::cout << "{"
                    << "\"ok\":false,"
                    << "\"error\":\"" << JsonEscape(ex.what()) << "\""
                    << "}\n";
          return 2;
        }
        std::cerr << "ERROR: failed to load stations: " << ex.what() << "\n";
        return 2;
      }
      if (want_tide) tide_out = Nearest(stations, lat, lon, Kind::Tide, top_n);
      if (want_current) cur_out = Nearest(stations, lat, lon, Kind::Current, top_n);

      if (!no_cache) {
        try { UpdateCacheFromStations(cache_path, fp, stations, lat, lon); }
        catch (...) { /* CLI-only warning could be added */ }
      }
    }

    if (json) {
      std::ostringstream os;
      os << "{"
         << "\"ok\":true,"
         << "\"command\":\"nearest\","
         << "\"lat\":" << std::setprecision(17) << lat << ","
         << "\"lon\":" << std::setprecision(17) << lon << ","
         << "\"top\":" << top_n << ","
         << "\"want_tide\":" << (want_tide ? "true" : "false") << ","
         << "\"want_current\":" << (want_current ? "true" : "false") << ","
         << "\"tcd_config_used\":" << (cfg_used ? ("\"" + JsonEscape(PathString(*cfg_used)) + "\"") : "null") << ","
         << "\"include_default_tcd_paths\":" << (include_defaults ? "true" : "false") << ","
         << "\"fingerprint\":\"" << JsonEscape(fp) << "\","
         << "\"tcd_files\":[";
      for (size_t i = 0; i < tcd_files.size(); ++i) {
        if (i) os << ",";
        os << "\"" << JsonEscape(PathString(tcd_files[i])) << "\"";
      }
      os << "],"
         << "\"cache\":{"
         << "\"enabled\":" << (!no_cache ? "true" : "false") << ","
         << "\"path\":\"" << JsonEscape(PathString(cache_path)) << "\","
         << "\"loaded\":" << (cache_loaded ? "true" : "false") << ","
         << "\"hit\":" << (cache_ok ? "true" : "false") << ","
         << "\"ttl_sec\":" << CacheTtlSec() << ","
         << "\"max_speed_kt\":" << CacheMaxSpeedKt() << ","
         << "\"min_radius_km\":" << CacheMinRadiusKm()
         << "},"
         << "\"results\":{";

      os << "\"tide\":[";
      for (size_t i = 0; i < tide_out.size(); ++i) {
        if (i) os << ",";
        json_write_candidate(os, tide_out[i]);
      }
      os << "],";

      os << "\"current\":[";
      for (size_t i = 0; i < cur_out.size(); ++i) {
        if (i) os << ",";
        json_write_candidate(os, cur_out[i]);
      }
      os << "]";

      os << "}}";
      os << "}\n";
      std::cout << os.str();
      return 0;
    }

    // Human output (CSV blocks)
    auto print_block = [&](Kind k, const std::vector<Candidate>& cand) {
      std::cout << "kind=" << KindStr(k) << " results=" << cand.size() << "\n";
      std::cout << "distance_km,name,lat,lon,units,min_dir_deg,max_dir_deg,tcd\n";
      for (const auto& c : cand) {
        std::cout << std::fixed << std::setprecision(3)
                  << c.distance_km << ","
                  << csv_quote(c.st.name) << ","
                  << std::setprecision(6) << c.st.lat_deg << ","
                  << std::setprecision(6) << c.st.lon_deg << ","
                  << csv_quote(c.st.units) << ","
                  << c.st.min_dir_deg << ","
                  << c.st.max_dir_deg << ","
                  << csv_quote(PathString(c.st.tcd_path))
                  << "\n";
      }
      std::cout << "\n";
    };

    if (want_tide) print_block(Kind::Tide, tide_out);
    if (want_current) print_block(Kind::Current, cur_out);
    return 0;
  }

  if (*cmd_predict) {
    const Kind k = parse_kind(kind_s);

    // Use cache if possible.
    std::optional<Station> st;
    if (cache_ok) {
      if (k == Kind::Tide && cache.has_tide) st = cache.tide;
      if (k == Kind::Current && cache.has_current) st = cache.current;
    }

    double dist_km = 0.0;
    if (st) {
      dist_km = HaversineKm(lat, lon, st->lat_deg, st->lon_deg);
    } else {
      // Scan stations.
      std::vector<Station> stations;
      try {
        stations = LoadStations(tcd_files);
      } catch (const std::exception& ex) {
        if (json) {
          std::cout << "{"
                    << "\"ok\":false,"
                    << "\"error\":\"" << JsonEscape(ex.what()) << "\""
                    << "}\n";
          return 2;
        }
        std::cerr << "ERROR: failed to load stations: " << ex.what() << "\n";
        return 2;
      }

      auto cand = Nearest(stations, lat, lon, k, 1);
      if (cand.empty()) {
        if (json) {
          std::cout << "{"
                    << "\"ok\":false,"
                    << "\"error\":\"No stations found for requested kind\""
                    << "}\n";
          return 2;
        }
        std::cerr << "No stations found for kind=" << KindStr(k) << "\n";
        return 2;
      }

      st = cand.front().st;
      dist_km = cand.front().distance_km;

      if (!no_cache) {
        try { UpdateCacheFromStations(cache_path, fp, stations, lat, lon); }
        catch (...) {}
      }
    }

    TideRunRequest tr;
    tr.tide_bin = tide_bin;
    tr.tcd_files = tcd_files;
    tr.station = *st;
    tr.begin = begin;
    tr.end = end;
    tr.step = step;
    tr.mode = mode;
    tr.format = format;
    tr.utc = utc;
    tr.include_sunmoon = include_sunmoon;
    tr.omit_units = omit_units;

    TideRunResult rr = RunTideCapture(tr);

    if (json) {
      std::ostringstream os;
      os << "{"
         << "\"ok\":" << (rr.rc == 0 ? "true" : "false") << ","
         << "\"command\":\"predict\","
         << "\"lat\":" << std::setprecision(17) << lat << ","
         << "\"lon\":" << std::setprecision(17) << lon << ","
         << "\"kind\":\"" << KindStr(k) << "\","
         << "\"fingerprint\":\"" << JsonEscape(fp) << "\","
         << "\"cache\":{"
         << "\"enabled\":" << (!no_cache ? "true" : "false") << ","
         << "\"path\":\"" << JsonEscape(PathString(cache_path)) << "\","
         << "\"loaded\":" << (cache_loaded ? "true" : "false") << ","
         << "\"hit\":" << (cache_ok ? "true" : "false")
         << "},"
         << "\"station\":";
      json_write_station(os, *st);
      os << ","
         << "\"distance_km\":" << std::fixed << std::setprecision(6) << dist_km << ","
         << "\"tide\":{"
         << "\"bin\":\"" << JsonEscape(tide_bin) << "\","
         << "\"begin\":\"" << JsonEscape(begin) << "\","
         << "\"end\":\"" << JsonEscape(end) << "\","
         << "\"step\":\"" << JsonEscape(step) << "\","
         << "\"mode\":\"" << JsonEscape(mode) << "\","
         << "\"format\":\"" << JsonEscape(format) << "\","
         << "\"utc\":" << (utc ? "true" : "false") << ","
         << "\"include_sunmoon\":" << (include_sunmoon ? "true" : "false") << ","
         << "\"omit_units\":" << (omit_units ? "true" : "false")
         << "},"
         << "\"runner_rc\":" << rr.rc << ","
         << "\"output\":{"
         << "\"format\":\"tide_stdout\","
         << "\"text\":\"" << JsonEscape(rr.stdout_text) << "\""
         << "}"
         << "}\n";
      std::cout << os.str();
      return rr.rc == 0 ? 0 : rr.rc;
    }

    // Non-JSON: optional metadata header, then raw tide output
    if (emit_metadata) {
      std::cout << "# kind=" << KindStr(st->kind)
                << " station=" << st->name
                << " distance_km=" << std::fixed << std::setprecision(3) << dist_km
                << " begin=" << begin
                << " end=" << end
                << " step=" << step
                << " utc=" << (utc ? "true" : "false")
                << "\n";
    }
    std::cout << rr.stdout_text;
    return rr.rc == 0 ? 0 : rr.rc;
  }

  return 0;
}
