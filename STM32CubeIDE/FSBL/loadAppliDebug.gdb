#set temporary breakpoint with commands to load appli symbols after ext flash init
tbreak JumpToApplication
commands
add-symbol-file ..\\Appli\\Debug\\STM32N6570-DK_Appli.elf
continue
end