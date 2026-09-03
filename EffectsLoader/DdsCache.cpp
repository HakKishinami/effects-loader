#include "DdsCache.h"
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>
#include <algorithm>

#pragma warning(push)
#pragma warning(disable: 4244 4267)
#define STB_DXT_IMPLEMENTATION
#include "stb_dxt.h"
#pragma warning(pop)

#pragma pack(push, 1)
struct DDS_PIXELFORMAT {
    uint32_t dwSize;
    uint32_t dwFlags;
    uint32_t dwFourCC;
    uint32_t dwRGBBitCount;
    uint32_t dwRBitMask;
    uint32_t dwGBitMask;
    uint32_t dwBBitMask;
    uint32_t dwABitMask;
};
struct DDS_HEADER {
    uint32_t dwSize;
    uint32_t dwFlags;
    uint32_t dwHeight;
    uint32_t dwWidth;
    uint32_t dwPitchOrLinearSize;
    uint32_t dwDepth;
    uint32_t dwMipMapCount;
    uint32_t dwReserved1[11];
    DDS_PIXELFORMAT ddspf;
    uint32_t dwCaps;
    uint32_t dwCaps2;
    uint32_t dwCaps3;
    uint32_t dwCaps4;
    uint32_t dwReserved2;
};
#pragma pack(pop)

// DDS header flags
#define DDSD_CAPS         0x1
#define DDSD_HEIGHT       0x2
#define DDSD_WIDTH        0x4
#define DDSD_PITCH        0x8
#define DDSD_PIXELFORMAT  0x1000
#define DDPF_ALPHAPIXELS  0x1
#define DDPF_RGB          0x40
#define DDSCAPS_TEXTURE   0x1000

namespace DdsCache {

bool EnsureCacheDir(const char *cacheDir) {
    if (!cacheDir || !cacheDir[0])
        return false;
    // Create parents one level at a time (cacheDir comes pre-validated
    // for length by the caller, so tmp cannot truncate here).
    char tmp[MAX_PATH];
    strncpy(tmp, cacheDir, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    size_t len = strlen(tmp);
    // Skip drive-letter colon so "D:\" is not passed to CreateDirectory.
    for (size_t i = 1; i < len; i++) {
        if (tmp[i] == '\\' || tmp[i] == '/') {
            char saved = tmp[i];
            tmp[i] = '\0';
            if (i > 2) // longer than "D:"
                CreateDirectoryA(tmp, NULL); // ignore ERROR_ALREADY_EXISTS
            tmp[i] = saved;
        }
    }
    if (!CreateDirectoryA(tmp, NULL)) {
        DWORD err = GetLastError();
        if (err != ERROR_ALREADY_EXISTS)
            return false;
    }
    DWORD attr = GetFileAttributesA(tmp);
    return (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY));
}

bool BuildCachePath(const char *cacheDir, const char *texName, char *outPath, size_t outSize) {
    if (!cacheDir || !texName || !outPath || outSize == 0)
        return false;
    int n = snprintf(outPath, outSize, "%s\\%s.dds", cacheDir, texName);
    return (n > 0 && (size_t)n < outSize);
}

bool JoinPath(char *outPath, size_t outSize, const char *dir, const char *file) {
    if (!outPath || outSize == 0 || !dir || !file)
        return false;
    int n = snprintf(outPath, outSize, "%s\\%s", dir, file);
    return (n > 0 && (size_t)n < outSize);
}

bool GetFileInfo(const char *path, unsigned long long *sizeOut, unsigned long long *mtime100nsOut) {
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fad))
        return false;
    if (sizeOut) {
        *sizeOut = ((unsigned long long)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
    }
    if (mtime100nsOut) {
        ULARGE_INTEGER t;
        t.LowPart = fad.ftLastWriteTime.dwLowDateTime;
        t.HighPart = fad.ftLastWriteTime.dwHighDateTime;
        *mtime100nsOut = t.QuadPart;
    }
    return true;
}

bool IsCacheFresh(const char *srcPngPath, const char *cacheDdsPath,
                  unsigned long long *srcSizeOut, unsigned long long *cacheSizeOut) {
    unsigned long long srcSize = 0, cacheSize = 0, srcTime = 0, cacheTime = 0;
    if (!GetFileInfo(srcPngPath, &srcSize, &srcTime))
        return false; // source vanished -> caller handles PNG failure anyway
    if (!GetFileInfo(cacheDdsPath, &cacheSize, &cacheTime))
        return false; // no cache -> MISS
    if (srcSizeOut)
        *srcSizeOut = srcSize;
    if (cacheSizeOut)
        *cacheSizeOut = cacheSize;
    // Fresh only if cache was written at/after the source PNG.
    // (>= tolerates same-second copies; a touched/re-saved PNG regenerates.)
    return cacheTime >= srcTime && cacheSize >= (4 + sizeof(DDS_HEADER));
}

unsigned long WriteUncompressedDDS(const char *ddsPath,
                                   const unsigned char *rgbaPixels,
                                   int width, int height, int rgbaStride, int depth) {
    if (!ddsPath || !rgbaPixels || width <= 0 || height <= 0 || width > 4096 || height > 4096)
        return 0;
    if (depth != 32 && depth != 24)
        return 0; // paletted/grey PNGs: skip caching, PNG path already handles them

    FILE *f = fopen(ddsPath, "wb");
    if (!f)
        return 0;

    DDS_HEADER h;
    memset(&h, 0, sizeof(h));
    h.dwSize = 124;
    h.dwFlags = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT;
    h.dwHeight = (uint32_t)height;
    h.dwWidth = (uint32_t)width;
    h.dwPitchOrLinearSize = (uint32_t)(width * 4);
    h.dwMipMapCount = 0; // no mipmaps in v1: matches current PNG raster path 1:1
    h.ddspf.dwSize = 32;
    h.ddspf.dwFlags = DDPF_RGB | DDPF_ALPHAPIXELS;
    h.ddspf.dwRGBBitCount = 32;
    h.ddspf.dwRBitMask = 0x00FF0000;
    h.ddspf.dwGBitMask = 0x0000FF00;
    h.ddspf.dwBBitMask = 0x000000FF;
    h.ddspf.dwABitMask = 0xFF000000;
    h.dwCaps = DDSCAPS_TEXTURE;

    unsigned long written = 0;
    if (fwrite("DDS ", 1, 4, f) == 4 && fwrite(&h, 1, sizeof(h), f) == sizeof(h)) {
        written = 4 + (unsigned long)sizeof(h);
        const int srcBpp = (depth == 32) ? 4 : 3;
        // DDS rows are top-down, same as RwImage. Swizzle RGBA -> BGRA.
        for (int y = 0; y < height; y++) {
            const unsigned char *row = rgbaPixels + (size_t)y * rgbaStride;
            for (int x = 0; x < width; x++) {
                unsigned char r = row[x * srcBpp + 0];
                unsigned char g = row[x * srcBpp + 1];
                unsigned char b = row[x * srcBpp + 2];
                unsigned char a = (depth == 32) ? row[x * srcBpp + 3] : 0xFF;
                unsigned char bgra[4] = { b, g, r, a };
                if (fwrite(bgra, 1, 4, f) != 4) {
                    written = 0; // partial write -> caller treats as failure
                    break;
                }
                written += 4;
            }
            if (written == 0)
                break;
        }
    } else {
        written = 0;
    }
    fclose(f);
    if (written == 0)
        DeleteFileA(ddsPath); // never leave a truncated .dds behind
    return written;
}

bool StripExtension(const char *path, char *outPath, size_t outSize) {
    if (!path || !outPath || outSize == 0)
        return false;
    strncpy(outPath, path, outSize - 1);
    outPath[outSize - 1] = '\0';
    char *dot = strrchr(outPath, '.');
    char *slash = strrchr(outPath, '\\');
    if (!dot || (slash && dot < slash))
        return false;
    *dot = '\0';
    return true;
}

bool MakeTexName(const char *filePath, char *texNameOut) {
    if (!filePath || !texNameOut)
        return false;
    _splitpath(filePath, NULL, NULL, texNameOut, NULL);
    texNameOut[MAX_TEX_NAME] = '\0';
    return texNameOut[0] != '\0';
}

// ---------------- V2 engineering: analysis + decision + compression ----

static bool IsPow2(int v) {
    return v > 0 && (v & (v - 1)) == 0;
}

static int FullMipChain(int w, int h) {
    int n = 1;
    while (w > 1 || h > 1) {
        if (w > 1)
            w /= 2;
        if (h > 1)
            h /= 2;
        n++;
    }
    return n;
}

const char *FormatName(DdsFormat f) {
    switch (f) {
    case DdsFormat::DXT1: return "DXT1";
    case DdsFormat::DXT5: return "DXT5";
    default: return "BGRA";
    }
}

AlphaStats AnalyzeAlpha(const unsigned char *rgba, int width, int height) {
    AlphaStats st;
    if (!rgba || width <= 0 || height <= 0)
        return st;
    const size_t total = (size_t)width * height;
    size_t semi = 0;
    for (size_t i = 0; i < total; i++) {
        unsigned char a = rgba[i * 4 + 3];
        if (a < 255) {
            st.hasAlpha = true;
            if (a > 0)
                semi++;
        }
    }
    st.semiFrac = total ? (double)semi / (double)total : 0.0;
    // Sharpness: 4x4 blocks holding both near-0 and near-255 alpha with few
    // in-between values (hard cutouts / text-like edges vs soft smoke).
    int blocks = 0, sharp = 0;
    for (int by = 0; by < height; by += 4) {
        for (int bx = 0; bx < width; bx += 4) {
            blocks++;
            int lo = 255, hi = 0, mid = 0, cnt = 0;
            for (int y = by; y < by + 4 && y < height; y++) {
                for (int x = bx; x < bx + 4 && x < width; x++) {
                    int a = rgba[((size_t)y * width + x) * 4 + 3];
                    if (a < lo)
                        lo = a;
                    if (a > hi)
                        hi = a;
                    if (a > 32 && a < 224)
                        mid++;
                    cnt++;
                }
            }
            if (lo <= 32 && hi >= 224 && mid * 4 < cnt)
                sharp++;
        }
    }
    st.sharpFrac = blocks ? (double)sharp / (double)blocks : 0.0;
    return st;
}

static bool NameHas(const std::string &lowerName, const char *token) {
    return lowerName.find(token) != std::string::npos;
}

DecideResult DecideFormat(const unsigned char *rgba, int width, int height,
                          const char *fileName, int fidelity, bool allowMipsNPOT,
                          int tinySize) {
    DecideResult r;
    std::string fn = fileName ? fileName : "";
    for (auto &c : fn)
        c = (char)tolower((unsigned char)c);

    if (NameHas(fn, "_nocache")) {
        r.skipCache = true;
        r.reason = "author-nocache";
        return r;
    }
    const bool fDxt1 = NameHas(fn, "_dxt1");
    const bool fDxt5 = NameHas(fn, "_dxt5");
    const bool fBgra = NameHas(fn, "_bgra");
    const bool fNoMip = NameHas(fn, "_nomip");
    const bool fMip = NameHas(fn, "_mip");

    AlphaStats st;
    if (!fBgra && fidelity != 0)
        st = AnalyzeAlpha(rgba, width, height);

    if (fBgra) {
        r.format = DdsFormat::BGRA;
        r.reason = "author-bgra";
    } else if (fidelity == 0) {
        r.format = DdsFormat::BGRA;
        r.reason = "lossless";
    } else if (fDxt1) {
        if (!st.hasAlpha) {
            r.format = DdsFormat::DXT1;
            r.reason = "author-dxt1";
        } else {
            // stb_dxt has no DXT1 punch-through mode: honoring _dxt1 on
            // transparent pixels would DESTROY the alpha. Fall back to DXT5.
            r.format = DdsFormat::DXT5;
            r.reason = "author-dxt1-has-alpha->dxt5";
        }
    } else if (fDxt5) {
        if (!st.hasAlpha) {
            r.format = DdsFormat::DXT1;
            r.reason = "author-dxt5-opaque->dxt1";
        } else {
            r.format = DdsFormat::DXT5;
            r.reason = "author-dxt5";
        }
    } else if (width <= tinySize && height <= tinySize) {
        r.format = DdsFormat::BGRA;
        r.reason = "tiny";
    } else if (!st.hasAlpha) {
        r.format = DdsFormat::DXT1;
        r.reason = "opaque";
    } else {
        r.format = DdsFormat::DXT5;
        r.reason = (st.sharpFrac > 0.2) ? "hard-alpha" : "smooth-alpha";
    }

    // D3D9/RW loader rejects DXT textures whose dimensions are not multiples
    // of 4 (proven on real cache: 17/17 such files HIT-FAILED, 225/225
    // aligned ones OK). Keep them on the V1-proven BGRA path instead.
    if ((r.format == DdsFormat::DXT1 || r.format == DdsFormat::DXT5) &&
        (width % 4 != 0 || height % 4 != 0)) {
        r.format = DdsFormat::BGRA;
        r.reason += "+align4->bgra";
    }

    // ---- mip rule (engineering build) ----
    // BGRA: never (matches V1 behavior). Small: never minified enough.
    // POT+POT: full chain (D3D9-legal everywhere). NPOT: only with the
    // MipsNPOT trial switch (D3D9-era hardware may reject mipmapped NPOT).
    const int maxDim = width > height ? width : height;
    if (r.format == DdsFormat::BGRA || maxDim < 64 || fNoMip) {
        r.mipLevels = 0;
        if (fNoMip && r.format != DdsFormat::BGRA)
            r.reason += "+nomip";
    } else if (IsPow2(width) && IsPow2(height)) {
        r.mipLevels = FullMipChain(width, height);
    } else if (allowMipsNPOT || fMip) {
        r.mipLevels = FullMipChain(width, height);
        r.reason += allowMipsNPOT ? "+npot-mips" : "+author-mip";
    } else {
        r.mipLevels = 0;
        r.reason += "+npot-off";
    }
    return r;
}

void BoxDownscale(const unsigned char *src, int srcW, int srcH,
                  unsigned char *out, int outW, int outH) {
    for (int y = 0; y < outH; y++) {
        int sy0 = y * srcH / outH;
        int sy1 = (y + 1) * srcH / outH;
        if (sy1 <= sy0)
            sy1 = sy0 + 1;
        for (int x = 0; x < outW; x++) {
            int sx0 = x * srcW / outW;
            int sx1 = (x + 1) * srcW / outW;
            if (sx1 <= sx0)
                sx1 = sx0 + 1;
            unsigned sum[4] = { 0, 0, 0, 0 };
            int n = 0;
            for (int sy = sy0; sy < sy1; sy++) {
                for (int sx = sx0; sx < sx1; sx++) {
                    const unsigned char *p = src + ((size_t)sy * srcW + sx) * 4;
                    sum[0] += p[0];
                    sum[1] += p[1];
                    sum[2] += p[2];
                    sum[3] += p[3];
                    n++;
                }
            }
            unsigned char *d = out + ((size_t)y * outW + x) * 4;
            d[0] = (unsigned char)(sum[0] / n);
            d[1] = (unsigned char)(sum[1] / n);
            d[2] = (unsigned char)(sum[2] / n);
            d[3] = (unsigned char)(sum[3] / n);
        }
    }
}

#define DDSD_CAPS        0x1
#define DDSD_HEIGHT      0x2
#define DDSD_WIDTH       0x4
#define DDSD_PITCH       0x8
#define DDSD_PIXELFORMAT 0x1000
#define DDSD_MIPMAPCOUNT 0x20000
#define DDSD_LINEARSIZE  0x80000
#define DDPF_FOURCC      0x4
#define DDSCAPS_COMPLEX  0x8
#define DDSCAPS_TEXTURE  0x1000
#define DDSCAPS_MIPMAP   0x400000
#define FOURCC_DXT1      0x31545844u
#define FOURCC_DXT5      0x35545844u

#pragma pack(push, 1)
struct DDS_PF_FOURCC {
    uint32_t dwSize;
    uint32_t dwFlags;
    uint32_t dwFourCC;
    uint32_t dwRGBBitCount;
    uint32_t dwRBitMask;
    uint32_t dwGBitMask;
    uint32_t dwBBitMask;
    uint32_t dwABitMask;
};
struct DDS_HEAD_DXT {
    uint32_t dwSize;
    uint32_t dwFlags;
    uint32_t dwHeight;
    uint32_t dwWidth;
    uint32_t dwPitchOrLinearSize;
    uint32_t dwDepth;
    uint32_t dwMipMapCount;
    uint32_t dwReserved1[11];
    DDS_PF_FOURCC ddspf;
    uint32_t dwCaps;
    uint32_t dwCaps2;
    uint32_t dwCaps3;
    uint32_t dwCaps4;
    uint32_t dwReserved2;
};
#pragma pack(pop)

unsigned long WriteCachedDDS(const char *ddsPath,
                             const unsigned char *rgba, int width, int height,
                             const DecideResult &dec) {
    if (!ddsPath || !rgba || width <= 0 || height <= 0 || width > 4096 || height > 4096)
        return 0;
    if (dec.format == DdsFormat::BGRA) {
        // Lossless path, identical to V1 (no mips by design in eng build).
        return WriteUncompressedDDS(ddsPath, rgba, width, height, width * 4, 32);
    }

    const bool dxt1 = (dec.format == DdsFormat::DXT1);
    const int blockBytes = dxt1 ? 8 : 16;

    // Build the mip chain in memory (level 0 = source, box-filtered below).
    struct Level {
        int w, h;
        std::vector<unsigned char> px;
    };
    std::vector<Level> chain;
    chain.reserve(dec.mipLevels > 0 ? dec.mipLevels : 1);
    {
        Level top;
        top.w = width;
        top.h = height;
        top.px.assign(rgba, rgba + (size_t)width * height * 4);
        chain.push_back(std::move(top));
    }
    const int wantLevels = dec.mipLevels > 1 ? dec.mipLevels : 1;
    while ((int)chain.size() < wantLevels) {
        const Level &prev = chain.back();
        if (prev.w <= 1 && prev.h <= 1)
            break;
        Level next;
        next.w = prev.w > 1 ? prev.w / 2 : 1;
        next.h = prev.h > 1 ? prev.h / 2 : 1;
        next.px.resize((size_t)next.w * next.h * 4);
        BoxDownscale(prev.px.data(), prev.w, prev.h, next.px.data(), next.w, next.h);
        chain.push_back(std::move(next));
    }

    FILE *f = fopen(ddsPath, "wb");
    if (!f)
        return 0;
    unsigned long written = 0;

    DDS_HEAD_DXT h;
    memset(&h, 0, sizeof(h));
    const int topBlocks =
        ((width + 3) / 4) * ((height + 3) / 4) * blockBytes;
    h.dwSize = 124;
    h.dwFlags = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PIXELFORMAT | DDSD_LINEARSIZE;
    h.dwHeight = (uint32_t)height;
    h.dwWidth = (uint32_t)width;
    h.dwPitchOrLinearSize = (uint32_t)topBlocks;
    h.dwMipMapCount = (uint32_t)chain.size();
    if (chain.size() > 1) {
        h.dwFlags |= DDSD_MIPMAPCOUNT;
        h.dwCaps = DDSCAPS_TEXTURE | DDSCAPS_MIPMAP | DDSCAPS_COMPLEX;
    } else {
        h.dwCaps = DDSCAPS_TEXTURE;
    }
    h.ddspf.dwSize = 32;
    h.ddspf.dwFlags = DDPF_FOURCC;
    h.ddspf.dwFourCC = dxt1 ? FOURCC_DXT1 : FOURCC_DXT5;

    std::vector<unsigned char> blockOut(16);
    unsigned char blockIn[16 * 4];
    if (fwrite("DDS ", 1, 4, f) == 4 && fwrite(&h, 1, sizeof(h), f) == sizeof(h)) {
        written = 4 + (unsigned long)sizeof(h);
        bool ok = true;
        for (const auto &lv : chain) {
            const int bw = (lv.w + 3) / 4;
            const int bh = (lv.h + 3) / 4;
            for (int by = 0; by < bh && ok; by++) {
                for (int bx = 0; bx < bw && ok; bx++) {
                    // Clamp-edge pad for non-multiple-of-4 levels.
                    for (int y = 0; y < 4; y++) {
                        for (int x = 0; x < 4; x++) {
                            int sx = bx * 4 + x;
                            int sy = by * 4 + y;
                            if (sx >= lv.w)
                                sx = lv.w - 1;
                            if (sy >= lv.h)
                                sy = lv.h - 1;
                            memcpy(blockIn + (y * 4 + x) * 4,
                                   lv.px.data() + ((size_t)sy * lv.w + sx) * 4, 4);
                        }
                    }
                    stb_compress_dxt_block(blockOut.data(), blockIn,
                                           dxt1 ? 0 : 1, STB_DXT_HIGHQUAL);
                    if (fwrite(blockOut.data(), 1, blockBytes, f) != (size_t)blockBytes)
                        ok = false;
                    else
                        written += blockBytes;
                }
            }
            if (!ok)
                break;
        }
        if (!ok)
            written = 0;
    } else {
        written = 0;
    }
    fclose(f);
    if (written == 0)
        DeleteFileA(ddsPath); // never leave a truncated .dds behind
    return written;
}

} // namespace DdsCache
