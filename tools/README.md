# Introduction
Here are a few tools I have made through the use of ChatGPT. No warranties whatsover regarding use.

# flagid.c

This is a small tool which takes the "flagid" provided to a control code in the event script (found in EV-files) and converts it to the specific bit and memory location where it will be stored. For example a unique flag provided to a control code may be "0108", this is translated to a specific memory bit which the control code turns on or off. In this way you can read the script and find interesting flag ids, and then find the memory locations to set breakpoints/watchpoints on during interactive debugging. Or you can find out which memory bits you need to turn on/off to pass flag checks. The program basically reimplements the function for checking flagids in _000PRG.DAT.
