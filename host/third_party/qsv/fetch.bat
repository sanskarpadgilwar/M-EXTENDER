@echo off
rem Fetches the oneVPL C header set (api/vpl) used by the QSV encoder.
rem The headers are MIT-licensed and compiled against directly; the runtime
rem (libmfxhw64.dll / libmfx64.dll / vpl.dll) is loaded dynamically at runtime,
rem so no SDK install is needed.

set "REPO=https://github.com/oneapi-src/oneVPL.git"
set "DEST=api\vpl"

if exist "%DEST%\mfx.h" (
    echo oneVPL headers already present: %DEST%
    goto :eof
)

where git >nul 2>nul || (
    echo git not found on PATH. Install git and retry.
    exit /b 1
)

if exist onevpl-src rmdir /s /q onevpl-src
git clone --depth 1 --branch main %REPO% onevpl-src || exit /b 1
if not exist "onevpl-src\api\vpl\mfx.h" (
    echo oneVPL api/vpl not found in the clone. Repository layout may have changed.
    exit /b 1
)

mkdir "%DEST%" >nul 2>nul
xcopy "onevpl-src\api\vpl" "%DEST%" /e /i /y >nul || exit /b 1
rmdir /s /q onevpl-src

echo Done. Headers in %DEST%.
