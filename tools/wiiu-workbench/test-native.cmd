@echo off
setlocal
set VSCMD_SKIP_SENDTELEMETRY=1
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
if not exist ".local\wiiu\workbench\native-tests" mkdir ".local\wiiu\workbench\native-tests"
cl /nologo /std:c++17 /EHsc /W4 /Zi /fsanitize=address tools\wiiu-workbench\test_native.cpp /Fo:.local\wiiu\workbench\native-tests\test_native.obj /Fd:.local\wiiu\workbench\native-tests\compiler.pdb /Fe:.local\wiiu\workbench\native-tests\test_native.exe /link /INCREMENTAL:NO
if errorlevel 1 exit /b 1
.local\wiiu\workbench\native-tests\test_native.exe
exit /b %errorlevel%
