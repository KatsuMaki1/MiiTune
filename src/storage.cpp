#include "storage.h"
#include <fstream>
#include <sys/stat.h>

// Order = priority. "fs:" is the wut mount of the SD card on console (and Cemu), "." = last resort.
static const char* DIRS[] = {
    "fs:/vol/external01/wiiu/apps/MiiTune",
    "/vol/external01/wiiu/apps/MiiTune",
    ".",
    nullptr
};

void StorageInit() {
    for (int i = 0; DIRS[i]; ++i) {
        std::string path = DIRS[i];
        // mkdir each level after the mount point (errors ignored: already exists / unsupported)
        size_t pos = path.find('/');
        while ((pos = path.find('/', pos + 1)) != std::string::npos) {
            mkdir(path.substr(0, pos).c_str(), 0777);
        }
        mkdir(path.c_str(), 0777);
    }
}

std::string StorageRead(const char* name) {
    for (int i = 0; DIRS[i]; ++i) {
        std::ifstream f(std::string(DIRS[i]) + "/" + name, std::ios::binary);
        if (!f.is_open()) continue;
        std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (!s.empty()) return s;
    }
    return "";
}

bool StorageWrite(const char* name, const std::string& data) {
    for (int i = 0; DIRS[i]; ++i) {
        std::ofstream f(std::string(DIRS[i]) + "/" + name, std::ios::binary | std::ios::trunc);
        if (!f.is_open()) continue;
        f << data;
        f.close();
        if (f.good()) return true;
    }
    return false;
}
