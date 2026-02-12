_extflash_:
ifeq ($(program_files_path),)
	$(error $(error_msg))
endif
	@cd "$(st_stm32cube_programmer_path)" && ./$(stm32cube_signingtool_filename) -s -bin "$(application_path)/$(binary_output_path)/intflash.bin" -nk -of 0x80000000 -t fsbl -o "$(application_path)/$(binary_output_path)/STM32N6570-DK_Appli-trusted.bin" -hv 2.3 -align -dump "$(application_path)/$(binary_output_path)/STM32N6570-DK_Appli-trusted.bin"
	@cd "$(st_stm32cube_programmer_path)" && ./$(stm32cube_programmer_filename) -c port=SWD mode=HOTPLUG AP=1 -el "$(stm32cube_loader_relative_path)" -d "$(application_path)/$(binary_output_path)/STM32N6570-DK_Appli-trusted.bin" 0x70100000
	@cd "$(st_stm32cube_programmer_path)" && ./$(stm32cube_programmer_filename) -c port=SWD mode=HOTPLUG AP=1 -el "$(stm32cube_loader_relative_path)" -d "$(application_path)/$(binary_output_path)/assets.hex"
_intflash_:
ifeq ($(program_files_path),)
	$(error $(error_msg))
endif
	@cd "$(st_stm32cube_programmer_path)" && ./$(stm32cube_signingtool_filename) -s -bin "$(application_path)/$(binary_output_path)/intflash.bin" -nk -of 0x80000000 -t fsbl -o "$(application_path)/$(binary_output_path)/STM32N6570-DK_Appli-trusted.bin" -hv 2.3 -align -dump "$(application_path)/$(binary_output_path)/STM32N6570-DK_Appli-trusted.bin"
	@cd "$(st_stm32cube_programmer_path)" && ./$(stm32cube_programmer_filename) -c port=SWD mode=HOTPLUG AP=1 -el "$(stm32cube_loader_relative_path)" -d "$(application_path)/$(binary_output_path)/STM32N6570-DK_Appli-trusted.bin" 0x70100000
_fsblflash_:
ifeq ($(program_files_path),)
	$(error $(error_msg))
endif
	@cd "$(st_stm32cube_programmer_path)" && ./$(stm32cube_signingtool_filename) -s -bin "$(application_path)/$(binary_output_path)/target.bin" -nk -of 0x80000000 -t fsbl -o "$(application_path)/$(binary_output_path)/STM32N6570-DK_FSBL-trusted.bin" -hv 2.3 -align -dump "$(application_path)/$(binary_output_path)/STM32N6570-DK_FSBL-trusted.bin"
	@cd "$(st_stm32cube_programmer_path)" && ./$(stm32cube_programmer_filename) -c port=SWD mode=HOTPLUG AP=1 -el "$(stm32cube_loader_relative_path)" -d "$(application_path)/$(binary_output_path)/STM32N6570-DK_FSBL-trusted.bin" 0x70000000