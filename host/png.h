// Minimal PNG writer (stored deflate blocks, no compression): for desktop screenshots.
#pragma once
#include <cstdint>
#include <string>

bool write_png(const std::string& path, const uint32_t* rgba, int w, int h);   // pixels 0xAABBGGRR
