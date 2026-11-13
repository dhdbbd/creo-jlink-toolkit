@echo off
rem Strip all BTK_* environment variables (names may contain colons; cmd set handles them)
for /f "tokens=1,* delims==" %%a in ('set BTK_') do set "%%a="
