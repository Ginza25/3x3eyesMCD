# Introduction
Here are a few tools I have made through the use of ChatGPT. No warranties whatsover regarding use.

# flagid.c

This is a small tool which takes the "flagid" provided to a control code in the event script (found in EV-files) and converts it to the specific bit and memory location where it will be stored. For example a unique flag provided to a control code may be "0108", this is translated to a specific memory bit which the control code turns on or off. In this way you can read the script and find interesting flag ids, and then find the memory locations to set breakpoints/watchpoints on during interactive debugging. Or you can find out which memory bits you need to turn on/off to pass flag checks. The program basically reimplements the function for checking flagids in _000PRG.DAT.

# readopcodes.c

This is a small program which reads the text opcode block of an input EV/event file (usually found at 0x14800), according to an overview defined in a CSV-file. It interprets the opcodes according to the opcodes.yml file. Flags are interpreted according to the flag.tbl and text is interpreted according to the text table specified in the CSV file for each EV-file.
