#ifndef MIITUNE_STORAGE_H
#define MIITUNE_STORAGE_H

#include <string>

// Config/data files in wiiu/apps/MiiTune/ (console SD card, Cemu folder, or cwd)
void StorageInit();                                        // creates the folders (call at startup)
std::string StorageRead(const char* name);                 // "" if missing
bool StorageWrite(const char* name, const std::string& data);

#endif
