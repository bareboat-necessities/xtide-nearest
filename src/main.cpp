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

#ifndef _WIN32
#include <sys/wait.h>
#endif

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

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

// XTide expects HFILE_PATH to be a path list of *directories* that contain .tcd files.
// Do NOT pass the .tcd file paths themselves.
std::string join_hfile_path(const std::vector<fs::path>& tcd_files) {
#ifdef _WIN32
  const char sep = ';';
#else
  const char sep = ':';
#endif

  std::vector<std::string> dirs;
  dirs.reserve(tcd_files.size());

  auto norm_dir = [&](fs::path p) -> fs::path {
    // If user passed a .tcd file, use its parent directory.
    if (p.has_extension() && to_lower_copy(p.extension().string()) == ".tcd") {
      p = p.parent_path();
    }
    if (p.empty()) p = fs::path(".");
    // Make absolute so child process can find it regardless of cwd changes.
    std::error_code ec;
    fs::path abs = fs::absolute(p, ec);
    if (ec) abs = p;
    abs = abs.lexically_normal();
#ifdef _WIN32
    abs.make_preferred();
#endif
    return abs;
  };

  for (const auto& p : tcd_files) {
    fs::path d = norm_dir(p);
    std::string ds = d.string();
    if (std::find(dirs.begin(), dirs.end(), ds) == dirs.end()) dirs.push_back(std::move(ds));
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
    // Convert UTF-16 to UTF-8
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

// Run tide.exe without cmd.exe parsing (no popen/system). Stream stdout to this process stdout.
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
  si.hStdError = GetStdHandle(STD_ERROR_HANDLE); // keep disclaimers/warnings on stderr

  PROCESS_INFORMATION pi{};

  std::wstring exe_w = widen_utf8(tide_bin_utf8);
  std::wstring cmdline = build_cmdline_win(exe_w, args_w);
  std::vector<wchar_t> cmdBuf(cmdline.begin(), cmdline.end());
  cmdBuf.push_back(L'\0');

  // lpApplicationName = nullptr enables PATH lookup if tide_bin is "tide" / "tide.exe"
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

  // Read child stdout and stream to our stdout.
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
                        const std::string& station_name,
                        const std::string& begin,
                        const std::string& end,
                        const std::string& step_hhmm,
                        bool utc,
                        bool include_sunmoon,   // NOTE: renamed semantics
                        bool omit_units,
                        bool emit_metadata,
                        double dist_km,
                        Kind kind) {
  const auto old_hfile = getenv_str("HFILE_PATH");
  setenv_portable("HFILE_PATH", join_hfile_path(tcd_files));

#ifdef _WIN32
  // Build argv for tide.exe as discrete args (no shell).
  std::vector<std::wstring> args_w;
  args_w.emplace_back(L"-l");  args_w.emplace_back(widen_utf8(station_name));
  args_w.emplace_back(L"-b");  args_w.emplace_back(widen_utf8(begin));
  args_w.emplace_back(L"-e");  args_w.emplace_back(widen_utf8(end));
  args_w.emplace_back(L"-m");  args_w.emplace_back(L"r");
  args_w.emplace_back(L"-f");  args_w.emplace_back(L"c");
  args_w.emplace_back(L"-s");  args_w.emplace_back(widen_utf8(step_hhmm));
  args_w.emplace_back(L"-z");  args_w.emplace_back(utc ? L"y" : L"n");

  // If we do NOT want sun/moon, suppress them.
  if (!include_sunmoon) { args_w.emplace_back(L"-em"); args_w.emplace_back(L"pSsMm"); }
  if (omit_units) { args_w.emplace_back(L"-ou"); args_w.emplace_back(L"y"); }

  // Debug string (for stderr on failure)
  std::ostringstream dbg;
  dbg << shell_quote_double(tide_bin)
      << " -l " << shell_quote_double(station_name)
      << " -b " << shell_quote_double(begin)
      << " -e " << shell_quote_double(end)
      << " -m r -f c"
      << " -s " << shell_quote_double(step_hhmm)
      << " -z " << (utc ? "y" : "n");
  if (!include_sunmoon) dbg << " -em pSsMm";
  if (omit_units) dbg << " -ou y";
  const std::string cmd_str = dbg.str();

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

  const int rc = run_tide_win_stream(tide_bin, args_w, cmd_str);

  if (old_hfile) setenv_portable("HFILE_PATH", *old_hfile);
  else unsetenv_portable("HFILE_PATH");

  return rc;

#else
  // POSIX: use popen() and shell quoting.
  std::ostringstream cmd;
  cmd << shell_quote_double(tide_bin)
      << " -l " << shell_quote_double(station_name)
      << " -b " << shell_quote_double(begin)
      << " -e " << shell_quote_double(end)
      << " -m r -f c"
      << " -s " << shell_quote_double(step_hhmm)
      << " -z " << (utc ? "y" : "n");

  if (!include_sunmoon) cmd << " -em pSsMm";
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
  bool include_sunmoon = false;   // default: suppressed (clean CSV)
  bool omit_units = false;
  bool emit_metadata = true;

  cmd_predict->add_option("--kind", kind_s, "tide | current")->default_val("current");
  cmd_predict->add_option("--begin", begin, "Begin time: \"YYYY-MM-DD HH:MM\"")->required();
  cmd_predict->add_option("--end", end, "End time: \"YYYY-MM-DD HH:MM\"")->required();
  cmd_predict->add_option("--step", step, "Step interval for raw mode: \"HH:MM\"")->default_val("00:10");
  cmd_predict->add_option("--tide-bin", tide_bin, "Path to tide(1) executable")->default_val("tide");
  cmd_predict->add_flag("--utc", utc, "Coerce timestamps to UTC (tide -z y)");
  cmd_predict->add_flag("--sunmoon,!--no-sunmoon", include_sunmoon,
                        "Include sun/moon events (default: suppressed for clean CSV)")->default_val(false);
  cmd_predict->add_flag("--omit-units", omit_units, "Omit unit suffix in numeric fields (tide -ou y)");
  cmd_predict->add_flag("--meta,!--no-meta", emit_metadata, "Emit metadata header line (default on)")->default_val(true);

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
      include_sunmoon,
      omit_units,
      emit_metadata,
      best.distance_km,
      k
    );
  }

  return 0;
}
