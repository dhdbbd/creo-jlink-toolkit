@echo off
setlocal enabledelayedexpansion
rem ============================================================
rem  run_server.bat  --  start J-Link -> Creo HTTP service
rem  Requires (see README.md):
rem    - a JRE 11+  (set JAVA_HOME, or put java.exe on PATH,
rem                  or drop a Temurin 11 JRE into jlink\runtime\)
rem    - pfcasync.jar + otk.jar from your own Creo install,
rem      copied into jlink\lib\
rem    - Creo Parametric 9 installed locally with a valid license
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

rem ---- locate a JRE 11+: bundled runtime first, then JAVA_HOME, then PATH ----
set "JAVA="
if exist "%PKG%\runtime\bin\java.exe" set "JAVA=%PKG%\runtime\bin\java.exe"
if not defined JAVA if defined JAVA_HOME if exist "%JAVA_HOME%\bin\java.exe" set "JAVA=%JAVA_HOME%\bin\java.exe"
if not defined JAVA (
  for /f "delims=" %%j in ('where java.exe 2^>nul') do if not defined JAVA set "JAVA=%%j"
)
if not defined JAVA (
  echo [ERROR] no java.exe found. Install JRE 11+, set JAVA_HOME, or
  echo         put a JRE into %PKG%\runtime\
  exit /b 1
)

set "COMMON=%CREO_HOME%\Common Files"
set "CREOLIB=%COMMON%\x86e_win64\lib"
set "CREOOBJ=%COMMON%\x86e_win64\obj"
set "LOGDIR=%PKG%\service\logs"
if not exist "%LOGDIR%" mkdir "%LOGDIR%"

if not exist "%PKG%\lib\pfcasync.jar" (
  echo [ERROR] missing %PKG%\lib\pfcasync.jar
  echo         copy pfcasync.jar and otk.jar from your Creo install:
  echo         ^<Creo^>\Common Files\text\java\
  exit /b 1
)
if not exist "%CREO_HOME%" (
  echo [ERROR] CREO_HOME not found: %CREO_HOME%  ^(edit run_server.bat^)
  exit /b 1
)

rem ---- PATH: bundled native FIRST (target lacks pfcasyncmt.dll),
rem      then Creo own lib (supplies the rest of DLL deps), then JDK ----
set "PATH=%PKG%\native;%CREOLIB%;%PATH%"

rem ---- J-Link / Creo required env ----
set "PRO_COMM_MSG_EXE=%CREOOBJ%\pro_comm_msg.exe"
set "PRO_DIRECTORY=%COMMON%"
set "PRO_MACHINE_TYPE=x86e_win64"
set "CREOAPP_DIRECTORY=%CREO_HOME%\Parametric"
set "CREOPMA_FEATURE_NAME=__FEATURE_NAME__"
set "PVIEW_PROEPVIEW_HOME=%COMMON%\apps\prodview"
set "MECH_LP=%COMMON%\applications\simulate"
set "MECH_HOME=%MECH_LP%\x86e_win64"

rem ---- service self config ----
set "LOG_DIR=%LOGDIR%"

echo [run_server] JLinkServer -> http://localhost:%PORT%
echo [run_server] java=%JAVA%
echo [run_server] CREO_HOME=%CREO_HOME%
echo [run_server] logs=%LOGDIR%

rem classpath order is CRITICAL: pfcasync.jar BEFORE otk.jar
"%JAVA%" -Dfile.encoding=UTF-8 ^
  -Djava.library.path="%PKG%\native;%CREOLIB%" ^
  -cp "%PKG%\lib\pfcasync.jar;%PKG%\service\build;%PKG%\lib\otk.jar" ^
  com.jlink.service.JLinkServer %PORT%

endlocal
