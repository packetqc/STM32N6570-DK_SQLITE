@echo off
set input=%1

REM set device = ""

REM if %input% == 1 set device = "004C00493234510E37333934" 
REM if %input% == 2 set device = "002100233234510E37333934"


setlocal

set "CUBEPROGRAMMER_TOOL_PATH=C:\Program Files\STMicroelectronics\STM32Cube\STM32CubeProgrammer\bin"
set "BIN_DIR=%~pd0..\..\..\STM32CubeIDE\Appli\Debug\"

:: Navigate to the binarys's directory
pushd "%BIN_DIR%"

:: Sign the app 
"%CUBEPROGRAMMER_TOOL_PATH%\STM32_SigningTool_CLI.exe" -s -bin "%BIN_DIR%STM32N6570-DK_Appli.bin" -nk -of 0x80000000 -t fsbl -o "%BIN_DIR%Project-trusted.bin" -hv 2.3 -align -dump "%BIN_DIR%Project-trusted.bin"

:: Load the binary

if %input% == "" "%CUBEPROGRAMMER_TOOL_PATH%\STM32_Programmer_CLI.exe" -c port=SWD mode=HOTPLUG AP=1 -el "%CUBEPROGRAMMER_TOOL_PATH%\ExternalLoader\MX66UW1G45G_STM32N6570-DK.stldr" -d "%BIN_DIR%Project-trusted.bin" 0x70100000

if %input% == 1 "%CUBEPROGRAMMER_TOOL_PATH%\STM32_Programmer_CLI.exe" -c port=SWD index=0 mode=HOTPLUG AP=1 -el "%CUBEPROGRAMMER_TOOL_PATH%\ExternalLoader\MX66UW1G45G_STM32N6570-DK.stldr" -d "%BIN_DIR%Project-trusted.bin" 0x70100000

if %input% == 2 "%CUBEPROGRAMMER_TOOL_PATH%\STM32_Programmer_CLI.exe" -c port=SWD index=1 mode=HOTPLUG AP=1 -el "%CUBEPROGRAMMER_TOOL_PATH%\ExternalLoader\MX66UW1G45G_STM32N6570-DK.stldr" -d "%BIN_DIR%Project-trusted.bin" 0x70100000


:: Return to the original directory
popd

:: Pause the script to view any messages
pause
