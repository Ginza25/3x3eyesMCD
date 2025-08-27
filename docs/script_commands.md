# Introduction
The game has several script commands. One type of script command is to print text, and such commands take several separate text control codes. For each pointer table preceding a script data block, e.g. typically located at 0x14800 in EV-files, the pointer first points to a **script command** within the script data block, which then may (or may not) include a command to print text data. The text data may contain separate **text control codes**, which are handled by the text processing engine.

There are in total 121 script commands according to the jump table in _000PRG.DAT at 0x1b7ce (0x229ce in memory). However, several of these overlap, e.g. with the only difference being whether to continue processing the next script command or to stop.

Script commands include references to offsets. For this reason if script data within the script data block (such as text data) is moved around or expanded, all following script commands with offset references must be updated. It is not enough to update the preceding pointer table pointing to each script command. Otherwise the script commands will fail as the offset reference is no longer valid. It is therefore necessary to analyse the many script commands and find which ones contain offsets vs. other parameters (flags to check etc.). Script commands which do not reference offsets, can probably be left untouched, at least for translation purposes.

# Script commands

## 0x01 Return

Ends processing of script commands. Some script commands automatically set the return flag, otherwise a 0x01 command is needed.

## 0x06 Process text offset if flag true

Format is 0x06 XX XX YY YY, where XX XX is the flag to check and YY YY is the offset with text data to process if the flag is true. If the flag is not true, the byte following YY YY is processed as a script command. In theory this could be 0x01 to stop processing script commandd. In practice it often seems to be a 0x24 script command to print a default text if the flag is not set. Example: 06 0108 00D2 24 00AE, checks if Ling Ling is present (flag 0108), if so, process text data at offset 00D2, otherwise script command 24 will process text data at offset 00AE and return.
 
## 0x0A Test flag

Test flag immediately following, e.g. 0108 (is Ling Ling present). Store result in be008 (0x00 if fail, 0x01 if true).

## 0x0B Test flag AND

Check for additional flag immediately following. Store success 0x01 in be008 if previous condition in be008 was true and current condition is also true/0x01.

## 0x12 Process text offset if flag(s) not true

Process text data starting at two byte text offset immediately following, if flag(s) are not yet set. Typically combined with a preceding 0x0A (and 0x0B if two flags) script command.

## 0x13 Process text offset if flag(s) true

Process text data starting at two byte text offset immediately following, if flag(s) are already set. Typically combined with a preceding 0x0A (and 0x0B if two flags) script command.

## 0x1A Process text offset, set flag and return

Process text data starting at two byte text offset immediately following, then sets flag defined by next two bytes and stops processing control codes.

For example 1a 0a16 0118: starts text processing at offset 0a16, then sets flag 0118 and ends processing control codes.

This script command is used a lot - typically if you need to talk to a number of NPCs in an area to set flags and proceed in the game, but there is no specific feedback from the NPC. For example you need to talk to a shopkeeper to set a flag, but his dialogue is always the same.

**0x3E** is the same script command as **0x1A**, only it does not return, but continues processing script commands.

## 0x24 Process text offset and return

Processes text data starting at two byte text offset immediately following and then stops processing control codes.

**0x4C** is the same script command as **0x24**, only it does not return, but continues processing script commands.

# Text control codes

Text control codes are arguments to the text processing function/engine
They are evaluated if a script command to run the process text data function is run first, such as 0x1A or 0x24.

## 0xE0 Show portrait
The **E0** control code takes a single byte argument, observed from 0x00 to 0x0B, which indicates a character portrait to print by a text box. The portraits available depend on the current EV-file, where up to 5 portraits are stored Kosinski-compressed, and additional PLAY-files, where additional portraits may be stored.

## 0xE1 Hide portrait
To hide the portrait, the **E1** control code is used.

## 0xE7F9 Pause text printing

Pause text printing according to a two byte argument. Observed values from 0x0018 to 0x0220. This is used to synch up text printing with spoken audio.

## 0xE7FE Text printing speed

Sets the speed to print text based on a single byte argument. Observed values from 0x00 to 0xA0. This is used to synch up text printing with spoken audio.
