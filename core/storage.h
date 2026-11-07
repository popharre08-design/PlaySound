// 설정/프리셋 저장. 외부 라이브러리 없음. 텍스트 key=value, 원자적 쓰기.
// 저장 위치: Windows %APPDATA%\PlaySound\  /  그 외 ~/.config/playsound/
#pragma once
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>
#include "dsp.h"

namespace ps {
namespace fs = std::filesystem;

inline fs::path dataDir() {
#ifdef _WIN32
  const char* a = std::getenv("APPDATA");
  return fs::path(a ? a : ".") / "PlaySound";
#else
  if (const char* x = std::getenv("XDG_CONFIG_HOME")) return fs::path(x) / "playsound";
  const char* h = std::getenv("HOME");
  return fs::path(h ? h : ".") / ".config" / "playsound";
#endif
}

inline float clampf(float v, float lo, float hi) { return std::min(std::max(v, lo), hi); }

inline Params sanitize(Params p) {  // 파일은 신뢰하지 않음 (APO 크래시 방지)
  p.intensity = clampf(p.intensity, 0, 1);
  p.reverbMix = clampf(p.reverbMix, 0, 1);
  p.rearLevel = clampf(p.rearLevel, 0, 1.5f);
  p.bassCutHz = clampf(p.bassCutHz, 40, 300);
  p.ceilingDb = clampf(p.ceilingDb, -12, 0);
  return p;
}

inline std::string serialize(const Params& p) {
  std::ostringstream o;
  o << "version=1\nintensity=" << p.intensity << "\nreverbMix=" << p.reverbMix
    << "\nrearLevel=" << p.rearLevel << "\nbassCutHz=" << p.bassCutHz
    << "\nceilingDb=" << p.ceilingDb << "\ndialogueLock=" << (p.dialogueLock ? 1 : 0) << "\n";
  return o.str();
}

inline Params parse(const std::string& text) {
  Params p;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    std::string k = line.substr(0, eq), v = line.substr(eq + 1);
    try {
      float f = std::stof(v);
      if (k == "intensity") p.intensity = f;
      else if (k == "reverbMix") p.reverbMix = f;
      else if (k == "rearLevel") p.rearLevel = f;
      else if (k == "bassCutHz") p.bassCutHz = f;
      else if (k == "ceilingDb") p.ceilingDb = f;
      else if (k == "dialogueLock") p.dialogueLock = f != 0;
    } catch (...) {}  // 깨진 줄은 무시 → 기본값 유지
  }
  return sanitize(p);
}

inline std::optional<Params> builtinPreset(const std::string& n) {
  Params p;
  if (n == "cinema")        { p.intensity = 0.75f; p.reverbMix = 0.15f; p.rearLevel = 0.9f; }
  else if (n == "concert")  { p.intensity = 0.8f;  p.reverbMix = 0.35f; p.rearLevel = 0.7f; p.dialogueLock = false; }
  else if (n == "immersive"){ p.intensity = 1.0f;  p.reverbMix = 0.25f; p.rearLevel = 1.2f; }
  else return std::nullopt;
  return p;
}

class Store {
 public:
  explicit Store(fs::path root = dataDir()) : root_(std::move(root)) {
    std::error_code ec; fs::create_directories(root_ / "presets", ec);
  }
  Params loadSettings() const {
    std::ifstream f(root_ / "settings.ini");
    if (!f) return Params();
    std::stringstream ss; ss << f.rdbuf();
    return parse(ss.str());
  }
  bool saveSettings(const Params& p) const { return atomicWrite(root_ / "settings.ini", serialize(sanitize(p))); }

  bool savePreset(const std::string& name, const Params& p) const {
    if (!validName(name) || builtinPreset(name)) return false;
    return atomicWrite(root_ / "presets" / (name + ".ps"), serialize(sanitize(p)));
  }
  std::optional<Params> loadPreset(const std::string& name) const {
    if (auto b = builtinPreset(name)) return b;
    if (!validName(name)) return std::nullopt;
    std::ifstream f(root_ / "presets" / (name + ".ps"));
    if (!f) return std::nullopt;
    std::stringstream ss; ss << f.rdbuf();
    return parse(ss.str());
  }
  bool removePreset(const std::string& name) const {
    if (!validName(name)) return false;
    std::error_code ec; return fs::remove(root_ / "presets" / (name + ".ps"), ec);
  }
  std::vector<std::string> listPresets() const {
    std::vector<std::string> v{"cinema", "concert", "immersive"};
    std::error_code ec;
    for (auto& e : fs::directory_iterator(root_ / "presets", ec))
      if (e.path().extension() == ".ps") v.push_back(e.path().stem().string());
    return v;
  }
 private:
  static bool validName(const std::string& n) {  // 경로 조작 방지
    if (n.empty() || n.size() > 40) return false;
    for (unsigned char c : n)
      if (!(std::isalnum(c) || c == '_' || c == '-' || c == ' ' || c >= 0x80)) return false;
    return true;
  }
  static bool atomicWrite(const fs::path& path, const std::string& data) {
    fs::path tmp = path; tmp += ".tmp";
    { std::ofstream o(tmp, std::ios::binary | std::ios::trunc); if (!o) return false; o << data; if (!o) return false; }
    std::error_code ec; fs::rename(tmp, path, ec);
    return !ec;
  }
  fs::path root_;
};
}  // namespace ps
