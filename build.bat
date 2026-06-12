@echo off
setlocal
if defined VSCMD_VER goto :build
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -latest -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH echo Visual Studio not found.& exit /b 1
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
:build
if not exist out mkdir out
cl /nologo /std:c++17 /O2 /EHsc /W4 /Fo:out\ /Fe:fastchm.exe src\lzx.cpp src\chmwriter.cpp src\sitemap.cpp src\fifti.cpp src\builder.cpp src\main.cpp
