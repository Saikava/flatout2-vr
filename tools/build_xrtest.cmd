@echo off
rem Builds tools\xrtest.exe (32-bit) next to a copy of the OpenXR loader.
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul || exit /b 1
set ROOT=%~dp0..
if not exist "%ROOT%\build\xrtest" mkdir "%ROOT%\build\xrtest"
cl /nologo /O2 /MT /EHa /W3 /I"%ROOT%\third_party\openxr\include" "%~dp0xrtest.cpp" /Fo"%ROOT%\build\xrtest\\" /Fe"%ROOT%\build\xrtest\xrtest.exe" /link "%ROOT%\third_party\openxr\lib\openxr_loader.lib" || exit /b 1
copy /y "%ROOT%\third_party\openxr\bin\openxr_loader.dll" "%ROOT%\build\xrtest\" >nul
