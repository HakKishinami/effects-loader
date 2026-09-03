#pragma once
#include <Windows.h>
#include <stdint.h>
#include <string>

// Experimental DDS cache (v1): converts PNG-loaded RwImages into UNCOMPRESSED
// 32-bit A8R8G8B8 .dds files under a central cache dir. Lossless by design -
// the pixel data is identical to the PNG path, so this version only validates
// the pipeline (naming, freshness, RwD3D9DDSTextureRead compatibility,
// dictionary insertion). DXT compression comes later once the link is proven.
//
// Cache layout:  <cacheDir>\<texName>.dds   (texName = 31-char RW texture name)
// Freshness:     cache .dds write-time >= source .png write-time  => HIT
// Safety:        source PNG is never touched; any DDS failure falls back to PNG.

namespace DdsCache {

static const bool ENABLED = true;

// Bump this whenever the on-disk cache format changes incompatibly -
// LoadProject wipes all *.dds once when the stored version mismatches,
// so broken/old files can never linger (e.g. V2 DXT with non-4-aligned
// dims that the game rejects).
static const int CACHE_VERSION = 2;

// Max texture name length respected by RW (see LoadPNGTextureCB texName[31]).
static const size_t MAX_TEX_NAME = 31;

// Ensures cacheDir exists (creates it incl. parents). Returns false on failure.
bool EnsureCacheDir(const char *cacheDir);

// Builds "<cacheDir>\<texName>.dds" into outPath (MAX_PATH). Returns false if overflow.
bool BuildCachePath(const char *cacheDir, const char *texName, char *outPath, size_t outSize);

// Joins dir + "\\" + file with overflow check. Returns false on truncation
// (caller must skip the file - never act on a truncated path).
bool JoinPath(char *outPath, size_t outSize, const char *dir, const char *file);

// True if cacheDds exists and is not older than srcPng (both by last-write time).
// On success fills sizes for logging. Never fails the load - false just means MISS.
bool IsCacheFresh(const char *srcPngPath, const char *cacheDdsPath,
                  unsigned long long *srcSizeOut, unsigned long long *cacheSizeOut);

// Writes an uncompressed 32-bit DDS from raw top-down RGBA pixels.
// width/height from RwImage, rgbaStride = RwImage->stride, depth must be 32 (or 24,
// in which case alpha is forced opaque). Returns bytes written, 0 on failure.
// NOTE: bytes are swizzled RGBA -> BGRA to match A8R8G8B8 masks.
unsigned long WriteUncompressedDDS(const char *ddsPath,
                                   const unsigned char *rgbaPixels,
                                   int width, int height, int rgbaStride, int depth);

// Strips the last extension: "C:\a\foo.dds" -> "C:\a\foo".
// RwD3D9DDSTextureRead expects the name WITHOUT extension (it appends .dds itself,
// same convention as the existing LoadDDSTextureCB).
bool StripExtension(const char *path, char *outPath, size_t outSize);

// Extracts RW texture name (no dir, no ext, truncated to 31 chars) from a file path.
bool MakeTexName(const char *filePath, char *texNameOut /*[MAX_PATH]*/);

// File-time/size helpers for logging.
bool GetFileInfo(const char *path, unsigned long long *sizeOut, unsigned long long *mtime100nsOut);

// ---- V2 engineering build: compressed DDS ----
enum class DdsFormat { BGRA, DXT1, DXT5 };

struct AlphaStats {
    bool hasAlpha = false;   // any pixel with alpha < 255
    double semiFrac = 0.0;   // fraction of pixels with 1..254 alpha
    double sharpFrac = 0.0;  // fraction of 4x4 blocks with hard 0<->255 alpha jumps
};

struct DecideResult {
    DdsFormat format = DdsFormat::BGRA;
    int mipLevels = 0;          // 0 = top level only, N = full chain length incl. top
    std::string reason;         // short tag for the log, e.g. "opaque", "smooth-alpha"
    bool skipCache = false;     // "_nocache" author veto
};

// Single O(n) pass over a packed RGBA buffer (stride = width*4).
AlphaStats AnalyzeAlpha(const unsigned char *rgba, int width, int height);

// Full decision: alpha stats + dimensions + filename suffixes + config.
// - tinySize: max(w,h) <= tinySize -> BGRA (default 32)
// - allowMipsNPOT: engineering switch for non-power-of-two mip chains (default off,
//   D3D9-era hardware may reject mipmapped NPOT; HIT-FAILED fallback protects)
// - fileName: full source path, scanned (case-insensitive) for _nocache/_dxt1/_dxt5/_bgra/_nomip/_mip
// - fidelity: 0 = lossless (all BGRA), 1 = balanced (table), 2 = fast (same table;
//   caller applies MaxDim downscale BEFORE calling, then analysis runs on final pixels)
DecideResult DecideFormat(const unsigned char *rgba, int width, int height,
                          const char *fileName, int fidelity, bool allowMipsNPOT,
                          int tinySize);

// Box-filter downscale of packed RGBA into out (outW*outH*4, preallocated).
void BoxDownscale(const unsigned char *src, int srcW, int srcH,
                  unsigned char *out, int outW, int outH);

// Writes the final cache file: BGRA (lossless, no mips) or DXT1/DXT5 with
// optional full mip chain (box-filtered). Returns bytes written, 0 on failure.
unsigned long WriteCachedDDS(const char *ddsPath,
                             const unsigned char *rgba, int width, int height,
                             const DecideResult &dec);

const char *FormatName(DdsFormat f);

} // namespace DdsCache
