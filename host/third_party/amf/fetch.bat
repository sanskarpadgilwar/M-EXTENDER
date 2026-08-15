@echo off
rem Fetches the AMF headers into third_party/amf/include (used by CMake).
rem Headers are MIT-licensed and only used at compile time; the AMF runtime
rem (amfrt64.dll) ships with AMD drivers and is loaded dynamically.

setlocal
cd /d "%~dp0"

if exist "include\components\VideoEncoderVCE.h" (
    echo include/ already has the AMF headers.
    goto :eof
)

if not exist "repo" (
    echo Cloning GPUOpen-LibrariesAndSDKs/AMF...
    git clone --depth 1 https://github.com/GPUOpen-LibrariesAndSDKs/AMF.git repo
    if errorlevel 1 (
        echo Clone failed. Check git and your network connection.
        exit /b 1
    )
)

echo Copying headers...
if exist "include" rmdir /s /q "include"
mkdir "include"
xcopy "repo\amf\public\include\*" "include\" /e /i /q
if errorlevel 1 (
    echo Copy failed.
    exit /b 1
)

echo Done. AMF headers are in third_party/amf/include.
