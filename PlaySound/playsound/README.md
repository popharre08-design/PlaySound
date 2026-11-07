# Play Sound (v0.1 코어)

Windows 시스템 전체 오디오를 입체화하는 엔진(APO 지향)의 **DSP 코어 + 저장 + 테스트 CLI**.

## 앱 실행 (Windows)
`PlaySound.exe`를 더블클릭하면 창이 뜹니다. exe는 Windows에서 빌드해야 만들어집니다 (아래 빌드).

사용법: ① 가상 케이블(VB-Cable 등) 설치 → ② Windows 소리 설정에서 **기본 출력을 가상 케이블**로 → ③ 앱에서 **출력 장치를 실제 헤드폰**으로 선택 → ④ 시작.
(기본 출력과 같은 장치를 고르면 소리가 두 번 나서 앱이 막습니다.)

## 구조
- `app/main.cpp` : Windows GUI (WASAPI 루프백 캡처 -> 엔진 -> 헤드폰 출력, 슬라이더/프리셋/자동 저장)
- `core/dsp.h` : 실시간 DSP 엔진 (header-only, 오디오 스레드에서 할당/락 없음)
  - 저음 분리(중앙 유지) → M/S → 대사 중앙 고정 → 올패스 디코릴레이션(뒤쪽 앰비언스)
  - 5개 가상 오브젝트(앞L/R, 중앙, 뒤L/R)를 간이 바이노럴(ITD + 고음 감쇠)로 렌더링
  - 초기 반사음 + 리버브, 입체감 슬라이더(스무딩), 리미터
- `core/storage.h` : 설정/프리셋 저장 (key=value 텍스트, 원자적 쓰기, 값 검증, 이름 검증)
  - Windows `%APPDATA%\PlaySound\settings.ini`, `presets\*.ps`
  - 내장 프리셋: cinema / concert / immersive
- `cli/main.cpp` : WAV 파일로 엔진 테스트 (10ms 블록 처리로 실시간 모사)

## 빌드
**방법 1 (VS 설치됨):** x64 Native Tools Command Prompt에서 `build.bat` -> `PlaySound.exe` 생성
**방법 2 (설치 없이):** GitHub에 올리면 `.github/workflows/build.yml`이 자동 빌드 -> Actions 탭에서 PlaySound.exe 다운로드
**방법 3 (MinGW):** `g++ -std=c++17 -O2 -DUNICODE -D_UNICODE -static -mwindows app/main.cpp -o PlaySound.exe -lole32 -luser32 -lcomctl32 -lgdi32 -luuid`

CLI 테스트:
    cmake -B build && cmake --build build
    ./build/ps_cli demo demo.wav
    ./build/ps_cli process demo.wav out.wav --preset immersive

## 다음 단계
1. 간이 바이노럴 → SOFA HRTF 컨볼루션 (`BinauralSource`만 교체, libmysofa)
2. WASAPI 루프백 데모 앱 (JUCE 또는 Win32) - `Engine::process`를 그대로 호출
3. Equalizer APO 구조 참고해 APO DLL로 이식 (`Engine::process`가 IO 콜백 안에서 호출됨)
4. 컨트롤 앱 ↔ APO 설정 전달: `Store`가 쓴 파일을 APO가 읽거나 공유 메모리 사용
5. 코드 서명, 설치 프로그램, 장치별 호환성 테스트

## 알아둘 점
- 이 HRTF는 구면 머리 모델 근사라 개인차/정밀 정위는 약함. SOFA 도입 후 체감이 크게 올라감.
- 헤드폰 전용 효과. 스피커에서는 의미 없음.
- **Windows 앱(app/main.cpp)은 컴파일/실행 테스트를 못 했음.** 코어(dsp/storage/CLI)만 Linux에서 검증. 첫 빌드에서 오류가 나면 메시지를 알려주세요.
- 이 방식은 가상 케이블이 필요하고 지연이 조금 있음. 설치만으로 자동 적용되는 APO 방식은 이후 단계.
