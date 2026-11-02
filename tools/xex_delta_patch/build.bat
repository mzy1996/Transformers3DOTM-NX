@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2017\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (
  echo vcvars64 failed
  exit /b 1
)
cd /d "%~dp0"
cl /nologo /O2 /EHsc /DHAVE_CONFIG_H /I "src" /I "src\mspack" /I "src\crypto" src\main.cpp src\mspack\lzxd.c /Fe:xex_patch.exe
if errorlevel 1 (
  echo build failed
  exit /b 1
)
echo BUILD_OK