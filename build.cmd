@echo off
rem Builds FO2VR.asi (32-bit, MSVC) and copies it into ..\scripts\.
rem Set VS to the Build Tools folder to override the auto-detection.
setlocal
if not defined VS call :findvs
if not defined VS (
  echo Visual Studio C++ Build Tools not found. Set VS to the install folder.
  exit /b 1
)
call "%VS%\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul || exit /b 1
set CMAKE=%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe
set NINJA=%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe
if not exist "%CMAKE%" set CMAKE=cmake
if not exist "%NINJA%" set NINJA=ninja
if not exist "%~dp0build\build.ninja" (
  "%CMAKE%" -S "%~dp0." -B "%~dp0build" -G Ninja -DCMAKE_MAKE_PROGRAM="%NINJA%" -DCMAKE_BUILD_TYPE=RelWithDebInfo || exit /b 1
)
"%CMAKE%" --build "%~dp0build" || exit /b 1
exit /b 0

:findvs
for %%D in (
  "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
  "C:\BuildTools"
  "C:\Program Files\Microsoft Visual Studio\2022\BuildTools"
  "C:\Program Files\Microsoft Visual Studio\2022\Community"
  "C:\Program Files\Microsoft Visual Studio\2022\Professional"
  "C:\Program Files\Microsoft Visual Studio\2022\Enterprise"
) do if not defined VS if exist "%%~D\VC\Tools\MSVC" set VS=%%~D
exit /b 0
