# Introduction
The Event files first have a block of **event opcodes** which are run if an event occurs, probably according to an event index number, such as talking to a person or attempting to open a door. These event opcodes are typically located at 0xC800 in the EV-files. Event opcodes seem to run functions for moving characters around, panning the screen and printing text etc. One (or more) event opcode can point to an entry in the text block, typically located at 0x14800. The text block starts with a pointer table to each of the entries. Each entry in the text block begins with a separate set of **text opcodes**. Text opcodes include testing flags and selecting alternative dialogue options etc. Each text entry may (or may not) include an opcode to print text data (such as 0x24). The print text data function includes separate **text control codes**, relating more to the actual presentation of text/portraits etc., which are handled by the text processing engine.

Text opcodes include references to offsets. For this reason if the location of text opcodes within the text data block is moved around or expanded, all following text opcodes with offset references must be updated. It is not enough to update the preceding pointer table pointing to the start of each text entry. It is therefore necessary to analyse the many text opcodes and find which ones contain offsets vs. other parameters (flags to check etc.). Text opcodes which do not reference offsets, can probably be left untouched, at least for translation purposes.

There are in total 82 event opcodes (according to the jump table in _000PRG.DAT at 0x1cd1e (0x23f1e in memory)) and 121 text opcodes (according to the jump table in _000PRG.DAT at 0x1b7ce (0x229ce in memory)). However, several of the text opcodes overlap, e.g. with the only difference being whether to continue processing the next opcode or to stop.

# Event opcodes

## 0x1C Setup actor

First following byte is the actor ID, it seems. Bytes 6 and 7 specify the text block index to associate with talking to this actor. Runs on loading an area, not every time you talk to an actor.

## 0x1F Setup actor II

Setup actor with some differences, less commonly used. Bytes 7 and 8 specify the text block index to associate with talking to this actor. Runs on loading an area, not every time you talk to an actor.

## 0x24 Wait before processing next opcode

Immediately following byte specifies time to wait.

## 0x27 Set flag

Immediately following two byte flag is set.

## 0x2A Request text

Request an entry in the dialogue text block according to an immediately following two byte index, and then starts processing dialogue opcodes for that entry. The two bytes at 0x14800 specify the location of the first two byte pointer pair in this index. Used in cutscenes, where there is no player interaction with NPCs.

## 0x35 Move actor sprite

First byte selects which actor to manipulate. The 4 bytes after opcode 0x35 are two signed 16-bit words: X velocity and Y velocity, in big-endian, stored into the actor’s velocity fields as (value << 8) (i.e., 8.8 fixed-point in a 32-bit slot).

## 0x37 Change actor sprite

Change actor sprite. First byte selects which actor to manipulate. 0x00 is Yakumo typically, 0x01 Pai, and so on. 0x05 is Aguri in Hong Kong. Second byte unknown. Third byte seems to be direction actor is facing.

# Text opcodes

## 0x01 Return

Ends processing of text opcodes. Some text opcodes automatically set the return flag, otherwise a 0x01 opcode is needed.

## 0x06 Process text offset if flag true

Format is 0x06 XX XX YY YY, where XX XX is the flag to check and YY YY is the offset with text data to process if the flag is true. If the flag is not true, the byte following YY YY is processed as a text opcode. In theory this could be 0x01 to stop processing text opcodes. In practice it often seems to be a 0x24 opcode to print a default text if the flag is not set. Example: 06 0108 00D2 24 00AE, checks if Ling Ling is present (flag 0108), if so, process text data at offset 00D2, otherwise opcode 0x24 will process text data at offset 00AE and return.
 
## 0x0A Test flag

Test flag immediately following, e.g. 0108 (is Ling Ling present). Store result in be008 (0x00 if fail, 0x01 if true).

## 0x0B Test flag AND

Check for additional flag immediately following. Store success 0x01 in be008 if previous condition in be008 was true and current condition is also true/0x01.

## 0x12 Process text offset if flag(s) not true

Process text data starting at two byte text offset immediately following, if flag(s) are not yet set. Typically combined with a preceding 0x0A (and 0x0B if two flags) text opcode.

## 0x13 Process text offset if flag(s) true

Process text data starting at two byte text offset immediately following, if flag(s) are already set. Typically combined with a preceding 0x0A (and 0x0B if two flags) text opcode.

## 0x1A Process text offset, set flag and return

Process text data starting at two byte text offset immediately following, then sets flag defined by next two bytes and stops processing text opcodes.

For example 1a 0a16 0118: starts text processing at offset 0a16, then sets flag 0118 and ends processing text opcodes.

This text opcode is used a lot - typically if you need to talk to a number of NPCs in an area to set flags and proceed in the game, but there is no specific feedback from the NPC. For example you need to talk to a shopkeeper to set a flag, but his dialogue is always the same.

**0x3E** is the same text opcode as **0x1A**, only it does not return, but continues processing text opcodes.

## 0x24 Process text offset and return

Processes text data starting at two byte text offset immediately following and then stops processing text opcodes.

**0x4C** is the same script command as **0x24**, only it does not return, but continues processing text opcodes.

# Text control codes

Text control codes are arguments to the text processing function/engine and are more related to the presentation than conditions for printing text etc. (handled by **text opcodes**). They are evaluated if a text opcode to run the process text data function is run first, such as 0x1A or 0x24.

## 0xE0 Show portrait
The **E0** control code takes a single byte argument, observed from 0x00 to 0x0B, which indicates a character portrait to print by a text box. The portraits available depend on the current EV-file, where up to 5 portraits are stored Kosinski-compressed, and additional PLAY-files, where additional portraits may be stored.

## 0xE1 Hide portrait
To hide the portrait, the **E1** control code is used.

## 0xE7F9 Pause text printing

Pause text printing according to a two byte argument. Observed values from 0x0018 to 0x0220. This is used to synch up text printing with spoken audio.

## 0xE7FE Text printing speed

Sets the speed to print text based on a single byte argument. Observed values from 0x00 to 0xA0. This is used to synch up text printing with spoken audio.
