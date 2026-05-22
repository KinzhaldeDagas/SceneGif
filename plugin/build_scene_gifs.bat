@echo off
setlocal
set OUT_DIR=%~dp0build
if not exist "%OUT_DIR%" mkdir "%OUT_DIR%"
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat" >nul
cl /nologo /std:c++20 /EHsc /O2 /MD /LD "%~dp0SceneGIFs.cpp" /Fe:"%OUT_DIR%\SceneGIFs.dll" /link /NOLOGO /MACHINE:X86 /SUBSYSTEM:WINDOWS /DEF:"%~dp0SceneGIFs.def" ole32.lib oleaut32.lib uuid.lib windowscodecs.lib d3d9.lib
