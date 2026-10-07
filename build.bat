@echo off
setlocal enabledelayedexpansion

set "SCRIPT_DIR=%~dp0"
if "%SCRIPT_DIR:~-1%"=="\" set "SCRIPT_DIR=%SCRIPT_DIR:~0,-1%"
pushd "%SCRIPT_DIR%"

:: -----------------------------------------------------------------------------
:: Parse Arguments
:: Syntax: build.bat [-c|--clean] [TARGET] [toolchain_path]
:: -----------------------------------------------------------------------------
set "CHOSEN_TARGET="
set "CUSTOM_TOOLCHAIN="
set "CLEAN_BUILD=0"
set "INSTRUMENTED_BUILD=0"

for %%A in ("%~1" "%~2" "%~3" "%~4") do (
    if not "%%~A"=="" (
        if /i "%%~A"=="-c" set "CLEAN_BUILD=1"
        if /i "%%~A"=="--clean" set "CLEAN_BUILD=1"
        if /i "%%~A"=="-i" set "INSTRUMENTED_BUILD=1"
        if /i "%%~A"=="--instrumented" set "INSTRUMENTED_BUILD=1"
        if /i "%%~A"=="-h" goto :show_help
        if /i "%%~A"=="--help" goto :show_help
        if /i "%%~A"=="/?" goto :show_help

        if /i "%%~A"=="host" (
            set "CHOSEN_TARGET=host"
        ) else if /i "%%~A"=="mingw64" (
            set "CHOSEN_TARGET=mingw64"
        ) else if /i "%%~A"=="windows" (
            set "CHOSEN_TARGET=mingw64"
        ) else if /i "%%~A"=="win" (
            set "CHOSEN_TARGET=mingw64"
        ) else if /i "%%~A"=="linux" (
            set "CHOSEN_TARGET=linux"
        ) else if /i "%%~A"=="posix" (
            set "CHOSEN_TARGET=linux"
        ) else if /i "%%~A"=="arm" (
            set "CHOSEN_TARGET=arm"
        ) else if /i "%%~A"=="all" (
            set "CHOSEN_TARGET=all"
        ) else if /i "%%~A"=="cortex-m0" (
            set "CHOSEN_TARGET=cortex-m0"
        ) else if /i "%%~A"=="m0" (
            set "CHOSEN_TARGET=cortex-m0"
        ) else if /i "%%~A"=="cortex-m0plus" (
            set "CHOSEN_TARGET=cortex-m0plus"
        ) else if /i "%%~A"=="cortex-m0+" (
            set "CHOSEN_TARGET=cortex-m0plus"
        ) else if /i "%%~A"=="m0plus" (
            set "CHOSEN_TARGET=cortex-m0plus"
        ) else if /i "%%~A"=="m0+" (
            set "CHOSEN_TARGET=cortex-m0plus"
        ) else if /i "%%~A"=="cortex-m3" (
            set "CHOSEN_TARGET=cortex-m3"
        ) else if /i "%%~A"=="m3" (
            set "CHOSEN_TARGET=cortex-m3"
        ) else if /i "%%~A"=="cortex-m4" (
            set "CHOSEN_TARGET=cortex-m4"
        ) else if /i "%%~A"=="m4" (
            set "CHOSEN_TARGET=cortex-m4"
        ) else if /i "%%~A"=="cortex-m7" (
            set "CHOSEN_TARGET=cortex-m7"
        ) else if /i "%%~A"=="m7" (
            set "CHOSEN_TARGET=cortex-m7"
        ) else if /i "%%~A"=="cortex-m23" (
            set "CHOSEN_TARGET=cortex-m23"
        ) else if /i "%%~A"=="m23" (
            set "CHOSEN_TARGET=cortex-m23"
        ) else if /i "%%~A"=="cortex-m33" (
            set "CHOSEN_TARGET=cortex-m33"
        ) else if /i "%%~A"=="m33" (
            set "CHOSEN_TARGET=cortex-m33"
        ) else if /i "%%~A"=="cortex-m55" (
            set "CHOSEN_TARGET=cortex-m55"
        ) else if /i "%%~A"=="m55" (
            set "CHOSEN_TARGET=cortex-m55"
        ) else if /i "%%~A"=="riscv" (
            set "CHOSEN_TARGET=riscv"
        ) else if /i "%%~A"=="rv32i" (
            set "CHOSEN_TARGET=rv32i"
        ) else if /i "%%~A"=="rv32imc" (
            set "CHOSEN_TARGET=rv32imc"
        ) else if /i "%%~A"=="rv32imac" (
            set "CHOSEN_TARGET=rv32imac"
        ) else if /i "%%~A"=="rv32imafc" (
            set "CHOSEN_TARGET=rv32imafc"
        ) else if exist "%%~A\bin\gcc.exe" (
            set "CUSTOM_TOOLCHAIN=%%~A\bin"
        ) else if exist "%%~A\gcc.exe" (
            set "CUSTOM_TOOLCHAIN=%%~A"
        ) else if exist "%%~A\bin\arm-none-eabi-gcc.exe" (
            set "CUSTOM_TOOLCHAIN=%%~A\bin"
        ) else if exist "%%~A\arm-none-eabi-gcc.exe" (
            set "CUSTOM_TOOLCHAIN=%%~A"
        ) else if exist "%%~A\bin\riscv-none-elf-gcc.exe" (
            set "CUSTOM_TOOLCHAIN=%%~A\bin"
        ) else if exist "%%~A\riscv-none-elf-gcc.exe" (
            set "CUSTOM_TOOLCHAIN=%%~A"
        )
    )
)

if not defined CHOSEN_TARGET (
    set "CHOSEN_TARGET=all"
)

if "%CLEAN_BUILD%"=="1" (
    call :clean_outputs
    if errorlevel 1 (
        popd
        endlocal
        exit /b 1
    )
)

:: -----------------------------------------------------------------------------
:: Common Paths, Directories, and Source Sets
:: -----------------------------------------------------------------------------
set "CORE_SRCS=src\sertos_task.c src\sertos_scheduler.c src\sertos_stats.c src\sertos_sem.c src\sertos_mutex.c src\sertos_queue.c src\sertos_stream_buffer.c src\sertos_timer.c"
set "MODULE_SRCS=modules\bitmap\bitmap.c modules\crc\crc.c modules\fsm\fsm.c modules\linked_list\linked_list.c modules\memory_pool\memory_pool.c modules\ring_buffer\ring_buffer.c"
set "INCLUDES=-Iinc -Iport -Imodules\atomic -Imodules\ring_buffer -Imodules\memory_pool -Imodules\linked_list -Imodules\bitmap -Imodules\crc -Imodules\fsm"

set "BUILD_FAIL=0"

:: -----------------------------------------------------------------------------
:: Execute Target Builds
:: -----------------------------------------------------------------------------
if "%CHOSEN_TARGET%"=="host" (
    call :build_host_target mingw64
    call :build_linux_wsl
) else if "%CHOSEN_TARGET%"=="mingw64" (
    call :build_host_target mingw64
) else if "%CHOSEN_TARGET%"=="linux" (
    call :build_linux_wsl
) else if "%CHOSEN_TARGET%"=="arm" (
    call :build_arm_all
) else if "%CHOSEN_TARGET%"=="cortex-m0" (
    call :build_arm_single cortex-m0
) else if "%CHOSEN_TARGET%"=="cortex-m0plus" (
    call :build_arm_single cortex-m0plus
) else if "%CHOSEN_TARGET%"=="cortex-m3" (
    call :build_arm_single cortex-m3
) else if "%CHOSEN_TARGET%"=="cortex-m4" (
    call :build_arm_single cortex-m4
) else if "%CHOSEN_TARGET%"=="cortex-m7" (
    call :build_arm_single cortex-m7
) else if "%CHOSEN_TARGET%"=="cortex-m23" (
    call :build_arm_single cortex-m23
) else if "%CHOSEN_TARGET%"=="cortex-m33" (
    call :build_arm_single cortex-m33
) else if "%CHOSEN_TARGET%"=="cortex-m55" (
    call :build_arm_single cortex-m55
) else if "%CHOSEN_TARGET%"=="riscv" (
    call :build_riscv_single rv32i    rv32i_zicsr    ilp32
    call :build_riscv_single rv32imc  rv32imc_zicsr  ilp32
    call :build_riscv_single rv32imac rv32imac_zicsr ilp32
    call :build_riscv_single rv32imafc rv32imafc_zicsr ilp32f
) else if "%CHOSEN_TARGET%"=="rv32i" (
    call :build_riscv_single rv32i    rv32i_zicsr    ilp32
) else if "%CHOSEN_TARGET%"=="rv32imc" (
    call :build_riscv_single rv32imc  rv32imc_zicsr  ilp32
) else if "%CHOSEN_TARGET%"=="rv32imac" (
    call :build_riscv_single rv32imac rv32imac_zicsr ilp32
) else if "%CHOSEN_TARGET%"=="rv32imafc" (
    call :build_riscv_single rv32imafc rv32imafc_zicsr ilp32f
) else if "%CHOSEN_TARGET%"=="all" (
    call :build_host_target mingw64
    call :build_linux_wsl
    call :build_arm_all
    call :build_riscv_single rv32i    rv32i_zicsr    ilp32
    call :build_riscv_single rv32imc  rv32imc_zicsr  ilp32
    call :build_riscv_single rv32imac rv32imac_zicsr ilp32
    call :build_riscv_single rv32imafc rv32imafc_zicsr ilp32f
)

echo.
echo ============================================================
if "!BUILD_FAIL!"=="0" (
    echo [SUCCESS] Build completed successfully
) else (
    echo [ERROR] Build completed with errors
)
echo ============================================================

if "!BUILD_FAIL!"=="0" (
    popd
    endlocal
    exit /b 0
) else (
    popd
    endlocal
    exit /b 1
)

:: -----------------------------------------------------------------------------
:: Subroutine: Build Native Linux Target Through WSL
:: -----------------------------------------------------------------------------
:build_linux_wsl
where wsl.exe >nul 2>nul
if errorlevel 1 (
    echo [ERROR] WSL was not found. Install and initialize WSL to build the linux target.
    set "BUILD_FAIL=1"
    goto :eof
)

echo.
echo ============================================================
echo [BUILD] Compiling SertOS for native Linux through WSL...
echo [SOURCE] %SCRIPT_DIR%
echo [TOOLCHAIN] WSL native Linux (gcc, ar, size)
echo ============================================================

if "%INSTRUMENTED_BUILD%"=="1" (
    wsl.exe --cd "%SCRIPT_DIR%" -- bash ./build.sh -i
) else (
    wsl.exe --cd "%SCRIPT_DIR%" -- bash ./build.sh
)
if errorlevel 1 (
    echo [ERROR] Native Linux build failed in WSL.
    set "BUILD_FAIL=1"
    goto :eof
)

echo [SUCCESS] Native Linux build completed.
goto :eof

:: -----------------------------------------------------------------------------
:: Subroutine: Build MinGW Windows Host Target
:: -----------------------------------------------------------------------------
:build_host_target
set "HOST_TARGET=%~1"
set "HOST_TOOLCHAIN="

if defined CUSTOM_TOOLCHAIN (
    if exist "%CUSTOM_TOOLCHAIN%\gcc.exe" set "HOST_TOOLCHAIN=%CUSTOM_TOOLCHAIN%"
)

if not defined HOST_TOOLCHAIN (
    if exist "C:\toolchains\mingw64\13.2.0\bin\gcc.exe" (
        set "HOST_TOOLCHAIN=C:\toolchains\mingw64\13.2.0\bin"
    )
)

if not defined HOST_TOOLCHAIN (
    where gcc.exe >nul 2>nul
    if not errorlevel 1 (
        for /f "delims=" %%I in ('where gcc.exe') do (
            if not defined HOST_TOOLCHAIN set "HOST_TOOLCHAIN=%%~dpI"
        )
    )
)

if not defined HOST_TOOLCHAIN (
    echo [ERROR] MinGW / Host GCC toolchain not found!
    set "BUILD_FAIL=1"
    goto :eof
)

if "%HOST_TOOLCHAIN:~-1%"=="\" set "HOST_TOOLCHAIN=%HOST_TOOLCHAIN:~0,-1%"

set "HOST_CC=%HOST_TOOLCHAIN%\gcc.exe"
set "HOST_AR=%HOST_TOOLCHAIN%\ar.exe"
set "HOST_SIZE=%HOST_TOOLCHAIN%\size.exe"

if not exist "%HOST_AR%" (
    echo [ERROR] Invalid MinGW toolchain: ar.exe was not found in "%HOST_TOOLCHAIN%".
    set "BUILD_FAIL=1"
    goto :eof
)
if not exist "%HOST_SIZE%" (
    echo [ERROR] Invalid MinGW toolchain: size.exe was not found in "%HOST_TOOLCHAIN%".
    set "BUILD_FAIL=1"
    goto :eof
)

set "PATH=%HOST_TOOLCHAIN%;%PATH%"

if not exist "%HOST_TOOLCHAIN%\..\libexec\gcc" (
    echo [ERROR] Invalid MinGW toolchain: GCC runtime directory was not found.
    echo         Expected under "%HOST_TOOLCHAIN%\..\libexec\gcc".
    set "BUILD_FAIL=1"
    goto :eof
)
if not exist "%HOST_TOOLCHAIN%\libwinpthread-1.dll" (
    echo [ERROR] Invalid MinGW toolchain: libwinpthread-1.dll was not found in "%HOST_TOOLCHAIN%".
    set "BUILD_FAIL=1"
    goto :eof
) 
if not exist "%HOST_TOOLCHAIN%\..\lib\gcc" (
    echo [ERROR] Invalid MinGW toolchain: GCC library directory was not found.
    echo         Expected under "%HOST_TOOLCHAIN%\..\lib\gcc".
    set "BUILD_FAIL=1"
    goto :eof
)
if not exist "%HOST_CC%" (
    echo [ERROR] Invalid MinGW toolchain: gcc.exe is not executable.
    set "BUILD_FAIL=1"
    goto :eof
)

set "LIB_OUT=lib\%HOST_TARGET%"
if not exist "%LIB_OUT%" mkdir "%LIB_OUT%"
set "OBJ_DIR=build\%HOST_TARGET%"
if not exist "%OBJ_DIR%" mkdir "%OBJ_DIR%"

if "%INSTRUMENTED_BUILD%"=="1" (
    set "HOST_LIB=%LIB_OUT%\libsertos_%HOST_TARGET%_instrumented.a"
    set "EXTRA_CORE_FLAGS=-finstrument-functions"
    set "BUILD_DESC=%HOST_TARGET% host architecture (Instrumented)"
) else (
    set "HOST_LIB=%LIB_OUT%\libsertos_%HOST_TARGET%.a"
    set "EXTRA_CORE_FLAGS="
    set "BUILD_DESC=%HOST_TARGET% host architecture"
)

echo.
echo ============================================================
echo [BUILD] Compiling SertOS for !BUILD_DESC!...
echo [TOOLCHAIN] %HOST_TOOLCHAIN%
echo ============================================================

set "PORT_SRC=port\windows\port_windows.c"
set "OBJS="

for %%S in (%CORE_SRCS%) do (
    set "OBJ_FILE=%OBJ_DIR%\%%~nS.o"
    set "OBJS=!OBJS! "!OBJ_FILE!""

    echo [COMPILE] %%S
    "%HOST_CC%" -O2 -Wall -Wextra -pedantic -std=c99 !EXTRA_CORE_FLAGS! %INCLUDES% -c "%%S" -o "!OBJ_FILE!"
    if !ERRORLEVEL! neq 0 (
        echo [ERROR] Compiler failed while compiling %%S. See the diagnostic above.
        set "BUILD_FAIL=1"
        goto :eof
    )
)

for %%S in (%MODULE_SRCS% %PORT_SRC%) do (
    set "OBJ_FILE=%OBJ_DIR%\%%~nS.o"
    set "OBJS=!OBJS! "!OBJ_FILE!""

    echo [COMPILE] %%S
    "%HOST_CC%" -O2 -Wall -Wextra -pedantic -std=c99 %INCLUDES% -c "%%S" -o "!OBJ_FILE!"
    if !ERRORLEVEL! neq 0 (
        echo [ERROR] Compiler failed while compiling %%S. See the diagnostic above.
        set "BUILD_FAIL=1"
        goto :eof
    )
)

"%HOST_AR%" rcs "%HOST_LIB%" %OBJS%
if !ERRORLEVEL! neq 0 (
    echo [ERROR] Failed creating archive %HOST_LIB%
    set "BUILD_FAIL=1"
    goto :eof
)

echo [SUCCESS] Generated: %HOST_LIB%
if exist "%HOST_SIZE%" (
    call :print_archive_size "%HOST_SIZE%" "%HOST_LIB%"
)
goto :eof

:: -----------------------------------------------------------------------------
:: Subroutine: Build All ARM Targets
:: -----------------------------------------------------------------------------
:build_arm_all
for %%T in (cortex-m0 cortex-m0plus cortex-m3 cortex-m4 cortex-m7 cortex-m23 cortex-m33 cortex-m55) do (
    call :build_arm_single %%T
)
goto :eof

:: -----------------------------------------------------------------------------
:: Subroutine: Build Single ARM Target
:: -----------------------------------------------------------------------------
:build_arm_single
set "ARM_TARGET=%~1"
set "ARM_TOOLCHAIN="

if defined CUSTOM_TOOLCHAIN (
    if exist "%CUSTOM_TOOLCHAIN%\arm-none-eabi-gcc.exe" set "ARM_TOOLCHAIN=%CUSTOM_TOOLCHAIN%"
)

if not defined ARM_TOOLCHAIN (
    if exist "C:\toolchains\arm\13.2.1\bin\arm-none-eabi-gcc.exe" (
        set "ARM_TOOLCHAIN=C:\toolchains\arm\13.2.1\bin"
    )
)

if not defined ARM_TOOLCHAIN (
    where arm-none-eabi-gcc.exe >nul 2>nul
    if not errorlevel 1 (
        for /f "delims=" %%I in ('where arm-none-eabi-gcc.exe') do (
            if not defined ARM_TOOLCHAIN set "ARM_TOOLCHAIN=%%~dpI"
        )
    )
)

if not defined ARM_TOOLCHAIN (
    echo [ERROR] GNU Arm Embedded Toolchain not found!
    set "BUILD_FAIL=1"
    goto :eof
)

if "%ARM_TOOLCHAIN:~-1%"=="\" set "ARM_TOOLCHAIN=%ARM_TOOLCHAIN:~0,-1%"

set "ARM_CC=%ARM_TOOLCHAIN%\arm-none-eabi-gcc.exe"
set "ARM_AR=%ARM_TOOLCHAIN%\arm-none-eabi-ar.exe"
set "ARM_SIZE=%ARM_TOOLCHAIN%\arm-none-eabi-size.exe"

set "LIB_OUT=lib\arm"
if not exist "%LIB_OUT%" mkdir "%LIB_OUT%"
set "OBJ_DIR=build\arm\%ARM_TARGET%"
if not exist "%OBJ_DIR%" mkdir "%OBJ_DIR%"

if "%ARM_TARGET%"=="cortex-m0" (
    set "ARCH_FLAGS=-mcpu=cortex-m0 -mthumb"
    set "LIB_NAME=libsertos_cortex_m0.a"
) else if "%ARM_TARGET%"=="cortex-m0plus" (
    set "ARCH_FLAGS=-mcpu=cortex-m0plus -mthumb"
    set "LIB_NAME=libsertos_cortex_m0plus.a"
) else if "%ARM_TARGET%"=="cortex-m3" (
    set "ARCH_FLAGS=-mcpu=cortex-m3 -mthumb"
    set "LIB_NAME=libsertos_cortex_m3.a"
) else if "%ARM_TARGET%"=="cortex-m4" (
    set "ARCH_FLAGS=-mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard"
    set "LIB_NAME=libsertos_cortex_m4.a"
) else if "%ARM_TARGET%"=="cortex-m7" (
    set "ARCH_FLAGS=-mcpu=cortex-m7 -mthumb -mfpu=fpv5-d16 -mfloat-abi=hard"
    set "LIB_NAME=libsertos_cortex_m7.a"
) else if "%ARM_TARGET%"=="cortex-m23" (
    set "ARCH_FLAGS=-mcpu=cortex-m23 -mthumb"
    set "LIB_NAME=libsertos_cortex_m23.a"
) else if "%ARM_TARGET%"=="cortex-m33" (
    set "ARCH_FLAGS=-mcpu=cortex-m33 -mthumb -mfpu=fpv5-sp-d16 -mfloat-abi=hard"
    set "LIB_NAME=libsertos_cortex_m33.a"
) else if "%ARM_TARGET%"=="cortex-m55" (
    set "ARCH_FLAGS=-mcpu=cortex-m55 -mthumb -mfpu=fpv5-d16 -mfloat-abi=hard"
    set "LIB_NAME=libsertos_cortex_m55.a"
)

if "%INSTRUMENTED_BUILD%"=="1" (
    set "ARM_LIB=%LIB_OUT%\%LIB_NAME:~0,-2%_instrumented.a"
    set "EXTRA_CORE_FLAGS=-finstrument-functions"
    set "BUILD_DESC=%ARM_TARGET% (Instrumented)"
) else (
    set "ARM_LIB=%LIB_OUT%\%LIB_NAME%"
    set "EXTRA_CORE_FLAGS="
    set "BUILD_DESC=%ARM_TARGET%"
)

echo.
echo ============================================================
echo [BUILD] Compiling SertOS for !BUILD_DESC!...
echo [TOOLCHAIN] %ARM_TOOLCHAIN%
echo ============================================================

set "PORT_DIR=port\arm\%ARM_TARGET%"
set "PORT_SRCS=%PORT_DIR%\port_cpu.c %PORT_DIR%\port_context.s"
set "OBJS="

for %%S in (%CORE_SRCS%) do (
    set "OBJ_FILE=%OBJ_DIR%\%%~nS.o"
    set "OBJS=!OBJS! "!OBJ_FILE!""

    "%ARM_CC%" !ARCH_FLAGS! -Os -ffreestanding -ffunction-sections -fdata-sections -Wall -Wextra -pedantic -std=c99 !EXTRA_CORE_FLAGS! %INCLUDES% -c "%%S" -o "!OBJ_FILE!"
    if !ERRORLEVEL! neq 0 (
        echo [ERROR] Failed compiling %%S for %ARM_TARGET%
        set "BUILD_FAIL=1"
        goto :eof
    )
)

for %%S in (%MODULE_SRCS% %PORT_SRCS%) do (
    set "OBJ_FILE=%OBJ_DIR%\%%~nS.o"
    set "OBJS=!OBJS! "!OBJ_FILE!""

    "%ARM_CC%" !ARCH_FLAGS! -Os -ffreestanding -ffunction-sections -fdata-sections -Wall -Wextra -pedantic -std=c99 %INCLUDES% -c "%%S" -o "!OBJ_FILE!"
    if !ERRORLEVEL! neq 0 (
        echo [ERROR] Failed compiling %%S for %ARM_TARGET%
        set "BUILD_FAIL=1"
        goto :eof
    )
)

"%ARM_AR%" rcs "%ARM_LIB%" %OBJS%
if !ERRORLEVEL! neq 0 (
    echo [ERROR] Failed creating archive %ARM_LIB%
    set "BUILD_FAIL=1"
    goto :eof
)

echo [SUCCESS] Generated: %ARM_LIB%
if exist "%ARM_SIZE%" (
    call :print_archive_size "%ARM_SIZE%" "%ARM_LIB%"
)
goto :eof

:: -----------------------------------------------------------------------------
:: Subroutine: Build RISC-V Single Target
:: Usage: call :build_riscv_single <profile> <march> <mabi>
::   profile = rv32i | rv32imc | rv32imac | rv32imafc
::   march   = rv32i_zicsr | rv32imc_zicsr | rv32imac_zicsr | rv32imafc_zicsr
::   mabi    = ilp32 | ilp32f
:: -----------------------------------------------------------------------------
:build_riscv_single
set "RISCV_PROFILE=%~1"
set "RISCV_MARCH=%~2"
set "RISCV_MABI=%~3"
set "RISCV_TOOLCHAIN="

if defined CUSTOM_TOOLCHAIN (
    if exist "%CUSTOM_TOOLCHAIN%\riscv-none-elf-gcc.exe" set "RISCV_TOOLCHAIN=%CUSTOM_TOOLCHAIN%"
)

if not defined RISCV_TOOLCHAIN (
    if exist "C:\toolchains\riscv\13.2.0\bin\riscv-none-elf-gcc.exe" (
        set "RISCV_TOOLCHAIN=C:\toolchains\riscv\13.2.0\bin"
    )
)

if not defined RISCV_TOOLCHAIN (
    where riscv-none-elf-gcc.exe >nul 2>nul
    if not errorlevel 1 (
        for /f "delims=" %%I in ('where riscv-none-elf-gcc.exe') do (
            if not defined RISCV_TOOLCHAIN set "RISCV_TOOLCHAIN=%%~dpI"
        )
    )
)

if not defined RISCV_TOOLCHAIN (
    echo [ERROR] GNU RISC-V Embedded Toolchain not found!
    set "BUILD_FAIL=1"
    goto :eof
)

if "%RISCV_TOOLCHAIN:~-1%"=="\" set "RISCV_TOOLCHAIN=%RISCV_TOOLCHAIN:~0,-1%"

set "RISCV_CC=%RISCV_TOOLCHAIN%\riscv-none-elf-gcc.exe"
set "RISCV_AR=%RISCV_TOOLCHAIN%\riscv-none-elf-ar.exe"
set "RISCV_SIZE=%RISCV_TOOLCHAIN%\riscv-none-elf-size.exe"

set "LIB_OUT=lib\riscv"
if not exist "%LIB_OUT%" mkdir "%LIB_OUT%"
set "OBJ_DIR=build\riscv\%RISCV_PROFILE%"
if not exist "%OBJ_DIR%" mkdir "%OBJ_DIR%"

set "ARCH_FLAGS=-march=%RISCV_MARCH% -mabi=%RISCV_MABI%"
if "%INSTRUMENTED_BUILD%"=="1" (
    set "RISCV_LIB=%LIB_OUT%\libsertos_%RISCV_PROFILE%_instrumented.a"
    set "EXTRA_CORE_FLAGS=-finstrument-functions"
    set "BUILD_DESC=RISC-V %RISCV_MARCH% (%RISCV_MABI%) (Instrumented)"
) else (
    set "RISCV_LIB=%LIB_OUT%\libsertos_%RISCV_PROFILE%.a"
    set "EXTRA_CORE_FLAGS="
    set "BUILD_DESC=RISC-V %RISCV_MARCH% (%RISCV_MABI%)"
)

echo.
echo ============================================================
echo [BUILD] Compiling SertOS for !BUILD_DESC!...
echo [TOOLCHAIN] %RISCV_TOOLCHAIN%
echo ============================================================

set "PORT_DIR=port\riscv"
set "PORT_SRCS=%PORT_DIR%\port_cpu.c %PORT_DIR%\port_context.S"
set "OBJS="

for %%S in (%CORE_SRCS%) do (
    set "OBJ_FILE=%OBJ_DIR%\%%~nS.o"
    set "OBJS=!OBJS! !OBJ_FILE!"
    "%RISCV_CC%" %ARCH_FLAGS% -O2 -Wall -Wextra -std=c99 -ffunction-sections -fdata-sections !EXTRA_CORE_FLAGS! %INCLUDES% -c "%%S" -o "!OBJ_FILE!"
    if !ERRORLEVEL! neq 0 (
        echo [ERROR] Compilation failed: %%S
        set "BUILD_FAIL=1"
        goto :eof
    )
)

for %%S in (%MODULE_SRCS% %PORT_SRCS%) do (
    set "OBJ_FILE=%OBJ_DIR%\%%~nS.o"
    set "OBJS=!OBJS! !OBJ_FILE!"
    "%RISCV_CC%" %ARCH_FLAGS% -O2 -Wall -Wextra -std=c99 -ffunction-sections -fdata-sections %INCLUDES% -c "%%S" -o "!OBJ_FILE!"
    if !ERRORLEVEL! neq 0 (
        echo [ERROR] Compilation failed: %%S
        set "BUILD_FAIL=1"
        goto :eof
    )
)

"%RISCV_AR%" rcs "%RISCV_LIB%" %OBJS%
if !ERRORLEVEL! neq 0 (
    echo [ERROR] Failed creating archive %RISCV_LIB%
    set "BUILD_FAIL=1"
    goto :eof
)

echo [SUCCESS] Generated: %RISCV_LIB%
if exist "%RISCV_SIZE%" (
    call :print_archive_size "%RISCV_SIZE%" "%RISCV_LIB%"
)
goto :eof

:: -----------------------------------------------------------------------------
:: Subroutine: Print Aggregate Static Library Size
:: -----------------------------------------------------------------------------
:print_archive_size
set "SIZE_TOOL=%~1"
set "SIZE_ARCHIVE=%~2"
for /f "tokens=1-5" %%A in ('call "%SIZE_TOOL%" -t "%SIZE_ARCHIVE%" ^| findstr /C:"TOTALS"') do (
    echo .text %%A
    echo .data %%B
    echo .bss  %%C
    echo Total %%D
)
goto :eof

:: -----------------------------------------------------------------------------
:: Help Menu
:: -----------------------------------------------------------------------------
:show_help
echo.
echo Usage: build.bat [-c^|--clean] [-i^|--instrumented] [TARGET] [TOOLCHAIN_PATH]
echo.
echo Options:
echo   -c, --clean         Remove generated build and library outputs before building
echo   -i, --instrumented  Build the instrumented static library variant (-finstrument-functions)
echo.
echo Targets:
echo   all         Build host, ARM Cortex, and all 4 RISC-V libraries (default)
echo   mingw64     Build MinGW-w64 host static library (lib\mingw64\libsertos_mingw64.a) [aliases: windows, win]
echo   linux       Build POSIX host static library    (lib\posix\libsertos_posix.a)   [alias: posix]
echo   arm         Build all 8 ARM Cortex libraries    (lib\arm\libsertos_cortex_*.a)
echo   riscv       Build all 4 RISC-V libraries        (lib\riscv\libsertos_rv32*.a)
echo   rv32i       Build RISC-V RV32I baseline         (lib\riscv\libsertos_rv32i.a)       ilp32
echo   rv32imc     Build RISC-V RV32IMC                (lib\riscv\libsertos_rv32imc.a)     ilp32  ^(ESP32-C3, GD32VF103^)
echo   rv32imac    Build RISC-V RV32IMAC               (lib\riscv\libsertos_rv32imac.a)    ilp32  ^(RP2350, ESP32-C6, FE310^)
echo   rv32imafc   Build RISC-V RV32IMAFC ^(hard-FPU^)  (lib\riscv\libsertos_rv32imafc.a)  ilp32f ^(ESP32-P4, CH32V307^)
echo   m0          Build Cortex-M0 library             (lib\arm\libsertos_cortex_m0.a)
echo   m0plus/m0+  Build Cortex-M0+ library            (lib\arm\libsertos_cortex_m0plus.a)
echo   m3          Build Cortex-M3 library             (lib\arm\libsertos_cortex_m3.a)
echo   m4          Build Cortex-M4 library             (lib\arm\libsertos_cortex_m4.a)
echo   m7          Build Cortex-M7 library             (lib\arm\libsertos_cortex_m7.a)
echo   m23         Build Cortex-M23 library            (lib\arm\libsertos_cortex_m23.a)
echo   m33         Build Cortex-M33 library            (lib\arm\libsertos_cortex_m33.a)
echo   m55         Build Cortex-M55 library            (lib\arm\libsertos_cortex_m55.a)
echo.
echo Examples:
echo   build.bat
echo   build.bat mingw64
echo   build.bat arm
echo   build.bat riscv
echo   build.bat rv32imac
echo   build.bat rv32imafc
echo   build.bat m4
echo   build.bat --clean m33
echo   build.bat riscv   C:\toolchains\riscv\13.2.0\bin
echo   build.bat arm     C:\toolchains\arm\13.2.1\bin
echo.
popd
endlocal
exit /b 0

:clean_outputs
echo [CLEAN] Removing generated build and library outputs...
if exist "build" (
    rmdir /s /q "build"
    if exist "build" (
        echo [ERROR] Could not remove the build directory.
        exit /b 1
    )
)
if exist "lib" (
    rmdir /s /q "lib"
    if exist "lib" (
        echo [ERROR] Could not remove the library output directory.
        exit /b 1
    )
)
exit /b 0
