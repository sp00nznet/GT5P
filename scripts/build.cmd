@echo off
REM Configure + build with clang-cl + Ninja inside a VS x64 environment.
REM Usage: scripts\build.cmd [extra cmake args]
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
cmake -S "%~dp0.." -B "%~dp0..\build" -G Ninja ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_C_COMPILER="C:/Program Files/LLVM/bin/clang-cl.exe" ^
  -DCMAKE_CXX_COMPILER="C:/Program Files/LLVM/bin/clang-cl.exe" %* || exit /b 1
cmake --build "%~dp0..\build" || exit /b 1
