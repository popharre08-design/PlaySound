// Play Sound DSP core - header-only, no allocation in process(), no locks.
// APO / WASAPI 앱 / CLI 어디서든 그대로 재사용.
#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include <vector>

namespace ps {
constexpr float kPi = 3.14159265358979f;

struct Params {
  float intensity = 0.7f;   // 0~1 원본 vs 공간 처리 비율
  float reverbMix = 0.18f;  // 0~1 룸 리버브/초기 반사음
  float rearLevel = 0.8f;   // 0~1.5 뒤쪽 앰비언스 크기
  float bassCutHz = 120.f;  // 이 아래는 공간화 없이 중앙 유지
  float ceilingDb = -1.f;   // 리미터 천장
  bool dialogueLock = true; // 대사(중앙) 정면 고정
};

class Biquad {  // 2차 버터워스
 public:
  void setLowpass(float fc, float fs) { design(fc, fs, false); }
  void setHighpass(float fc, float fs) { design(fc, fs, true); }
  float process(float x) {
    float y = b0_ * x + z1_;
    z1_ = b1_ * x - a1_ * y + z2_;
    z2_ = b2_ * x - a2_ * y;
    return y;
  }
 private:
  void design(float fc, float fs, bool hp) {
    float w = 2 * kPi * fc / fs, c = std::cos(w), al = std::sin(w) / (2 * 0.70710678f);
    float a0 = 1 + al;
    a1_ = -2 * c / a0; a2_ = (1 - al) / a0;
    b0_ = (hp ? (1 + c) : (1 - c)) / 2 / a0;
    b1_ = (hp ? -(1 + c) : (1 - c)) / a0;
    b2_ = b0_;
  }
  float b0_ = 1, b1_ = 0, b2_ = 0, a1_ = 0, a2_ = 0, z1_ = 0, z2_ = 0;
};

class OnePole {
 public:
  void setCutoff(float fc, float fs) { a_ = 1 - std::exp(-2 * kPi * fc / fs); }
  float process(float x) { y_ += a_ * (x - y_); return y_; }
 private:
  float a_ = 1, y_ = 0;
};

class FracDelay {  // 선형 보간 분수 딜레이 (ITD용)
 public:
  FracDelay() : buf_(256, 0.f) {}
  void setDelay(float d) { d_ = std::min(std::max(d, 0.f), 250.f); }
  float process(float x) {
    int n = (int)buf_.size();
    buf_[w_] = x;
    float rp = (float)w_ - d_;
    if (rp < 0) rp += n;
    int i0 = (int)rp, i1 = (i0 + 1) % n;
    float fr = rp - (float)i0;
    float y = buf_[i0] * (1 - fr) + buf_[i1] * fr;
    w_ = (w_ + 1) % n;
    return y;
  }
 private:
  std::vector<float> buf_; int w_ = 0; float d_ = 0;
};

class Allpass {  // 디코릴레이터 / 리버브 확산기
 public:
  explicit Allpass(int n = 1, float g = 0.5f) : buf_(std::max(n, 1), 0.f), g_(g) {}
  float process(float x) {
    float z = buf_[i_], y = -g_ * x + z;
    buf_[i_] = x + g_ * y;
    i_ = (i_ + 1) % (int)buf_.size();
    return y;
  }
 private:
  std::vector<float> buf_; int i_ = 0; float g_;
};

class Comb {
 public:
  Comb(int n, float fb, float damp) : buf_(std::max(n, 1), 0.f), fb_(fb), damp_(damp) {}
  float process(float x) {
    float o = buf_[i_];
    lp_ = o * (1 - damp_) + lp_ * damp_;
    buf_[i_] = x + lp_ * fb_;
    i_ = (i_ + 1) % (int)buf_.size();
    return o;
  }
 private:
  std::vector<float> buf_; int i_ = 0; float fb_, damp_, lp_ = 0;
};

// 간이 바이노럴: ITD(Woodworth) + 먼 쪽 귀 고음 감쇠 + 뒤쪽 핀나 감쇠.
// 나중에 SOFA HRTF 컨볼루션으로 이 클래스만 교체하면 됨.
class BinauralSource {
 public:
  void init(float azDeg, float fs, float level = 1.f) {
    float az = azDeg * kPi / 180.f;
    float lat = std::asin(std::sin(az));
    float sl = std::sin(lat), al = std::fabs(lat), as = std::fabs(sl);
    float rear = std::cos(az) < 0 ? 1.f : 0.f;
    float itd = 0.0875f / 343.f * (al + std::sin(al));
    right_ = sl >= 0;
    far_.setDelay(itd * fs);
    float lim = fs * 0.45f;
    float nearFc = std::min(16000.f * (1 - 0.35f * rear), lim);
    float farFc = std::min((16000.f - 13500.f * as) * (1 - 0.35f * rear), lim);
    nearLp_.setCutoff(nearFc, fs);
    farLp_.setCutoff(farFc, fs);
    nearG_ = level * (1 + 0.15f * as);
    farG_ = level * (1 - 0.35f * as);
  }
  void process(float x, float& l, float& r) {
    float n = nearLp_.process(x) * nearG_;
    float f = farLp_.process(far_.process(x)) * farG_;
    l += right_ ? f : n;
    r += right_ ? n : f;
  }
 private:
  FracDelay far_; OnePole nearLp_, farLp_;
  float nearG_ = 1, farG_ = 1; bool right_ = false;
};

class Reverb {  // 초기 반사음 4탭 + Freeverb 스타일 후기 잔향
 public:
  void init(float fs) {
    float k = fs / 44100.f;
    cl_.clear(); cr_.clear();
    for (int n : {1116, 1188, 1277, 1356}) {
      cl_.emplace_back((int)(n * k), 0.80f, 0.3f);
      cr_.emplace_back((int)((n + 23) * k), 0.80f, 0.3f);
    }
    apl_ = Allpass((int)(556 * k), 0.5f);
    apr_ = Allpass((int)(579 * k), 0.5f);
    er_.assign(4096, 0.f);
    static const float ms[4] = {7.f, 11.5f, 17.f, 23.f};
    for (int i = 0; i < 4; i++) erDelay_[i] = std::min((int)(ms[i] * 0.001f * fs), 4095);
  }
  void process(float in, float& l, float& r) {
    er_[w_] = in;
    static const float g[4] = {0.5f, 0.4f, 0.3f, 0.25f};
    float el = 0, er = 0;
    for (int i = 0; i < 4; i++) {
      float v = er_[(w_ - erDelay_[i] + 4096) % 4096] * g[i];
      (i % 2 ? er : el) += v;
    }
    w_ = (w_ + 1) % 4096;
    float sl = 0, sr = 0, x = in * 0.05f;
    for (auto& c : cl_) sl += c.process(x);
    for (auto& c : cr_) sr += c.process(x);
    l = apl_.process(sl) + el;
    r = apr_.process(sr) + er;
  }
 private:
  std::vector<Comb> cl_, cr_; Allpass apl_, apr_;
  std::vector<float> er_; int erDelay_[4] = {0, 0, 0, 0}; int w_ = 0;
};

class Engine {
 public:
  void init(float fs, const Params& p = Params()) {
    fs_ = fs; slot_[0] = slot_[1] = p; active_ = 0; smooth_ = p.intensity;
    designBass(p.bassCutHz);
    const float k = fs / 48000.f;
    decA_.clear(); decB_.clear(); gain_ = 1.f;
    for (int d : {113, 157, 211}) decA_.emplace_back((int)(d * k), 0.5f);
    for (int d : {127, 173, 229}) decB_.emplace_back((int)(d * k), 0.5f);
    src_[0].init(-30, fs); src_[1].init(30, fs);   // 앞 L/R
    src_[2].init(0, fs);                           // 중앙(대사)
    src_[3].init(-120, fs); src_[4].init(120, fs); // 뒤 L/R(앰비언스)
    rev_.init(fs);
    rel_ = 1 - std::exp(-1.f / (0.1f * fs));
  }
  // UI 스레드에서 호출 (더블 버퍼: 오디오 스레드는 락 없이 읽음)
  void setParams(const Params& p) {
    int n = 1 - active_.load(std::memory_order_acquire);
    slot_[n] = p;
    active_.store(n, std::memory_order_release);
  }
  // 인터리브 스테레오 float, 제자리 처리. 오디오 스레드용.
  void process(float* io, int frames) {
    const Params p = slot_[active_.load(std::memory_order_acquire)];
    if (p.bassCutHz != bassHz_) designBass(p.bassCutHz);
    const float ceil = std::pow(10.f, p.ceilingDb / 20.f);
    const float sm = 0.002f;
    for (int i = 0; i < frames; i++) {
      float L = io[2 * i], R = io[2 * i + 1];
      float bass = 0.5f * (lpL_.process(L) + lpR_.process(R));
      float hl = hpL_.process(L), hr = hpR_.process(R);
      float m = 0.5f * (hl + hr), s = 0.5f * (hl - hr);
      float da = s, db = -s;
      for (auto& a : decA_) da = a.process(da);
      for (auto& a : decB_) db = a.process(db);
      float fl = hl, fr = hr, c = 0;
      if (p.dialogueLock) { fl -= 0.6f * m; fr -= 0.6f * m; c = 1.2f * m; }
      float vl = 0, vr = 0;
      src_[0].process(fl, vl, vr);
      src_[1].process(fr, vl, vr);
      src_[2].process(c, vl, vr);
      src_[3].process(da * p.rearLevel, vl, vr);
      src_[4].process(db * p.rearLevel, vl, vr);
      float rl, rr;
      rev_.process(m, rl, rr);
      float sl = 0.9f * vl + bass + p.reverbMix * rl;
      float sr = 0.9f * vr + bass + p.reverbMix * rr;
      smooth_ += sm * (p.intensity - smooth_);
      float ol = (1 - smooth_) * L + smooth_ * sl;
      float orr = (1 - smooth_) * R + smooth_ * sr;
      float pk = std::max(std::fabs(ol), std::fabs(orr));
      float tgt = pk > ceil ? ceil / pk : 1.f;
      if (tgt < gain_) gain_ = tgt; else gain_ += rel_ * (tgt - gain_);
      io[2 * i] = ol * gain_; io[2 * i + 1] = orr * gain_;
    }
  }
 private:
  void designBass(float hz) {
    bassHz_ = hz;
    lpL_.setLowpass(hz, fs_); lpR_.setLowpass(hz, fs_);
    hpL_.setHighpass(hz, fs_); hpR_.setHighpass(hz, fs_);
  }
  float fs_ = 48000, bassHz_ = 0, smooth_ = 0, gain_ = 1, rel_ = 0.0002f;
  Params slot_[2]; std::atomic<int> active_{0};
  Biquad lpL_, lpR_, hpL_, hpR_;
  std::vector<Allpass> decA_, decB_;
  BinauralSource src_[5]; Reverb rev_;
};
}  // namespace ps
