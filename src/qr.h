#ifndef MIITUNE_QR_H
#define MIITUNE_QR_H

#include <string>
#include <vector>

namespace qr {

struct Code {
    int size = 0;             // Modules per side (e.g. 21, 25, 29...)
    std::vector<bool> dark;   // size * size, row by row
    bool at(int x, int y) const {
        if (x < 0 || x >= size || y < 0 || y >= size) return false;
        return dark[(size_t)y * size + x];
    }
};

Code encode(const std::string& text);

} // namespace qr

#endif // MIITUNE_QR_H
