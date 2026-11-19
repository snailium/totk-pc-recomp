// Save data import and export, the CLI modes --save-export and --save-import (save_io.c). They run while the game
// is not running, on the committed save, so no transaction or working copy is in flight. The import finds the save
// inside whatever the player hands it - this port's own export, a console or JKSV dump, a Ryujinx or yuzu save
// directory - by the shape of the game's own folders, and backs the current save up before replacing it.
int save_export(const char *out);            // 0 on success; out: the folder to create or fill
int save_import(const char *in, int dry_run);   // 0 on success; in: a folder holding save data somewhere inside
