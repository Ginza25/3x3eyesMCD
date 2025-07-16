# Welcome
Welcome to my notes and small tools archive for the Sega Mega CD game Seima Densetsu 3x3 Eyes (聖魔伝説 サザンアイズ). The game is an quite ambitious RPG made by Sega themselves, based on a manga by Yuzo Takada (高田裕三). It has lots of Sega in-jokes and references, an interesting setting and more adult themes - which make it worth investigating in my opinion! 

A table file has been made, and the 8x16 font system expanded from supporting numbers to the full ASCII character set. A future goal would be a full translation, but this would require quite a bit of teamwork.

![Screenshot 2](SEGA_3X3EYES_002.jpg?raw=true "Screenshot 2")

# RAM cart menu translation

セ一フ Save	りネーム Rename		切替 Exchange

ロ一ド Load	削除 Delete		終了 End

新ファイル New file
上書き- Overwrite

# Cheats

These apply to the Bizhawk emulator. Adjust memory in the Hex Editor within the Mega CD 2M memory.

Horizontal position 034008-03009 (two bytes)

Vertical position  03400c-03400d (two bytes)

Random encounter rate 03f250 (two bytes). Freeze at 0x00 for no random encounters.

Character 1 HP 0x036204

Character 1 MP 0x036208

Character 1 Max HP 0x036213

Money 0x03620C

Boss HP 0x036C04
