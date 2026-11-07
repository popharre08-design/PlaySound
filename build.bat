@echo off
REM Visual Studio의 "x64 Native Tools Command Prompt"에서 실행하세요.
where cl >nul 2>&1 || (echo cl.exe 를 찾을 수 없습니다. VS Developer Command Prompt에서 실행하세요. & exit /b 1)
cl /nologo /EHsc /O2 /std:c++17 /utf-8 /DUNICODE /D_UNICODE app\main.cpp /Fe:PlaySound.exe /link /SUBSYSTEM:WINDOWS
if exist PlaySound.exe (echo. & echo 완료: PlaySound.exe) else (echo 빌드 실패)
