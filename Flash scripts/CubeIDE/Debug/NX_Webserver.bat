
cd D:\STM32N6\Nx_WebServer\STM32CubeIDE\FSBL\Debug
"c:\Program Files\STMicroelectronics\STM32Cube\STM32CubeProgrammer\bin\STM32_SigningTool_CLI.exe" -s -bin Nx_WebServer_FSBL.bin -nk -of 0x80000000 -t fsbl -o Nx_WebServer-trusted.bin -hv 2.3 -dump "Nx_WebServer-trusted.bin" -align
"c:\Program Files\STMicroelectronics\STM32Cube\STM32CubeProgrammer\bin\STM32_Programmer_CLI.exe" -c port=SWD mode=HOTPLUG AP=1 -el "c:\Program Files\STMicroelectronics\STM32Cube\STM32CubeProgrammer\bin\ExternalLoader\MX66UW1G45G_STM32N6570-DK.stldr" -d "Nx_WebServer-trusted.bin" 0x70000000
