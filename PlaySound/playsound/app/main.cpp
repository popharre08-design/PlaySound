// Play Sound - Windows GUI 앱 (WASAPI 루프백 -> Engine -> 헤드폰 출력)
// 사용 전제: Windows 기본 출력 장치를 가상 케이블(VB-Cable 등)로 설정하고,
// 이 앱에서 실제 헤드폰을 "출력 장치"로 선택. (같은 장치면 소리가 두 번 나므로 막음)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commctrl.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <atomic>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include "../core/dsp.h"
#include "../core/storage.h"
#ifdef _MSC_VER
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

#define REL(p) do { if (p) { (p)->Release(); (p) = nullptr; } } while (0)
enum { ID_DEV = 100, ID_PRE, ID_BTN, ID_DLG, WM_STATUS = WM_APP, WM_STOPPED = WM_APP + 1 };

static ps::Engine g_engine;
static ps::Params g_params;
static ps::Store g_store;
static std::atomic<bool> g_run{false};
static std::thread g_thread;
static std::vector<std::wstring> g_devIds;
static HWND g_hwnd, hSl[3], hDev, hPre, hBtn, hDlg, hStatus;

static std::wstring W(const std::string& s) {
  if (s.empty()) return L"";
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  std::wstring w(n, 0); MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n); w.resize(n - 1); return w;
}
static std::string U8(const std::wstring& w) {
  if (w.empty()) return "";
  int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
  std::string s(n, 0); WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr); s.resize(n - 1); return s;
}
static void postStatus(const wchar_t* m) { PostMessageW(g_hwnd, WM_STATUS, 0, (LPARAM) new std::wstring(m)); }

static void audioThread(std::wstring outId) {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  IMMDeviceEnumerator* en = nullptr; IMMDevice *cap = nullptr, *out = nullptr;
  IAudioClient *ac = nullptr, *rc = nullptr; IAudioCaptureClient* cc = nullptr; IAudioRenderClient* rr = nullptr;
  WAVEFORMATEX* mf = nullptr;
  const wchar_t* err = nullptr;
  do {
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&en))) { err = L"오디오 시스템 초기화 실패"; break; }
    if (FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &cap))) { err = L"기본 출력 장치를 찾을 수 없음"; break; }
    if (FAILED(en->GetDevice(outId.c_str(), &out))) { err = L"선택한 출력 장치를 열 수 없음"; break; }
    LPWSTR capId = nullptr; cap->GetId(&capId);
    bool same = capId && outId == capId; CoTaskMemFree(capId);
    if (same) { err = L"기본 출력과 같은 장치입니다. 기본 출력을 가상 케이블로 바꾸세요"; break; }
    if (FAILED(cap->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&ac)) || FAILED(ac->GetMixFormat(&mf))) { err = L"캡처 장치 열기 실패"; break; }
    if (mf->wBitsPerSample != 32 || mf->nChannels < 1) { err = L"지원하지 않는 오디오 형식(32비트 float 필요)"; break; }
    if (FAILED(ac->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 400000, 0, mf, nullptr))) { err = L"루프백 캡처 초기화 실패"; break; }
    if (FAILED(ac->GetService(__uuidof(IAudioCaptureClient), (void**)&cc))) { err = L"캡처 서비스 실패"; break; }
    const int sr = (int)mf->nSamplesPerSec, ch = mf->nChannels;
    WAVEFORMATEX wf = {}; wf.wFormatTag = WAVE_FORMAT_IEEE_FLOAT; wf.nChannels = 2; wf.nSamplesPerSec = sr;
    wf.wBitsPerSample = 32; wf.nBlockAlign = 8; wf.nAvgBytesPerSec = sr * 8;
    if (FAILED(out->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&rc))) { err = L"출력 장치 열기 실패"; break; }
    if (FAILED(rc->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY, 500000, 0, &wf, nullptr))) { err = L"출력 초기화 실패"; break; }
    UINT32 rbuf = 0; rc->GetBufferSize(&rbuf);
    if (FAILED(rc->GetService(__uuidof(IAudioRenderClient), (void**)&rr))) { err = L"출력 서비스 실패"; break; }
    g_engine.init((float)sr, g_params);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    ac->Start(); rc->Start();
    postStatus(L"동작 중");
    std::vector<float> tmp;
    while (g_run) {
      Sleep(5);
      UINT32 pkt = 0;
      while (SUCCEEDED(cc->GetNextPacketSize(&pkt)) && pkt > 0) {
        BYTE* data = nullptr; UINT32 n = 0; DWORD fl = 0;
        if (FAILED(cc->GetBuffer(&data, &n, &fl, nullptr, nullptr))) break;
        tmp.assign((size_t)n * 2, 0.f);
        if (!(fl & AUDCLNT_BUFFERFLAGS_SILENT)) {
          const float* s = (const float*)data;
          for (UINT32 i = 0; i < n; i++) { tmp[2 * i] = s[i * ch]; tmp[2 * i + 1] = s[i * ch + (ch > 1 ? 1 : 0)]; }
        }
        cc->ReleaseBuffer(n);
        g_engine.process(tmp.data(), (int)n);
        UINT32 pad = 0; rc->GetCurrentPadding(&pad);
        UINT32 w = std::min<UINT32>(n, rbuf - pad);
        BYTE* ob = nullptr;
        if (w && SUCCEEDED(rr->GetBuffer(w, &ob))) { std::memcpy(ob, tmp.data(), (size_t)w * 8); rr->ReleaseBuffer(w, 0); }
      }
    }
    ac->Stop(); rc->Stop();
  } while (false);
  if (err) postStatus(err);
  REL(rr); REL(rc); REL(cc); REL(ac); REL(out); REL(cap); REL(en);
  CoTaskMemFree(mf);
  CoUninitialize();
  PostMessageW(g_hwnd, WM_STOPPED, 0, 0);
}

static void stopAudio() { g_run = false; if (g_thread.joinable()) g_thread.join(); }

static void readUi() {
  g_params.intensity = SendMessageW(hSl[0], TBM_GETPOS, 0, 0) / 100.f;
  g_params.reverbMix = SendMessageW(hSl[1], TBM_GETPOS, 0, 0) / 100.f;
  g_params.rearLevel = SendMessageW(hSl[2], TBM_GETPOS, 0, 0) / 100.f;
  g_params.dialogueLock = SendMessageW(hDlg, BM_GETCHECK, 0, 0) == BST_CHECKED;
  g_engine.setParams(g_params);
}
static void writeUi() {
  SendMessageW(hSl[0], TBM_SETPOS, TRUE, (LPARAM)(g_params.intensity * 100 + 0.5f));
  SendMessageW(hSl[1], TBM_SETPOS, TRUE, (LPARAM)(g_params.reverbMix * 100 + 0.5f));
  SendMessageW(hSl[2], TBM_SETPOS, TRUE, (LPARAM)(g_params.rearLevel * 100 + 0.5f));
  SendMessageW(hDlg, BM_SETCHECK, g_params.dialogueLock ? BST_CHECKED : BST_UNCHECKED, 0);
  g_engine.setParams(g_params);
}

static void fillDevices() {
  IMMDeviceEnumerator* en = nullptr; IMMDeviceCollection* col = nullptr; IMMDevice* def = nullptr;
  if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&en))) return;
  std::wstring defId;
  if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &def))) { LPWSTR id = nullptr; def->GetId(&id); if (id) defId = id; CoTaskMemFree(id); }
  int sel = -1;
  if (SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col))) {
    UINT n = 0; col->GetCount(&n);
    for (UINT i = 0; i < n; i++) {
      IMMDevice* d = nullptr; if (FAILED(col->Item(i, &d))) continue;
      LPWSTR id = nullptr; d->GetId(&id);
      IPropertyStore* ps = nullptr; PROPVARIANT v; PropVariantInit(&v);
      std::wstring name = L"(이름 없음)";
      if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps)) && SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR && v.pwszVal) name = v.pwszVal;
      bool isDef = id && id == defId;
      if (isDef) name += L"  [기본 출력 - 선택 불가]";
      SendMessageW(hDev, CB_ADDSTRING, 0, (LPARAM)name.c_str());
      g_devIds.push_back(id ? id : L"");
      if (!isDef && sel < 0) sel = (int)g_devIds.size() - 1;
      PropVariantClear(&v); REL(ps); CoTaskMemFree(id); REL(d);
    }
  }
  SendMessageW(hDev, CB_SETCURSEL, sel < 0 ? 0 : sel, 0);
  REL(col); REL(def); REL(en);
}

static HWND mk(const wchar_t* cls, const wchar_t* txt, DWORD st, int x, int y, int w, int h, int id = 0) {
  return CreateWindowExW(0, cls, txt, WS_CHILD | WS_VISIBLE | st, x, y, w, h, g_hwnd, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
}

static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  switch (m) {
    case WM_CREATE: {
      g_hwnd = h;
      int y = 14;
      mk(L"STATIC", L"출력 장치 (헤드폰/이어폰)", 0, 16, y, 380, 18); y += 20;
      hDev = mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, 16, y, 380, 200, ID_DEV); y += 34;
      mk(L"STATIC", L"프리셋", 0, 16, y, 380, 18); y += 20;
      hPre = mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, 16, y, 380, 200, ID_PRE); y += 38;
      const wchar_t* names[3] = {L"입체감", L"리버브", L"뒤쪽 앰비언스"};
      const int maxv[3] = {100, 100, 150};
      for (int i = 0; i < 3; i++) {
        mk(L"STATIC", names[i], 0, 16, y + 4, 100, 18);
        hSl[i] = mk(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS, 120, y, 276, 28);
        SendMessageW(hSl[i], TBM_SETRANGE, TRUE, MAKELONG(0, maxv[i])); y += 34;
      }
      hDlg = mk(L"BUTTON", L"대사 중앙 고정", BS_AUTOCHECKBOX, 16, y, 200, 22); y += 36;
      hBtn = mk(L"BUTTON", L"시작", BS_PUSHBUTTON, 16, y, 120, 34, ID_BTN);
      hStatus = mk(L"STATIC", L"대기 중 - 기본 출력을 가상 케이블로 설정하세요", 0, 148, y + 8, 250, 36);
      for (auto& n : g_store.listPresets()) SendMessageW(hPre, CB_ADDSTRING, 0, (LPARAM)W(n).c_str());
      fillDevices();
      EnumChildWindows(h, [](HWND c, LPARAM) -> BOOL { SendMessageW(c, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE); return TRUE; }, 0);
      writeUi();
      return 0;
    }
    case WM_HSCROLL: readUi(); return 0;
    case WM_COMMAND:
      if (LOWORD(wp) == ID_DLG || (HWND)lp == hDlg) readUi();
      if (LOWORD(wp) == ID_PRE && HIWORD(wp) == CBN_SELCHANGE) {
        int i = (int)SendMessageW(hPre, CB_GETCURSEL, 0, 0);
        wchar_t buf[128] = {};
        if (i >= 0 && SendMessageW(hPre, CB_GETLBTEXT, i, (LPARAM)buf) > 0)
          if (auto p = g_store.loadPreset(U8(buf))) { g_params = *p; writeUi(); }
      }
      if (LOWORD(wp) == ID_BTN) {
        if (g_run) { stopAudio(); SetWindowTextW(hBtn, L"시작"); SetWindowTextW(hStatus, L"정지됨"); }
        else {
          int i = (int)SendMessageW(hDev, CB_GETCURSEL, 0, 0);
          if (i < 0 || i >= (int)g_devIds.size()) return 0;
          if (g_thread.joinable()) g_thread.join();
          readUi(); g_run = true;
          g_thread = std::thread(audioThread, g_devIds[i]);
          SetWindowTextW(hBtn, L"정지"); SetWindowTextW(hStatus, L"시작하는 중...");
        }
      }
      return 0;
    case WM_STATUS: { auto* s = (std::wstring*)lp; SetWindowTextW(hStatus, s->c_str()); delete s; return 0; }
    case WM_STOPPED:  // 스레드가 스스로 끝난 경우(오류 등)
      if (g_run) { g_run = false; if (g_thread.joinable()) g_thread.join(); SetWindowTextW(hBtn, L"시작"); }
      return 0;
    case WM_CLOSE: stopAudio(); readUi(); g_store.saveSettings(g_params); DestroyWindow(h); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
  }
  return DefWindowProcW(h, m, wp, lp);
}

int WINAPI WinMain(HINSTANCE hi, HINSTANCE, LPSTR, int show) {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  INITCOMMONCONTROLSEX ic = {sizeof(ic), ICC_BAR_CLASSES}; InitCommonControlsEx(&ic);
  g_params = g_store.loadSettings();
  WNDCLASSW wc = {}; wc.lpfnWndProc = proc; wc.hInstance = hi; wc.lpszClassName = L"PlaySoundWnd";
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW); wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
  RegisterClassW(&wc);
  RECT r = {0, 0, 412, 330}; AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
  HWND w = CreateWindowW(L"PlaySoundWnd", L"Play Sound", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                         CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr, hi, nullptr);
  ShowWindow(w, show); UpdateWindow(w);
  MSG msg; while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
  CoUninitialize();
  return 0;
}
