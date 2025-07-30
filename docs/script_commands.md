# Introduction
The game has at least two types of script commands/text control codes. For each pointer table preceding a text block, e.g. typically located at 0x14800 in EV-files, the pointer first points to a **script command** within the text block, which then may (or may not) point to actual text data. The text data may contain separate **text control codes**. Script commands contain references to offsets, and are placed within the text block. For this reason it is not enough for changes to the text data block to update the preceding pointer table offsets. The inline script command references to specific offsets must also be updated, otherwise the script commands will fail. It is therefore necessary to analyse the many script commands and find which ones contain offsets vs. other parameters (flags to check etc.). Other script command parameters than offsets can probably be left untouched, at least for translation purposes.

# Script commands

## 0x01 End processing

Ends processing of script commands. Some script commands automatically end, otherwise a 0x01 command is needed.

## 0x06 Jump to offset if flag true

Format is 0x06 XX XX YY YY, where XX XX is the flag to check and YY YY is the offset to jump to if the flag is true. If the jump is not made, the immediately following byte is processed as a script command. In theory this could be 0x01 to stop processing. In practice it often seems to be a 0x24 script command to print a default text if the flag is not set. Example: 06 0108 00D2 24 00AE, checks if Ling Ling is present (flag 0108), if so, jump to offset 00D2, otherwise script command 24 will jump to offset 00AE.
 
## 0x0A Test flag

Test flag immediately following, e.g. 0108 (is Ling Ling present). Store result in be008 (0x00 if fail, 0x01 if true).

## 0x0B Test flag AND

Check for additional flag immediately following. Store success 0x01 in be008 if previous condition in be008 was true and current condition is also true/0x01.

## 0x12 Jump to offset if flag(s) not true

Jumps to immediately following two byte offset, if flag(s) are not yet set. Typically combined with a preceding 0x0A (and 0x0B if two flags) script command.

## 0x13 Jump to offset if flag(s) true

Jumps to immediately following two byte offset, if flag(s) are already set. Typically combined with a preceding 0x0A (and 0x0B if two flags) script command.
