@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul || exit /b 1
set ROOT=%~dp0..
cl /nologo /O2 /MT /W3 /D_CRT_SECURE_NO_WARNINGS /I"%ROOT%\third_party\openxr\include" /I"%ROOT%\third_party\vulkan\include" "%~dp0xrvkreq.cpp" /Fo"%ROOT%\build\xrtest\\" /Fe"%ROOT%\build\xrtest\xrvkreq.exe" /link "%ROOT%\third_party\openxr\lib\openxr_loader.lib"
