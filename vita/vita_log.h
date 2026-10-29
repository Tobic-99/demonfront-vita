#pragma once
// Vita build: route printf to stderr (the TTY); stdout is not connected.
#include <cstdio>
#define printf(...) fprintf(stderr, __VA_ARGS__)
