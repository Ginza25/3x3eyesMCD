# EV files

EV files (Event?) contain specific sprites, event data, text and portraits for a script.

The file starts with sprite data from 0x0 to some point before 0xc800. At 0xc800 an event type script data is stored. This includes for example different actions to take depending on if some flags have been set when attempting to open a door. Some times text data is stored here instead. There seems to be at least two variations of the EV files - with text data either at 0xc800 or 0x14800. If no text data at 0xc800, then text data is stored at 0x14800. In that case portraits to use with the text are stored Kosinski-compressed, with a preceding header at 0x1b000, 0x1bd00, 0x1ca00, 0x1d700 and 0x1e400 and an attached tilemap and palette.

The portrait tilemap has primary two byte code attached which specifies which sets of bytes are compressed with XOR delta compression and which are uncompressed. Then each set of 8 bytes has additional two byte control code which specifies which tiles are repeated (00) or direct copy (10) or flipped/rotated (11 - not researched further at this point).

The game CD contains 310 EV files, however for some reason several are duplicates and only 231 are unique.

## Story order of EV files

EV000A0.DAT Benares intro

EV000FD.DAT Yakumo meeting Pai in bar in Tokyo

EV00006.DAT Hong Kong

EV00017.DAT Yougekisha

EV00012.DAT Kowloon
