@echo off
REM ============================================================
REM  EnduranceTest_Batch.bat
REM  BEFORE RUNNING: Update SERVER_IP to your server PC's IP
REM  Runs FOREVER until you manually close it (Ctrl+C or close window)
REM ============================================================
SET "SERVER_IP=127.0.0.1"
SET "SERVER_PORT=5000"
SET "TELEM_FILE=katl-kefd-B737-700.txt"

SET /A "index = 1"
SET /A "count = 100"

:while
@echo %time%
     :spawnloop
     if %index% leq %count% (
          START /MIN Client.exe %SERVER_IP% %SERVER_PORT% %TELEM_FILE%
          SET /A index = %index% + 1
          @echo %index%
          goto :spawnloop
          )
     timeout 250 > NUL
     SET /A index = 1
     goto :while
