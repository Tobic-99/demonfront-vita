// ROM access for the desktop and Vita builds: each file is taken from <base>/rom/ when present there, otherwise
// from any .zip in <base> or <base>/rom (MAME sets: dmnfrnt.zip, and pgm.zip for the BIOS).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace romset {
void init(const std::string& base);
bool read(const std::string& name, std::vector<uint8_t>& out);   // whole file
bool arm_rom(std::vector<uint8_t>& out);                          // arm_ext.bin, or v105_32m.u26 decrypted
}
