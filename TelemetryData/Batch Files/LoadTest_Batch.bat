@echo off
REM ============================================================
REM  LoadTest_Batch.bat
REM  BEFORE RUNNING: Update SERVER_IP to your server PC's IP
REM  For local testing use 127.0.0.1
REM  For LAN testing use the server's IPv4 from ipconfig
REM ============================================================
SET "SERVER_IP=127.0.0.1"
SET "SERVER_PORT=5000"
SET "TELEM_FILE=katl-kefd-B737-700.txt"

SET /A "index = 1"
SET /A "count = 100"

:while
if %index% leq %count% (
     START /MIN Client.exe %SERVER_IP% %SERVER_PORT% %TELEM_FILE%
     SET /A index = %index% + 1
     @echo %index%
     goto :while
)
