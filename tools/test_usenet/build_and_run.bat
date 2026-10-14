@echo off
rem Builds test_usenet.exe with a PC compiler - Visual Studio 2010's when it's
rem there, as test_inflate's does, else the newest Visual Studio's - and runs
rem it against the data make_test_data.py made: the folder given, or .\data.
rem
rem   python make_test_data.py
rem   python make_par2_data.py C:\path\to\par2.exe   (optional: the par2 repair test)
rem   build_and_run.bat
setlocal
cd /d "%~dp0"
if not exist build mkdir build
set "VS2010=%ProgramFiles(x86)%\Microsoft Visual Studio 10.0\VC"
if exist "%VS2010%\bin\cl.exe" goto vs2010
rem Through a file, not for /f: the "(x86)" in the path ends a for's brackets.
"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath > build\vs.txt
set /p VSLATEST=<build\vs.txt
call "%VSLATEST%\VC\Auxiliary\Build\vcvars32.bat" >nul
goto build
:vs2010
call "%VS2010%\vcvarsall.bat" x86 >nul
:build
cl /nologo /O2 /EHsc /W3 /D_CRT_SECURE_NO_WARNINGS /Fobuild\ /Febuild\test_usenet.exe ^
    test_usenet.cpp ..\..\src\Yenc.cpp ..\..\src\Nzb.cpp ..\..\src\RarVolumes.cpp ..\..\src\RarHeaders.cpp ^
    ..\..\src\Md5Digest.cpp ..\..\src\Par2.cpp
if errorlevel 1 exit /b 1
if "%~1"=="" (build\test_usenet.exe data) else (build\test_usenet.exe "%~1")
