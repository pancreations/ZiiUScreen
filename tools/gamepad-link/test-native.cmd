@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b %errorlevel%
cd /d "%~dp0\..\.."
if not exist ".local\gamepad-link\tests" mkdir ".local\gamepad-link\tests"
cl /nologo /W4 /WX /EHsc /std:c++17 /fsanitize=address /Zi tools\gamepad-link\test_tx_format.cpp /Fo.local\gamepad-link\tests\test_tx_format.obj /Fe.local\gamepad-link\tests\test_tx_format.exe /Fd.local\gamepad-link\tests\test_tx_format.pdb
if errorlevel 1 exit /b %errorlevel%
.local\gamepad-link\tests\test_tx_format.exe
exit /b %errorlevel%
