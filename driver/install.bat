@echo off
setlocal
rem Install the TwinScreen virtual display driver (test-signed).
rem Run as Administrator. See README.md first.
cd /d "%~dp0"

echo [1/4] Enabling test signing...
bcdedit /set testsigning on

echo [2/4] Staging runtime config...
if not exist "C:\ProgramData\TwinScreen" mkdir "C:\ProgramData\TwinScreen"
if exist "out\pkg\option.txt" (
    copy /y "out\pkg\option.txt" "C:\ProgramData\TwinScreen\option.txt" >nul
) else (
    if exist "option.txt" copy /y "option.txt" "C:\ProgramData\TwinScreen\option.txt" >nul
)

echo [3/4] Installing driver package...
pnputil /add-driver "out\pkg\TwinScreen.inf" /install

echo [4/4] Done.
echo.
echo Reboot for test-signing to take effect. After reboot, if the virtual
echo monitor is not present, install the INF manually:
echo   Device Manager ^> Action ^> Add legacy hardware ^> select from list
echo   ^> Display adapters ^> Have Disk ^> browse to out\pkg\TwinScreen.inf
echo Then open Display settings and set the TwinScreen Virtual Display to your
echo tablet's native resolution with "Extend these displays".
endlocal
