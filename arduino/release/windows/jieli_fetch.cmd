@echo off
if defined SMCPAD_FETCH_DEBUG echo on
rem The JieLi compiler for Windows, fetched into this folder the first time a sketch is built
rem (the Arduino platform runs this before every build; afterwards it returns at once).
rem It comes from JieLi's own server, checked against the SHA-256 below, and only its pi32
rem folder is taken out of the installer: no installation, nothing outside this folder, no
rem CodeBlocks. We may not host it ourselves: it carries no license.
setlocal
set "HERE=%~dp0"
if exist "%HERE%pi32\bin\clang.exe" exit /b 0

set "URL=https://jl-update.oss-cn-shenzhen.aliyuncs.com/2.5.3.exe"
set "SHA=399bd518a28e47a7a0394506cebd5bf658748a58cd6b7c02512e24b4b88c5a5e"
set "SETUP=%HERE%jieli-2.5.3.exe"

echo SMC-PAD: first build on this computer: fetching the JieLi compiler from JieLi (52 MB)...
curl.exe -sfL -o "%SETUP%" "%URL%"
if errorlevel 1 (
    echo SMC-PAD: could not download %URL%
    exit /b 1
)
set "GOT="
for /f "delims=" %%h in ('certutil -hashfile "%SETUP%" SHA256 ^| findstr /v ":"') do set "GOT=%%h"
set "GOT=%GOT: =%"
if /i not "%GOT%"=="%SHA%" (
    echo SMC-PAD: the downloaded file is not the expected one ^(SHA-256 %GOT%^)
    del "%SETUP%"
    exit /b 1
)
"%HERE%innoextract.exe" -s -I "C$/JL/pi32" -d "%HERE%x" "%SETUP%"
if errorlevel 1 exit /b 1
move "%HERE%x\C$\JL\pi32" "%HERE%pi32" >nul
rmdir /s /q "%HERE%x"
del "%SETUP%"
echo SMC-PAD: JieLi compiler ready.
exit /b 0
