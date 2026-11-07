// 사용법:
//   ps_cli demo out.wav                       테스트 신호 생성
//   ps_cli process in.wav out.wav [--preset NAME] [--intensity X] [--reverb X] [--rear X]
//   ps_cli save-preset NAME [--intensity X ...]   프리셋 저장
//   ps_cli list                               프리셋 목록
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include "../core/dsp.h"
#include "../core/storage.h"

static bool readWav(const std::string& path, std::vector<float>& out, int& sr) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  char id[4], wv[4]; uint32_t sz;
  f.read(id, 4); f.ignore(4); f.read(wv, 4);
  if (std::memcmp(id, "RIFF", 4) || std::memcmp(wv, "WAVE", 4)) return false;
  uint16_t tag = 0, ch = 0, bits = 0; uint32_t rate = 0; std::vector<char> raw;
  while (f.read(id, 4) && f.read((char*)&sz, 4)) {
    if (!std::memcmp(id, "fmt ", 4)) {
      uint32_t br; uint16_t ba;
      f.read((char*)&tag, 2); f.read((char*)&ch, 2); f.read((char*)&rate, 4);
      f.read((char*)&br, 4); f.read((char*)&ba, 2); f.read((char*)&bits, 2);
      f.ignore(sz - 16 + (sz & 1));
    } else if (!std::memcmp(id, "data", 4)) {
      raw.resize(sz); f.read(raw.data(), sz);
    } else f.ignore(sz + (sz & 1));
  }
  if (ch < 1 || ch > 2 || raw.empty()) return false;
  bool f32 = tag == 3 && bits == 32;
  if (!(f32 || (tag == 1 && bits == 16))) return false;
  size_t n = raw.size() / (bits / 8) / ch;
  out.assign(n * 2, 0.f); sr = (int)rate;
  for (size_t i = 0; i < n; i++)
    for (int c = 0; c < 2; c++) {
      size_t idx = i * ch + (ch == 2 ? c : 0);
      float v;
      if (f32) std::memcpy(&v, &raw[idx * 4], 4);
      else { int16_t s; std::memcpy(&s, &raw[idx * 2], 2); v = s / 32768.f; }
      out[i * 2 + c] = v;
    }
  return true;
}

static bool writeWav(const std::string& path, const std::vector<float>& d, int sr) {
  std::ofstream o(path, std::ios::binary);
  if (!o) return false;
  uint32_t bytes = (uint32_t)d.size() * 2, v32; uint16_t v16;
  auto w32 = [&](uint32_t x) { o.write((char*)&x, 4); };
  auto w16 = [&](uint16_t x) { o.write((char*)&x, 2); };
  o.write("RIFF", 4); w32(36 + bytes); o.write("WAVEfmt ", 8);
  w32(16); w16(1); w16(2); w32(sr); w32(sr * 4); w16(4); w16(16);
  o.write("data", 4); w32(bytes);
  for (float x : d) { int16_t s = (int16_t)std::lrint(ps::clampf(x, -1, 1) * 32767); o.write((char*)&s, 2); }
  (void)v32; (void)v16;
  return (bool)o;
}

static std::vector<float> makeDemo(int sr) {
  std::mt19937 g(1); std::normal_distribution<float> nd(0, 0.08f);
  std::vector<float> d(sr * 5 * 2);
  for (int i = 0; i < sr * 5; i++) {
    float t = (float)i / sr;
    float burst = std::fmod(t, 1.f) < 0.3f ? 0.25f * std::sin(2 * ps::kPi * 1000 * t) : 0.f;  // 대사 대용(중앙)
    float n = nd(g);                                                                           // 앰비언스(사이드)
    d[2 * i]     = 0.2f * std::sin(2 * ps::kPi * 220 * t) + burst + n + 0.2f * std::sin(2 * ps::kPi * 60 * t);
    d[2 * i + 1] = 0.2f * std::sin(2 * ps::kPi * 330 * t) + burst - n + 0.2f * std::sin(2 * ps::kPi * 60 * t);
  }
  return d;
}

static float corr(const std::vector<float>& d) {
  double ll = 0, rr = 0, lr = 0;
  for (size_t i = 0; i + 1 < d.size(); i += 2) { ll += d[i] * d[i]; rr += d[i + 1] * d[i + 1]; lr += d[i] * d[i + 1]; }
  return (float)(lr / std::sqrt(ll * rr + 1e-12));
}

int main(int argc, char** argv) {
  if (argc < 2) { std::puts("usage: ps_cli demo|process|save-preset|list ..."); return 1; }
  std::string cmd = argv[1];
  ps::Store store;
  ps::Params p = store.loadSettings();
  int pos = 2;
  std::string a2 = argc > 2 ? argv[2] : "", a3 = argc > 3 ? argv[3] : "";
  for (int i = 2; i < argc; i++) {
    std::string k = argv[i];
    auto val = [&]() { return i + 1 < argc ? std::stof(argv[++i]) : 0.f; };
    if (k == "--preset" && i + 1 < argc) { if (auto q = store.loadPreset(argv[++i])) p = *q; else { std::puts("preset not found"); return 1; } }
    else if (k == "--intensity") p.intensity = val();
    else if (k == "--reverb") p.reverbMix = val();
    else if (k == "--rear") p.rearLevel = val();
  }
  p = ps::sanitize(p); (void)pos;

  if (cmd == "list") { for (auto& n : store.listPresets()) std::puts(n.c_str()); return 0; }
  if (cmd == "save-preset") { bool ok = store.savePreset(a2, p); std::puts(ok ? "saved" : "failed (invalid/builtin name)"); return ok ? 0 : 1; }
  if (cmd == "demo") { return writeWav(a2, makeDemo(48000), 48000) ? 0 : 1; }
  if (cmd == "process") {
    std::vector<float> d; int sr;
    if (!readWav(a2, d, sr)) { std::puts("cannot read wav (16-bit PCM or float32, mono/stereo)"); return 1; }
    float c0 = corr(d);
    ps::Engine e; e.init((float)sr, p);
    for (size_t off = 0; off < d.size() / 2; off += 480) {  // 10ms 블록 (실시간 환경 모사)
      int n = (int)std::min<size_t>(480, d.size() / 2 - off);
      e.process(&d[off * 2], n);
    }
    float pk = 0; bool bad = false;
    for (float x : d) { pk = std::max(pk, std::fabs(x)); if (!std::isfinite(x)) bad = true; }
    std::printf("L/R correlation: %.3f -> %.3f | peak %.3f | NaN/Inf: %s\n", c0, corr(d), pk, bad ? "YES" : "no");
    store.saveSettings(p);  // 마지막 사용 설정 저장
    return writeWav(a3, d, sr) ? 0 : 1;
  }
  return 1;
}
