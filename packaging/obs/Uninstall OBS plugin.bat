rem PhoneCam: removes phonecam-obs.dll from OBS Studio (asks for admin rights)
@echo off
chcp 65001 >nul
net session >nul 2>&1
if %errorlevel% neq 0 (
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)
set "OBSDIR="
for /f "usebackq delims=" %%a in (`powershell -NoProfile -Command "(Get-ItemProperty 'HKLM:\SOFTWARE\OBS Studio' -ErrorAction SilentlyContinue).'(default)'"`) do set "OBSDIR=%%a"
if not defined OBSDIR set "OBSDIR=%ProgramFiles%\obs-studio"
if not exist "%OBSDIR%\bin\64bit\obs64.exe" (
    echo OBS Studio not found in "%OBSDIR%".
    echo Не нашёл OBS Studio / Не знайшов OBS Studio.
    echo Copy phonecam-obs.dll manually to obs-studio\obs-plugins\64bit\
    pause
    exit /b 1
)
tasklist /fi "imagename eq obs64.exe" | find /i "obs64.exe" >nul && (
    echo Close OBS and run this file again.
    echo Закрой OBS и запусти этот файл ещё раз.
    echo Закрий OBS і запусти цей файл ще раз.
    pause
    exit /b 1
)
if not exist "%OBSDIR%\obs-plugins\64bit\phonecam-obs.dll" (
    echo The plugin is not installed. / Плагин не установлен. / Плагін не встановлено.
    pause
    exit /b 0
)
del /f /q "%OBSDIR%\obs-plugins\64bit\phonecam-obs.dll" || (
    echo Could not remove the plugin. / Не удалось удалить плагин. / Не вдалося видалити плагін.
    pause
    exit /b 1
)
echo.
echo Done! The PhoneCam plugin was removed from OBS.
echo Готово! Плагин PhoneCam удалён из OBS.
echo Готово! Плагін PhoneCam видалено з OBS.
pause
