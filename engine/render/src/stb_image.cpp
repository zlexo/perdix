// stb_image's implementation, compiled once, for PNG files only (RFC 0004):
// CMake defines STB_IMAGE_IMPLEMENTATION and STBI_ONLY_PNG for this file's
// target. Files are read by the engine and handed over as bytes, so stb's
// file API is left out with its float and HDR loaders.
#include <stb_image.h>
