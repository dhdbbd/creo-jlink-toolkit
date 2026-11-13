@echo off
setlocal enabledelayedexpansion
rem ============================================================
rem  run_server.bat  --  start J-Link -> Creo HTTP service
rem  Self-contained: uses bundled OpenJDK + J-Link jars + native dll
rem  Target machine only needs its OWN Creo 9.0.0.0 installed.
rem ============================================================

rem ---------- USER CONFIG: edit these 3 lines ----------
set "CREO_HOME=__CREO_HOME__"
set "PTC_D_LICENSE_FILE=__LICENSE_FILE__"
set "PORT=8080"
rem -----------------------------------------------------

rem Allow first CLI arg to override port: run_server.bat 9090
if not "%~1"=="" set "PORT=%~1"

set "PKG=%~dp0"
if "%PKG:~-1%"=="\" set "PKG=%PKG:~0,-1%"
set "JAVA=%PKG%\runtime\bin\java.exe"
set "COMMON=%CREO_HOME%\Common Files"
set "CREOLIB=%COMMON%\x86e_win64\lib"
set "CREOOBJ=%COMMON%\x86e_win64\obj"
set "LOGDIR=%PKG%\service\logs"
if not exist "%LOGDIR%" mkdir "%LOGDIR%"

if not exist "%JAVA%" (
  echo [ERROR] bundled java not found: %JAVA%
  exit /b 1
)
if not exist "%CREO_HOME%" (
  echo [ERROR] CREO_HOME not found: %CREO_HOME%  ^(edit run_server.bat^)
  exit /b 1
)

rem ---- PATH: bundled native FIRST (target lacks pfcasyncmt.dll),
rem      then Creo own lib (supplies the rest of DLL deps), then JDK ----
set "PATH=%PKG%\native;%CREOLIB%;%PKG%\runtime\bin;%PATH%"

rem ---- J-Link / Creo required env (same set that works on source PC) ----
set "PRO_COMM_MSG_EXE=%CREOOBJ%\pro_comm_msg.exe"
set "PRO_DIRECTORY=%COMMON%"
set "PRO_MACHINE_TYPE=x86e_win64"
set "CREOAPP_DIRECTORY=%CREO_HOME%\Parametric"
set "CREOPMA_FEATURE_NAME=__FEATURE_NAME__"
set "PVIEW_PROEPVIEW_HOME=%COMMON%\apps\prodview"
set "MECH_LP=%COMMON%\applications\simulate"
set "MECH_HOME=%MECH_LP%\x86e_win64"

rem ---- service self config ----
set "CREO_HOME=%CREO_HOME%"
set "LOG_DIR=%LOGDIR%"

echo [run_server] JLinkServer -> http://localhost:%PORT%
echo [run_server] CREO_HOME=%CREO_HOME%
echo [run_server] logs=%LOGDIR%

rem classpath order is CRITICAL: pfcasync.jar BEFORE otk.jar
"%JAVA%" -Dfile.encoding=UTF-8 ^
  -Djava.library.path="%PKG%\native;%CREOLIB%" ^
  -cp "%PKG%\lib\pfcasync.jar;%PKG%\service\build;%PKG%\lib\otk.jar" ^
  com.jlink.service.JLinkServer %PORT%

endlocal
