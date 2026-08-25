@echo off
setlocal EnableExtensions

set "PROJECT_DIR=%~dp0"
set "BASH_CMD="

if defined BASH_EXE if exist "%BASH_EXE%" set "BASH_CMD=%BASH_EXE%"
if not defined BASH_CMD if exist "C:\MinGW\msys\1.0\bin\bash.exe" set "BASH_CMD=C:\MinGW\msys\1.0\bin\bash.exe"
if not defined BASH_CMD if exist "C:\msys64\usr\bin\bash.exe" set "BASH_CMD=C:\msys64\usr\bin\bash.exe"

if not defined BASH_CMD (
  echo Cannot find bash. Set BASH_EXE or install MSYS2/MSYS.
  exit /b 1
)

pushd "%PROJECT_DIR%"
"%BASH_CMD%" ./build.sh %*
set "BUILD_RESULT=%ERRORLEVEL%"
popd

exit /b %BUILD_RESULT%
