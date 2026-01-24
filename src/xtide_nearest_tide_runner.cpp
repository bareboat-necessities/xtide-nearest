/*
  Copyright 2026, Mikhail Grushinskiy
*/
#include "xtide_nearest/xtide_nearest.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef _WIN32
#include <sys/wait.h>
#endif

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX 1
#endif
#include <windows.h>
#endif

namespace xtide_nearest {
namespace {

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

bool is_tcd_file_ext(const fs::path& p) {
  if (!p.has_extension()) return false;
  auto ext = p.extension().string();
  for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
  return ext == ".tcd";
}

// XTide expects HFILE_PATH = path list of directories containing .tcd files.
// Put preferred directory first (station’s own .tcd dir) to avoid name collisions.
std::string join_hfile_path(const std::vector<fs::path>& tcd_files, const fs::path& prefer_tcd_file) {
#ifdef _WIN32
  const char sep = ';';
#else
  const char sep = ':';
#endif

  auto norm_abs = [](const fs::path& p) {
    std::error_code ec;
    fs::path a = fs::absolute(p, ec);
    if (ec) a = p;
    a = a.lexically_normal();
#ifdef _WIN32
    a.make_preferred();
#endif
    return a;
  };

  auto norm_dir = [&](fs::path p) -> fs::path {
    if (p.has_extension() && is_tcd_file_ext(p)) p = p.parent_path();
    if (p.empty()) p = fs::path(".");
    return norm_abs(p);
  };

  std::vector<std::string> dirs;

  auto push_unique = [&](const fs::path& d) {
    std::string ds = d.string();
#ifdef _WIN32
    std::string key = ds;
    for (auto& c : key) c = (char)std::tolower((unsigned char)c);
    for (const auto& existing : dirs) {
      std::string ex = existing;
      for (auto& c : ex) c = (char)std::tolower((unsigned char)c);
      if (ex == key) return;
    }
#else
    for (const auto& existing : dirs) if (existing == ds) return;
#endif
    dirs.push_back(std::move(ds));
  };

  if (!prefer_tcd_file.empty()) push_unique(norm_dir(prefer_tcd_file));
  for (const auto& p : tcd_files) push_unique(norm_dir(p));

  std::ostringstream oss;
  for (size_t i = 0; i < dirs.size(); ++i) {
    if (i) oss << sep;
    oss << dirs[i];
  }
  return oss.str();
}

// Environment changes (HFILE_PATH) are process-global; serialize runner usage.
std::mutex g_env_mutex;

#ifdef _WIN32

std::wstring widen_utf8(const std::string& s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  if (n <= 0) throw std::runtime_error("MultiByteToWideChar failed");
  std::wstring w((size_t)n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
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
      out.resize((size_t)m - 1);
      WideCharToMultiByte(CP_UTF8, 0, buf, -1, out.data(), m, nullptr, nullptr);
    }
  }
  if (buf) LocalFree(buf);
  if (out.empty()) out = "Windows error " + std::to_string(err);
  return out;
}

std::wstring quote_arg_win(std::wstring_view s) {
  const bool need_quotes = s.empty() || s.find_first_of(L" \t\n\v\"") != std::wstring_view::npos;
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

int run_tide_win_capture(const std::string& tide_bin_utf8,
                         const std::vector<std::wstring>& args_w,
                         std::string& out_stdout,
                         std::string& out_err) {
  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;

  HANDLE childStdoutRead = nullptr, childStdoutWrite = nullptr;
  if (!CreatePipe(&childStdoutRead, &childStdoutWrite, &sa, 0)) {
    out_err = "CreatePipe(stdout) failed: " + win_errstr(GetLastError());
    return 2;
  }
  SetHandleInformation(childStdoutRead, HANDLE_FLAG_INHERIT, 0);

  // stderr passthrough to parent stderr (still captured as message on failures only).
  HANDLE childStderr = GetStdHandle(STD_ERROR_HANDLE);

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
  si.hStdOutput = childStdoutWrite;
  si.hStdError  = childStderr;

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
    out_err = "CreateProcessW failed: " + win_errstr(e);
    return 2;
  }

  out_stdout.clear();
  std::array<char, 8192> buf{};
  DWORD nread = 0;
  while (ReadFile(childStdoutRead, buf.data(), (DWORD)buf.size(), &nread, nullptr) && nread > 0) {
    out_stdout.append(buf.data(), buf.data() + nread);
  }
  CloseHandle(childStdoutRead);

  WaitForSingleObject(pi.hProcess, INFINITE);

  DWORD exit_code = 0;
  GetExitCodeProcess(pi.hProcess, &exit_code);

  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);

  return (exit_code == 0) ? 0 : 3;
}

#endif // _WIN32

} // namespace

TideRunResult RunTideCapture(const TideRunRequest& req) {
  std::lock_guard<std::mutex> lk(g_env_mutex);

  TideRunResult r;
  if (req.station.name.empty()) {
    r.rc = 2;
    r.stdout_text.clear();
    return r;
  }

  const auto old_hfile = getenv_str("HFILE_PATH");
  setenv_portable("HFILE_PATH", join_hfile_path(req.tcd_files, req.station.tcd_path));

#ifdef _WIN32
  std::vector<std::wstring> args_w;
  args_w.emplace_back(L"-l"); args_w.emplace_back(widen_utf8(req.station.name));
  args_w.emplace_back(L"-b"); args_w.emplace_back(widen_utf8(req.begin));
  args_w.emplace_back(L"-e"); args_w.emplace_back(widen_utf8(req.end));
  args_w.emplace_back(L"-m"); args_w.emplace_back(widen_utf8(req.mode));
  args_w.emplace_back(L"-f"); args_w.emplace_back(widen_utf8(req.format));
  args_w.emplace_back(L"-s"); args_w.emplace_back(widen_utf8(req.step));
  args_w.emplace_back(L"-z"); args_w.emplace_back(req.utc ? L"y" : L"n");

  if (!req.include_sunmoon) { args_w.emplace_back(L"-em"); args_w.emplace_back(L"pSsMm"); }
  if (req.omit_units) { args_w.emplace_back(L"-ou"); args_w.emplace_back(L"y"); }

  std::string err;
  r.rc = run_tide_win_capture(req.tide_bin, args_w, r.stdout_text, err);
  if (!err.empty()) {
    // put errors into stdout_text? no: keep rc and let caller print err if desired
  }

#else
  std::ostringstream cmd;
  cmd << shell_quote_double(req.tide_bin)
      << " -l " << shell_quote_double(req.station.name)
      << " -b " << shell_quote_double(req.begin)
      << " -e " << shell_quote_double(req.end)
      << " -m " << shell_quote_double(req.mode)
      << " -f " << shell_quote_double(req.format)
      << " -s " << shell_quote_double(req.step)
      << " -z " << (req.utc ? "y" : "n");

  if (!req.include_sunmoon) cmd << " -em pSsMm";
  if (req.omit_units) cmd << " -ou y";

  const std::string cmd_str = cmd.str();

  FILE* pipe = popen_portable(cmd_str.c_str(), "r");
  if (!pipe) {
    r.rc = 2;
    r.stdout_text.clear();
  } else {
    std::array<char, 8192> buf{};
    while (std::fgets(buf.data(), (int)buf.size(), pipe)) {
      r.stdout_text += buf.data();
    }
    const int status = pclose_portable(pipe);
    int exit_code = status;
    if (WIFEXITED(status)) exit_code = WEXITSTATUS(status);
    r.rc = (exit_code == 0) ? 0 : 3;
  }
#endif

  if (old_hfile) setenv_portable("HFILE_PATH", *old_hfile);
  else unsetenv_portable("HFILE_PATH");

  return r;
}
} // namespace xtide_nearest