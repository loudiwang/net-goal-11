@echo off
setlocal
call "%~dp0..\..\build\vcvars32.bat"
if errorlevel 1 exit /b 1
cd /d "%~dp0"
if not exist "..\..\dist\x86" mkdir "..\..\dist\x86"
cl /nologo /LD /O2 /MT /EHsc /std:c++17 /W4 /D_USRDLL /D_WINDLL /D_CRT_SECURE_NO_WARNINGS /D_WINSOCK_DEPRECATED_NO_WARNINGS hook.cpp ^
  /Fo:"..\..\dist\x86\\" /Fe:"..\..\dist\x86\Net Goal 11.Hook.dll" ^
  /link /MACHINE:X86 ws2_32.lib user32.lib
if errorlevel 1 exit /b 1
echo Built ..\..\dist\x86\Net Goal 11.Hook.dll
