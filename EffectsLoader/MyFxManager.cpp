#include "MyFxManager.h"
#include "Search.h"
#include "LogFile.h"
#include "DdsCache.h"
#include <stdio.h>
#include <share.h>
#include <Windows.h>
#include <map>
#include "game_sa\FxManager_c.h"
#include "game_sa\CTxdStore.h"
#include "game_sa\CFileMgr.h"
#include "game_sa\CKeyGen.h"

std::unordered_set<unsigned int> MyFxManager::customParticlesKeys;
char MyFxManager::tempSystemName[256];
std::unordered_set<std::string> MyFxManager::customTexturesNames;
std::string MyFxManager::ddsCacheDir;
bool MyFxManager::ddsCacheReady = false;
int MyFxManager::statPngSeen = 0;
int MyFxManager::statCacheHit = 0;
int MyFxManager::statCacheMiss = 0;
int MyFxManager::statCacheWriteOk = 0;
int MyFxManager::statCacheWriteSkip = 0;
int MyFxManager::statCacheWriteFail = 0;
int MyFxManager::statDdsHitFallbackToPng = 0;
unsigned long long MyFxManager::statPngBytes = 0;
unsigned long long MyFxManager::statDdsBytes = 0;
unsigned long long MyFxManager::statPngMs = 0;
unsigned long long MyFxManager::statDdsMs = 0;

// Configured capacity for FxMemoryPool_c blueprint definition pool (default 16MB)
unsigned int MyFxManager::configuredPoolSizeMB = 16;

// ---- V2-eng cache config (effects-loader.ini next to our own .asi) ----
// [Cache] Enabled=1 Fidelity=balanced|lossless|fast MipsNPOT=0 MaxDim=0
static int g_cfgCacheOn = 1;
static int g_cfgFidelity = 1; // 0 = lossless (all BGRA), 1 = balanced, 2 = fast (+MaxDim clamp)
static int g_cfgMipsNPOT = 0;
static int g_cfgMaxDim = 0;
static std::string g_cfgCacheDir; // empty = default (<game>\effects-loader-cache)

std::string MyFxManager::GetOwnModuleDir() {
    HMODULE hm = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&MyFxManager::GetOwnModuleDir, &hm);
    char p[MAX_PATH] = { 0 };
    if (hm)
        GetModuleFileNameA(hm, p, MAX_PATH);
    std::string s = p;
    size_t pos = s.find_last_of("\\/");
    return (pos == std::string::npos) ? std::string() : s.substr(0, pos);
}

static void LoadCacheConfig() {
    g_cfgCacheOn = 1;
    g_cfgFidelity = 1;
    g_cfgMipsNPOT = 0;
    g_cfgMaxDim = 0;
    g_cfgCacheDir.clear();
    std::string dir = MyFxManager::GetOwnModuleDir();
    std::string ini = dir.empty() ? "effects-loader.ini" : (dir + "\\effects-loader.ini");
    if (GetFileAttributesA(ini.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::string gameDir = MyFxManager::GetGameDirectory();
        ini = gameDir + "\\effects-loader.ini";
    }
    g_cfgCacheOn = GetPrivateProfileIntA("Cache", "Enabled", 1, ini.c_str());
    char fid[32] = { 0 };
    GetPrivateProfileStringA("Cache", "Fidelity", "balanced", fid, sizeof(fid), ini.c_str());
    if (!_stricmp(fid, "lossless"))
        g_cfgFidelity = 0;
    else if (!_stricmp(fid, "fast"))
        g_cfgFidelity = 2;
    else
        g_cfgFidelity = 1;
    g_cfgMipsNPOT = GetPrivateProfileIntA("Cache", "MipsNPOT", 0, ini.c_str());
    g_cfgMaxDim = GetPrivateProfileIntA("Cache", "MaxDim", 0, ini.c_str());
    if (g_cfgMaxDim < 0)
        g_cfgMaxDim = 0;
    char custom[MAX_PATH] = { 0 };
    GetPrivateProfileStringA("Cache", "CacheDir", "", custom, sizeof(custom), ini.c_str());
    // Trim trailing slashes (but keep "X:\" style roots intact).
    std::string c = custom;
    while (c.size() > 3 && (c.back() == '\\' || c.back() == '/'))
        c.pop_back();
    g_cfgCacheDir = c;
}

// Resolves the effective cache dir: ini CacheDir wins when set.
// Absolute (X:\... or \\UNC\...) is used as-is, otherwise relative to gameDir.
static std::string ResolveCacheDir(const std::string &gameDir) {
    if (!g_cfgCacheDir.empty()) {
        const std::string &c = g_cfgCacheDir;
        bool absolute = (c.size() > 2 && c[1] == ':' && (c[2] == '\\' || c[2] == '/')) ||
                        (c.size() > 1 && c[0] == '\\' && c[1] == '\\');
        if (absolute)
            return c;
        return gameDir + "\\" + c;
    }
    std::string mlDir = gameDir + "\\modloader";
    if (Search::DirectoryExists(mlDir.c_str()))
        return mlDir + "\\.effects-cache";
    return gameDir + "\\effects-loader-cache";
}

// Lowercases for dedup-set keys only. Game-facing strings (dictionary names,
// cache file names) always keep their original case - see the header comment.
static std::string LowerStr(const char *s) {
    std::string r = s ? s : "";
    for (auto &c : r)
        c = (char)tolower((unsigned char)c);
    return r;
}

// Non-recursive iterative 2-pointer wildcard matcher (case-insensitive).
// Supports '*' (match 0 or more characters) and '?' (match exactly 1 character).
static bool WildcardMatch(const char *pat, const char *str) {
    if (!pat || !str) return false;
    const char *pPat = pat;
    const char *pStr = str;
    const char *starPat = nullptr;
    const char *starStr = nullptr;

    while (*pStr) {
        if (*pPat == '?' || tolower((unsigned char)*pPat) == tolower((unsigned char)*pStr)) {
            pPat++;
            pStr++;
        } else if (*pPat == '*') {
            starPat = pPat++;
            starStr = pStr;
        } else if (starPat) {
            pPat = starPat + 1;
            pStr = ++starStr;
        } else {
            return false;
        }
    }
    while (*pPat == '*') pPat++;
    return *pPat == '\0';
}

static std::string TrimStr(const std::string &s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    std::string r = s.substr(start, end - start + 1);
    if (r.size() >= 2 && ((r.front() == '"' && r.back() == '"') || (r.front() == '\'' && r.back() == '\''))) {
        r = r.substr(1, r.size() - 2);
        start = r.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) return "";
        end = r.find_last_not_of(" \t\r\n");
        r = r.substr(start, end - start + 1);
    }
    return r;
}

enum class ModIgnoreReason {
    NotIgnored,
    DotPrefix,
    IgnoreAll,
    ExcludeAll,
    IgnoreList
};

struct ModLoaderFilter {
    bool ignoreAll = false;
    bool excludeAll = false;
    std::vector<std::string> ignoreList;
    std::vector<std::string> includeList;

    ModIgnoreReason GetIgnoreReason(const char *folderName) const {
        if (!folderName || folderName[0] == '\0')
            return ModIgnoreReason::DotPrefix;
        // ModLoader natively treats folders whose name starts with '.' as disabled
        if (folderName[0] == '.')
            return ModIgnoreReason::DotPrefix;
        if (ignoreAll)
            return ModIgnoreReason::IgnoreAll;
        if (excludeAll) {
            bool included = false;
            for (const auto &pat : includeList) {
                if (WildcardMatch(pat.c_str(), folderName)) {
                    included = true;
                    break;
                }
            }
            if (!included)
                return ModIgnoreReason::ExcludeAll;
        }
        for (const auto &pat : ignoreList) {
            if (WildcardMatch(pat.c_str(), folderName))
                return ModIgnoreReason::IgnoreList;
        }
        return ModIgnoreReason::NotIgnored;
    }

    bool IsIgnored(const char *folderName) const {
        return GetIgnoreReason(folderName) != ModIgnoreReason::NotIgnored;
    }
};

static ModLoaderFilter ParseModLoaderIni(const std::string &modloaderDir) {
    ModLoaderFilter filter;
    std::string iniPath = modloaderDir + "\\modloader.ini";
    FILE *f = _fsopen(iniPath.c_str(), "rt", _SH_DENYNO);
    if (!f)
        return filter;

    char lineBuf[1024];
    std::string activeProfile = "Default";
    std::vector<std::string> activeProfiles;

    // Pass 1: Read active profile from [Folder.Config]
    std::string currentSec;
    bool isFirstLine = true;
    while (fgets(lineBuf, sizeof(lineBuf), f)) {
        std::string line = lineBuf;
        if (isFirstLine) {
            isFirstLine = false;
            if (line.size() >= 3 &&
                (unsigned char)line[0] == 0xEF &&
                (unsigned char)line[1] == 0xBB &&
                (unsigned char)line[2] == 0xBF) {
                line.erase(0, 3);
            }
        }
        size_t semi = line.find(';');
        if (semi != std::string::npos)
            line.erase(semi);
        line = TrimStr(line);
        if (line.empty())
            continue;

        if (line.size() >= 2 && line.front() == '[' && line.back() == ']') {
            currentSec = LowerStr(TrimStr(line.substr(1, line.size() - 2)).c_str());
            continue;
        }

        if (currentSec == "folder.config") {
            size_t eq = line.find('=');
            if (eq != std::string::npos) {
                std::string key = LowerStr(TrimStr(line.substr(0, eq)).c_str());
                std::string val = TrimStr(line.substr(eq + 1));
                if (key == "profile" && !val.empty()) {
                    activeProfile = val;
                }
            }
        }
    }

    activeProfiles.push_back(LowerStr(activeProfile.c_str()));

    // Pass 2: Look for Parents in [Profiles.<activeProfile>.Config]
    fseek(f, 0, SEEK_SET);
    currentSec.clear();
    isFirstLine = true;
    std::string profCfgSec = "profiles." + LowerStr(activeProfile.c_str()) + ".config";
    while (fgets(lineBuf, sizeof(lineBuf), f)) {
        std::string line = lineBuf;
        if (isFirstLine) {
            isFirstLine = false;
            if (line.size() >= 3 &&
                (unsigned char)line[0] == 0xEF &&
                (unsigned char)line[1] == 0xBB &&
                (unsigned char)line[2] == 0xBF) {
                line.erase(0, 3);
            }
        }
        size_t semi = line.find(';');
        if (semi != std::string::npos)
            line.erase(semi);
        line = TrimStr(line);
        if (line.empty())
            continue;

        if (line.size() >= 2 && line.front() == '[' && line.back() == ']') {
            currentSec = LowerStr(TrimStr(line.substr(1, line.size() - 2)).c_str());
            continue;
        }

        if (currentSec == profCfgSec) {
            size_t eq = line.find('=');
            if (eq != std::string::npos) {
                std::string key = LowerStr(TrimStr(line.substr(0, eq)).c_str());
                std::string val = TrimStr(line.substr(eq + 1));
                if (key == "parents" && !val.empty() && LowerStr(val.c_str()) != "$none") {
                    size_t pos = 0;
                    while (pos < val.size()) {
                        size_t comma = val.find(',', pos);
                        std::string item = (comma == std::string::npos) ? val.substr(pos) : val.substr(pos, comma - pos);
                        item = LowerStr(TrimStr(item).c_str());
                        if (!item.empty() && item != "$none") {
                            bool found = false;
                            for (const auto &p : activeProfiles) {
                                if (p == item) { found = true; break; }
                            }
                            if (!found)
                                activeProfiles.push_back(item);
                        }
                        if (comma == std::string::npos) break;
                        pos = comma + 1;
                    }
                }
            }
        }
    }

    // Pass 3: Collect IgnoreMods, Config, Priority, IncludeMods for all activeProfiles
    fseek(f, 0, SEEK_SET);
    currentSec.clear();
    isFirstLine = true;

    auto matchesProfileSec = [&](const std::string &suffix) -> bool {
        for (const auto &prof : activeProfiles) {
            if (currentSec == "profiles." + prof + "." + suffix)
                return true;
        }
        return false;
    };

    while (fgets(lineBuf, sizeof(lineBuf), f)) {
        std::string line = lineBuf;
        if (isFirstLine) {
            isFirstLine = false;
            if (line.size() >= 3 &&
                (unsigned char)line[0] == 0xEF &&
                (unsigned char)line[1] == 0xBB &&
                (unsigned char)line[2] == 0xBF) {
                line.erase(0, 3);
            }
        }
        size_t semi = line.find(';');
        if (semi != std::string::npos)
            line.erase(semi);
        line = TrimStr(line);
        if (line.empty())
            continue;

        if (line.size() >= 2 && line.front() == '[' && line.back() == ']') {
            currentSec = LowerStr(TrimStr(line.substr(1, line.size() - 2)).c_str());
            continue;
        }

        if (matchesProfileSec("config")) {
            size_t eq = line.find('=');
            if (eq != std::string::npos) {
                std::string key = LowerStr(TrimStr(line.substr(0, eq)).c_str());
                std::string val = LowerStr(TrimStr(line.substr(eq + 1)).c_str());
                if (key == "ignoreallmods") {
                    if (val == "true" || val == "1" || val == "yes" || val == "on")
                        filter.ignoreAll = true;
                } else if (key == "excludeallmods") {
                    if (val == "true" || val == "1" || val == "yes" || val == "on")
                        filter.excludeAll = true;
                }
            }
        } else if (matchesProfileSec("ignoremods")) {
            size_t eq = line.find('=');
            std::string mod = (eq != std::string::npos) ? line.substr(0, eq) : line;
            mod = TrimStr(mod);
            if (!mod.empty())
                filter.ignoreList.push_back(mod);
        } else if (matchesProfileSec("includemods")) {
            size_t eq = line.find('=');
            std::string mod = (eq != std::string::npos) ? line.substr(0, eq) : line;
            mod = TrimStr(mod);
            if (!mod.empty())
                filter.includeList.push_back(mod);
        } else if (matchesProfileSec("priority")) {
            size_t eq = line.find('=');
            if (eq != std::string::npos) {
                std::string key = TrimStr(line.substr(0, eq));
                std::string val = TrimStr(line.substr(eq + 1));
                if (val == "0" && !key.empty())
                    filter.ignoreList.push_back(key);
            }
        }
    }

    fclose(f);
    return filter;
}

// Stores names of effect mods that were skipped because they were disabled in ModLoader
static std::vector<std::string> g_mlIgnoredFolders;

// Structure for tracking textures declared in .fxs files (for missing texture diagnostics)
struct DeclaredTexture {
    std::string originalName;
    std::string fxsSourcePath;
};
static std::map<std::string, DeclaredTexture> s_declaredFxsTextures;

// Extracts texture names declared in an .fxs file (TEXTURE:, TEXTURE2:, etc.)
// Strictly respects NUM_PRIMS declared in the header: GTA SA engine stops reading
// primitives after NUM_PRIMS, so any abandoned/draft primitives at the end of the file
// are ignored and do not trigger false missing texture warnings.
template<typename TCallback>
static void ParseTexturesFromFxs(const char *fxsPath, TCallback cb) {
    FILE *f = _fsopen(fxsPath, "rt", _SH_DENYNO);
    if (!f) return;

    char line[512];
    bool isFirstLine = true;
    int maxPrims = 999999;
    int currentPrims = 0;

    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        if (isFirstLine) {
            isFirstLine = false;
            if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) {
                p += 3;
            }
        }
        while (*p == ' ' || *p == '\t') p++;

        // Read active primitive count limit (GTA SA engine stops loading primitives after NUM_PRIMS)
        if (_strnicmp(p, "NUM_PRIMS:", 10) == 0) {
            p += 10;
            while (*p == ' ' || *p == '\t') p++;
            int n = 0;
            if (sscanf(p, "%d", &n) == 1 && n >= 0) {
                maxPrims = n;
            }
            continue;
        }

        // Each primitive begins with FX_PRIM_EMITTER_DATA:
        if (_strnicmp(p, "FX_PRIM_EMITTER_DATA:", 21) == 0) {
            currentPrims++;
            if (currentPrims > maxPrims) {
                break; // Stop parsing; ignore all discarded/draft primitives after maxPrims
            }
            continue;
        }

        // Only parse texture declarations from active primitives
        if (currentPrims <= maxPrims) {
            // Match TEXTURE, TEXTURE2, TEXTURE3 ... TEXTURE28
            if (_strnicmp(p, "TEXTURE", 7) == 0) {
                p += 7;
                while (*p >= '0' && *p <= '9') p++; // skip trailing numbers
                while (*p == ' ' || *p == '\t') p++;

                if (*p == ':') {
                    p++;
                    while (*p == ' ' || *p == '\t') p++;

                    char texName[64];
                    int len = 0;
                    while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' && len < 31) {
                        texName[len++] = *p++;
                    }
                    texName[len] = '\0';

                    if (len > 0 && _stricmp(texName, "NULL") != 0) {
                        cb(texName);
                    }
                }
            }
        }
    }
    fclose(f);
}

static void ExtractTexturesFromFxs(const char *fxsPath, std::unordered_set<std::string> &outSet) {
    ParseTexturesFromFxs(fxsPath, [&](const char *name) {
        outSet.insert(LowerStr(name));
    });
}

static void ExtractTexturesFromFxs(const char *fxsPath, std::map<std::string, DeclaredTexture> &outMap) {
    ParseTexturesFromFxs(fxsPath, [&](const char *name) {
        std::string lower = LowerStr(name);
        if (outMap.find(lower) == outMap.end()) {
            outMap[lower] = { std::string(name), std::string(fxsPath) };
        }
    });
}

// Pointer to the EffectFolder currently being loaded (for .fxs whitelist checking)
const MyFxManager::EffectFolder *MyFxManager::currentLoadingFolder = nullptr;

std::string MyFxManager::GetGameDirectory() {
    char exePath[MAX_PATH];
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    char *lastSlash = strrchr(exePath, '\\');
    if (lastSlash) {
        *lastSlash = '\0';
    }
    return std::string(exePath);
}

std::vector<MyFxManager::EffectFolder> MyFxManager::CollectEffectFolders() {
    std::vector<EffectFolder> list;
    g_mlIgnoredFolders.clear();
    std::string gameDir = GetGameDirectory();

    // 1. Scan ModLoader directory if it exists
    std::string modloaderDir = gameDir + "\\modloader";
    if (Search::DirectoryExists(modloaderDir.c_str())) {
        ModLoaderFilter mlFilter = ParseModLoaderIni(modloaderDir);

        struct ScanContext {
            std::vector<EffectFolder> *pList;
            const ModLoaderFilter *pFilter;
        } ctx = { &list, &mlFilter };

        Search::ForAllFolders(modloaderDir.c_str(), [](const char *folderName, void *data) {
            auto pCtx = reinterpret_cast<ScanContext*>(data);
            if (!folderName)
                return;

            std::string gameDir = GetGameDirectory();
            char modRoot[MAX_PATH];
            snprintf(modRoot, sizeof(modRoot), "%s\\modloader\\%s", gameDir.c_str(), folderName);

            char pathModelsEffects[MAX_PATH];
            snprintf(pathModelsEffects, sizeof(pathModelsEffects), "%s\\models\\effects", modRoot);

            char pathEffects[MAX_PATH];
            snprintf(pathEffects, sizeof(pathEffects), "%s\\effects", modRoot);

            auto reason = pCtx->pFilter->GetIgnoreReason(folderName);
            if (reason != ModIgnoreReason::NotIgnored) {
                // If this ignored folder actually contains effect files, record it for the log
                bool isEffectMod = (Search::DirectoryExists(pathModelsEffects) && Search::HasAnyEffectFilesRecursively(pathModelsEffects)) ||
                                   (Search::DirectoryExists(pathEffects) && Search::HasAnyEffectFilesRecursively(pathEffects)) ||
                                   Search::HasFileWithExtensionRecursively(modRoot, "fxs");
                if (isEffectMod) {
                    const char *reasonStr = "ignored";
                    switch (reason) {
                    case ModIgnoreReason::DotPrefix: reasonStr = "dot-prefixed"; break;
                    case ModIgnoreReason::IgnoreAll: reasonStr = "modloader.ini IgnoreAllMods"; break;
                    case ModIgnoreReason::ExcludeAll: reasonStr = "modloader.ini ExcludeAllMods"; break;
                    case ModIgnoreReason::IgnoreList: reasonStr = "modloader.ini ignore list"; break;
                    default: break;
                    }
                    char msg[MAX_PATH + 64];
                    snprintf(msg, sizeof(msg), "%s (%s)", folderName, reasonStr);
                    g_mlIgnoredFolders.push_back(msg);
                }
                return;
            }

            auto pList = pCtx->pList;
            bool found = false;
            // Pattern 1: modloader/<mod>/models/effects
            if (Search::DirectoryExists(pathModelsEffects) && Search::HasAnyEffectFilesRecursively(pathModelsEffects)) {
                pList->push_back({ pathModelsEffects, "modloader/<mod>/models/effects", false, {} });
                found = true;
            }
            // Pattern 2: modloader/<mod>/effects
            if (!found && Search::DirectoryExists(pathEffects) && Search::HasAnyEffectFilesRecursively(pathEffects)) {
                pList->push_back({ pathEffects, "modloader/<mod>/effects", false, {} });
                found = true;
            }
            // Pattern 3: modloader/<mod>/ ONLY if it contains actual .fxs particle files
            // (Prevents scanning unrelated vehicle paintjobs or HUD sprites as effects!)
            // Textures in such a loose layout are strictly filtered by a whitelist extracted
            // from all .fxs files in this mod, so unrelated images elsewhere in the mod are never pulled in,
            // while textures in separate subdirectories (e.g. textures/) are properly loaded.
            if (!found && Search::HasFileWithExtensionRecursively(modRoot, "fxs")) {
                EffectFolder ef;
                ef.path = modRoot;
                ef.matchedBy = "modloader/<mod> recursive .fxs (whitelist filtered)";
                ef.filterByFxsWhitelist = true;
                Search::ForAllFilesRecursively(modRoot, "fxs", [](const char *fxsPath, void *data) {
                    auto pSet = reinterpret_cast<std::unordered_set<std::string>*>(data);
                    ExtractTexturesFromFxs(fxsPath, *pSet);
                }, &ef.allowedTextures);
                pList->push_back(ef);
            }
        }, &ctx);
    }

    // 2. Scan root directory fallback (models\effects)
    std::string rootEffects = gameDir + "\\models\\effects";
    if (Search::DirectoryExists(rootEffects.c_str()) && Search::HasAnyEffectFilesRecursively(rootEffects.c_str()))
        list.push_back({ rootEffects, "models/effects", false, {} });

    return list;
}

bool MyFxManager::TextureAlreadyLoaded(const char *name) {
    if (!name)
        return false;
    return customTexturesNames.find(LowerStr(name)) != customTexturesNames.end();
}

bool MyFxManager::IsInCacheDir(const char *path) {
    if (!ddsCacheReady || ddsCacheDir.empty() || !path || !path[0])
        return false;
    size_t n = ddsCacheDir.size();
    if (_strnicmp(path, ddsCacheDir.c_str(), n) != 0)
        return false;
    return path[n] == '\\' || path[n] == '/' || path[n] == '\0';
}

void MyFxManager::LoadPNGTextureCB(const char *path, void *dictionary) {
    if (IsInCacheDir(path)) {
        LogFile::WriteFormattedLine("DDS-CACHE SCAN-SKIP \"%s\" - inside cache dir, not an effect source", path);
        return;
    }
    char texName[MAX_PATH];
    _splitpath(path, NULL, NULL, texName, NULL);
    texName[31] = '\0';
    if (TextureAlreadyLoaded(texName)) {
        LogFile::WriteFormattedLine("Loading PNG texture \"%s\" - texture was already loaded.", path);
        return;
    }
    if (currentLoadingFolder && currentLoadingFolder->filterByFxsWhitelist) {
        if (currentLoadingFolder->allowedTextures.find(LowerStr(texName)) == currentLoadingFolder->allowedTextures.end()) {
            LogFile::WriteFormattedLine("FILTERED \"%s\" - not referenced by any .fxs in this mod, skipped.", path);
            return;
        }
    }
    statPngSeen++;

    // ---- Experimental DDS cache: HIT path ----
    // If a fresh "<cacheDir>\<texName>.dds" exists, load it instead of the PNG.
    // Any failure falls through to the classic PNG path (never a black texture).
    if (ddsCacheReady) {
        char cachePath[MAX_PATH];
        if (DdsCache::BuildCachePath(ddsCacheDir.c_str(), texName, cachePath, sizeof(cachePath))) {
            unsigned long long srcSize = 0, cacheSize = 0;
            if (DdsCache::IsCacheFresh(path, cachePath, &srcSize, &cacheSize)) {
                char ddsNoExt[MAX_PATH];
                if (DdsCache::StripExtension(cachePath, ddsNoExt, sizeof(ddsNoExt))) {
                    ULONGLONG t0 = GetTickCount64();
                    RwTexture *tex = RwD3D9DDSTextureRead(ddsNoExt, NULL);
                    if (tex && !tex->raster) {
                        // Defensive: a texture without raster is unusable -
                        // drop it and take the PNG fallback below (no leak).
                        RwTextureDestroy(tex);
                        tex = nullptr;
                    }
                    ULONGLONG dt = GetTickCount64() - t0;
                    statDdsMs += dt;
                    if (tex) {
                        statCacheHit++;
                        statDdsBytes += cacheSize;
                        if (tex->raster && (tex->raster->width > 1024 || tex->raster->height > 1024)) {
                            LogFile::WriteFormattedLine("[PERF] Oversized cached particle texture \"%s\" (%dx%d > 1024x1024). Consider setting MaxDim=1024 in effects-loader.ini to prevent GPU fillrate lag during dense particle bursts.",
                                texName, tex->raster->width, tex->raster->height);
                        }
                        LogFile::WriteFormattedLine(
                            "DDS-CACHE HIT tex=\"%s\" src=\"%s\" (%llu bytes) cache=\"%s\" (%llu bytes, %llu ms) - DDS loaded OK",
                            texName, path, srcSize, cachePath, cacheSize, dt);
                        // Same filter convention as the PNG path (parity: uncompressed
                        // cache has no mipmaps, so cFormat&0x80 is clear -> LINEAR).
                        if ((tex->raster->cFormat & 0x80) == 0)
                            RwTextureSetFilterMode(tex, rwFILTERLINEAR);
                        else
                            RwTextureSetFilterMode(tex, rwFILTERLINEARMIPLINEAR);
                        RwTextureSetAddressing(tex, rwTEXTUREADDRESSWRAP);
                        customTexturesNames.insert(LowerStr(texName));
                        RwTexture *oldTex = RwTexDictionaryFindNamedTexture(reinterpret_cast<TxdDef *>(dictionary)->m_pRwDictionary, texName);
                        if (oldTex) {
                            LogFile::WriteFormattedLine("Removing old texture \"%s\"", texName);
                            RwTexDictionaryRemoveTexture(oldTex);
                            RwTextureDestroy(oldTex);
                        }
                        RwTexDictionaryAddTexture(reinterpret_cast<TxdDef *>(dictionary)->m_pRwDictionary, tex);
                        return;
                    }
                    statDdsHitFallbackToPng++;
                    LogFile::WriteFormattedLine(
                        "DDS-CACHE HIT-FAILED tex=\"%s\" cache=\"%s\" (%llu bytes, %llu ms) - RwD3D9DDSTextureRead returned NULL, falling back to PNG",
                        texName, cachePath, cacheSize, dt);
                } else {
                    LogFile::WriteFormattedLine("DDS-CACHE SKIP tex=\"%s\" - StripExtension failed for \"%s\"",
                        texName, cachePath);
                }
            } else {
                statCacheMiss++;
                unsigned long long sSize = 0;
                DdsCache::GetFileInfo(path, &sSize, NULL);
                LogFile::WriteFormattedLine("DDS-CACHE MISS tex=\"%s\" src=\"%s\" (%llu bytes) - no fresh cache, loading PNG",
                    texName, path, sSize);
            }
        }
    }

    LogFile::WriteFormattedLine("Loading PNG texture \"%s\"", path);
    ULONGLONG pngT0 = GetTickCount64();
    int width, height, depth, flags;
    RwImage *image = RtPNGImageRead(path);
    if (!image) {
        LogFile::WriteFormattedLine("\"%s\" - FAILED to load texture", path);
        return;
    }
    if (image->width > 1024 || image->height > 1024) {
        LogFile::WriteFormattedLine("[PERF] Oversized particle texture \"%s\" (%dx%d > 1024x1024). Consider setting MaxDim=1024 in effects-loader.ini to prevent GPU fillrate lag during dense particle bursts.",
            texName, image->width, image->height);
    }
    ULONGLONG pngDt = GetTickCount64() - pngT0;
    statPngMs += pngDt;
    {
        unsigned long long sSize = 0;
        if (DdsCache::GetFileInfo(path, &sSize, NULL)) {
            statPngBytes += sSize;
            LogFile::WriteFormattedLine("DDS-CACHE PNG-DECODE tex=\"%s\" %dx%d depth=%d (%llu bytes on disk, %llu ms decode)",
                texName, image->width, image->height, image->depth, sSize, pngDt);
        } else {
            LogFile::WriteFormattedLine("DDS-CACHE PNG-DECODE tex=\"%s\" %dx%d depth=%d (%llu ms decode, size unknown)",
                texName, image->width, image->height, image->depth, pngDt);
        }
    }
    // ---- V2-eng WRITE path: RGBA -> (MaxDim downscale) -> decide -> BGRA/DXT1/DXT5 ----
    // Cache from the pristine decoded image BEFORE FindRasterFormat may convert it.
    if (ddsCacheReady) {
        char cachePath[MAX_PATH];
        if (DdsCache::BuildCachePath(ddsCacheDir.c_str(), texName, cachePath, sizeof(cachePath))) {
            // Repack into stride-free RGBA (image->stride may include padding).
            // Dims are capped: hostile/broken PNGs must never cause multi-GB allocs.
            std::vector<unsigned char> packed;
            const unsigned char *rgba = nullptr;
            if ((image->depth == 32 || image->depth == 24 ||
                 (image->depth == 8 && image->palette)) &&
                image->width > 0 && image->height > 0 &&
                image->width <= 4096 && image->height <= 4096) {
                packed.resize((size_t)image->width * image->height * 4);
                const int sbpp = image->depth == 32 ? 4 : (image->depth == 24 ? 3 : 1);
                for (int y = 0; y < image->height; y++) {
                    const unsigned char *row = image->cpPixels + (size_t)y * image->stride;
                    for (int x = 0; x < image->width; x++) {
                        unsigned char *dst = &packed[((size_t)y * image->width + x) * 4];
                        if (sbpp == 4) {
                            dst[0] = row[x * 4 + 0]; dst[1] = row[x * 4 + 1];
                            dst[2] = row[x * 4 + 2]; dst[3] = row[x * 4 + 3];
                        } else if (sbpp == 3) {
                            dst[0] = row[x * 3 + 0]; dst[1] = row[x * 3 + 1];
                            dst[2] = row[x * 3 + 2]; dst[3] = 0xFF;
                        } else {
                            const RwRGBA &pal = image->palette[row[x]];
                            dst[0] = pal.red; dst[1] = pal.green;
                            dst[2] = pal.blue; dst[3] = pal.alpha;
                        }
                    }
                }
                rgba = packed.data();
            }
            if (!rgba) {
                statCacheWriteSkip++;
                LogFile::WriteFormattedLine("DDS-CACHE WRITE-SKIP tex=\"%s\" depth=%d %dx%d (only 8/24/32-bit, dims 1..4096)",
                    texName, image->depth, image->width, image->height);
            } else {
                int ew = image->width, eh = image->height;
                std::vector<unsigned char> scaled;
                int effMax = g_cfgMaxDim;
                if (g_cfgFidelity == 2 && effMax == 0)
                    effMax = 256; // fast tier default clamp
                if (effMax > 0 && (ew > effMax || eh > effMax)) {
                    int ew2 = (ew >= eh) ? effMax : (ew * effMax / eh);
                    int eh2 = (ew >= eh) ? (eh * effMax / ew) : effMax;
                    if (ew2 < 1)
                        ew2 = 1;
                    if (eh2 < 1)
                        eh2 = 1;
                    scaled.resize((size_t)ew2 * eh2 * 4);
                    DdsCache::BoxDownscale(rgba, ew, eh, scaled.data(), ew2, eh2);
                    LogFile::WriteFormattedLine("DDS-CACHE DOWNSCALE tex=\"%s\" %dx%d -> %dx%d (MaxDim=%d)",
                        texName, ew, eh, ew2, eh2, effMax);
                    rgba = scaled.data();
                    ew = ew2;
                    eh = eh2;
                }
                DdsCache::DecideResult dec = DdsCache::DecideFormat(
                    rgba, ew, eh, path, g_cfgFidelity, g_cfgMipsNPOT != 0, 32);
                if (dec.skipCache) {
                    statCacheWriteSkip++;
                    LogFile::WriteFormattedLine("DDS-CACHE WRITE-SKIP tex=\"%s\" (%s)",
                        texName, dec.reason.c_str());
                } else {
                    ULONGLONG wT0 = GetTickCount64();
                    unsigned long wBytes = DdsCache::WriteCachedDDS(cachePath, rgba, ew, eh, dec);
                    ULONGLONG wDt = GetTickCount64() - wT0;
                    if (wBytes) {
                        statCacheWriteOk++;
                        LogFile::WriteFormattedLine("DDS-CACHE WRITE tex=\"%s\" -> \"%s\" (%s mips%d, %s, %dx%d, %lu bytes, %llu ms)",
                            texName, cachePath, DdsCache::FormatName(dec.format),
                            dec.mipLevels, dec.reason.c_str(), ew, eh, wBytes, wDt);
                    } else {
                        statCacheWriteFail++;
                        LogFile::WriteFormattedLine("DDS-CACHE WRITE-FAILED tex=\"%s\" -> \"%s\" (%s)",
                            texName, cachePath, dec.reason.c_str());
                    }
                }
            }
        }
    }
    RwImageFindRasterFormat(image, 4, &width, &height, &depth, &flags);
    RwRaster *raster = RwRasterCreate(width, height, depth, flags);
    if (!raster) {
        RwImageDestroy(image);
        LogFile::WriteFormattedLine("\"%s\" - FAILED to create raster", path);
        return;
    }
    RwRasterSetFromImage(raster, image);
    RwImageDestroy(image);
    RwTexture *tex = RwTextureCreate(raster);
    if (!tex) {
        RwRasterDestroy(raster);
        LogFile::WriteFormattedLine("\"%s\" - FAILED to create texture from raster", path);
        return;
    }
    RwTextureSetName(tex, texName);
    if ((tex->raster->cFormat & 0x80) == 0)
        RwTextureSetFilterMode(tex, rwFILTERLINEAR);
    else
        RwTextureSetFilterMode(tex, rwFILTERLINEARMIPLINEAR);
    RwTextureSetAddressing(tex, rwTEXTUREADDRESSWRAP);
    customTexturesNames.insert(LowerStr(texName));
    RwTexture *oldTex = RwTexDictionaryFindNamedTexture(reinterpret_cast<TxdDef *>(dictionary)->m_pRwDictionary, texName);
    if (oldTex) {
        LogFile::WriteFormattedLine("Removing old texture \"%s\"", texName);
        RwTexDictionaryRemoveTexture(oldTex);
        RwTextureDestroy(oldTex);
    }
    RwTexDictionaryAddTexture(reinterpret_cast<TxdDef *>(dictionary)->m_pRwDictionary, tex);
}

void MyFxManager::LoadDDSTextureCB(const char *path, void *dictionary) {
    if (IsInCacheDir(path)) {
        LogFile::WriteFormattedLine("DDS-CACHE SCAN-SKIP \"%s\" - inside cache dir, not an effect source", path);
        return;
    }
    char texName[MAX_PATH];
    _splitpath(path, NULL, NULL, texName, NULL);
    texName[31] = '\0';
    if (TextureAlreadyLoaded(texName)) {
        LogFile::WriteFormattedLine("Loading DDS texture \"%s\" - texture was already loaded.", path);
        return;
    }
    if (currentLoadingFolder && currentLoadingFolder->filterByFxsWhitelist) {
        if (currentLoadingFolder->allowedTextures.find(LowerStr(texName)) == currentLoadingFolder->allowedTextures.end()) {
            LogFile::WriteFormattedLine("FILTERED \"%s\" - not referenced by any .fxs in this mod, skipped.", path);
            return;
        }
    }
    LogFile::WriteFormattedLine("Loading DDS texture \"%s\"", path);
    // StripExtension (not a naive last-dot cut): a dot inside a directory
    // name must not be mistaken for the file extension.
    char ddsPath[MAX_PATH];
    if (!DdsCache::StripExtension(path, ddsPath, sizeof(ddsPath))) {
        LogFile::WriteFormattedLine("\"%s\" - FAILED to load texture (no extension)", path);
        return;
    }
    RwTexture *tex = RwD3D9DDSTextureRead(ddsPath, NULL);
    if (!tex) {
        LogFile::WriteFormattedLine("\"%s\" - FAILED to load texture", path);
        return;
    }
    if (!tex->raster) {
        RwTextureDestroy(tex);
        LogFile::WriteFormattedLine("\"%s\" - FAILED: DDS texture has no raster", path);
        return;
    }
    if (tex->raster->width > 1024 || tex->raster->height > 1024) {
        LogFile::WriteFormattedLine("[PERF] Oversized particle texture \"%s\" (%dx%d > 1024x1024). Consider downscaling to <= 1024x1024 to prevent GPU fillrate lag during dense particle bursts.",
            texName, tex->raster->width, tex->raster->height);
    }
    if ((tex->raster->cFormat & 0x80) == 0)
        RwTextureSetFilterMode(tex, rwFILTERLINEAR);
    else
        RwTextureSetFilterMode(tex, rwFILTERLINEARMIPLINEAR);
    RwTextureSetAddressing(tex, rwTEXTUREADDRESSWRAP);
    customTexturesNames.insert(LowerStr(texName));
    RwTexture *oldTex = RwTexDictionaryFindNamedTexture(reinterpret_cast<TxdDef *>(dictionary)->m_pRwDictionary, texName);
    if (oldTex) {
        LogFile::WriteFormattedLine("Removing old texture \"%s\"", texName);
        RwTexDictionaryRemoveTexture(oldTex);
        RwTextureDestroy(oldTex);
    }
    RwTexDictionaryAddTexture(reinterpret_cast<TxdDef *>(dictionary)->m_pRwDictionary, tex);
}

unsigned int MyFxManager::GetSystemNameKey(int file) {
    unsigned int posn = CFileMgr::Tell(file);
    char line[256];
    CFileMgr::ReadLine(file, line, 256);
    unsigned int version = 0;
    sscanf(line, "%d", &version);
    CFileMgr::ReadLine(file, line, 256);
    if (version > 100)
        CFileMgr::ReadLine(file, line, 256);
    CFileMgr::ReadLine(file, line, 256);
    tempSystemName[0] = '\0';
    sscanf(line, "%*s %255s", tempSystemName);
    CFileMgr::Seek(file, posn, SEEK_SET);
    return CKeyGen::GetUppercaseKey(tempSystemName);
}

bool MyFxManager::IsThisParticleLoaded(unsigned int key) {
    return customParticlesKeys.find(key) != customParticlesKeys.end();
}

void MyFxManager::LoadFxSystemFileCB(const char *path, void *data) {
    char linebuf[256];
    int file = CFileMgr::OpenFile(const_cast<char*>(path), "r");
    if (file != 0) {
        char *pHeader = nullptr;
        // Skip leading blank lines / whitespace until the first non-empty line
        while (CFileMgr::ReadLine(file, linebuf, 256)) {
            char *p = linebuf;
            // Skip UTF-8 BOM if present
            if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) {
                p += 3;
            }
            while (*p == ' ' || *p == '\t') p++;
            if (*p != '\r' && *p != '\n' && *p != '\0') {
                pHeader = p;
                break;
            }
        }

        bool systemLoaded = false;
        if (pHeader && !strncmp(pHeader, "FX_SYSTEM_DATA:", 15)) {
            unsigned int key = GetSystemNameKey(file);
            if (!IsThisParticleLoaded(key)) {
                customParticlesKeys.insert(key);
                LogFile::WriteFormattedLine("Loading custom system \"%s\" (\"%s\")", path, tempSystemName);
                reinterpret_cast<MyFxManager *>(data)->LoadFxSystemBP(const_cast<char*>(path), file);
                systemLoaded = true;
            }
            else {
                LogFile::WriteFormattedLine("Loading custom system \"%s\" (\"%s\") - this system was already loaded.", path, tempSystemName);
            }
        }
        else {
            LogFile::WriteFormattedLine("Skipping \"%s\" - header is not FX_SYSTEM_DATA: (found: \"%s\")", path, pHeader ? pHeader : "<empty file>");
        }
        CFileMgr::CloseFile(file);
        if (systemLoaded) {
            ExtractTexturesFromFxs(path, s_declaredFxsTextures);
        }
    }
    else {
        LogFile::WriteFormattedLine("Error: Failed to open file \"%s\"", path);
    }
}

bool MyFxManager::LoadProject(char *fxFileName) {
    std::string gameDir = GetGameDirectory();
    auto effectFolders = CollectEffectFolders();

    // Determine the optimal location for log.txt:
    // 1. If a custom effects folder exists (e.g. modloader\Essential.Effects Loader\models\effects), write log.txt right inside it!
    // 2. Else if root models\effects already exists, write in models\effects\log.txt
    // 3. Otherwise, write effects-loader.log in game root without creating extra folders
    std::string logFilePath;
    if (!effectFolders.empty()) {
        logFilePath = effectFolders[0].path + "\\log.txt";
    } else if (Search::DirectoryExists((gameDir + "\\models\\effects").c_str())) {
        logFilePath = gameDir + "\\models\\effects\\log.txt";
    } else {
        logFilePath = gameDir + "\\effects-loader.log";
    }

    LogFile::Open(logFilePath.c_str());

    char txdFileName[MAX_PATH];
    strcpy(txdFileName, fxFileName);
    strcpy(&txdFileName[strlen(txdFileName) - 4], "PC.txd");
    this->m_nFxTxdIndex = CTxdStore::AddTxdSlot("fx");
    CTxdStore::LoadTxd(this->m_nFxTxdIndex, txdFileName);

    LogFile::WriteLine("========================================");
    LogFile::WriteLine("EffectsLoader - Initializing custom effects");
    LogFile::WriteFormattedLine("Log File: \"%s\"", logFilePath.c_str());
    LogFile::WriteFormattedLine("Game Directory: \"%s\"", gameDir.c_str());
    // Log FxMemoryPool expansion info
    LogFile::WriteFormattedLine("FxMemoryPool: %u MB (%u bytes) [Original: 1 MB, configured via effects-loader.ini]",
        configuredPoolSizeMB, configuredPoolSizeMB * 1024 * 1024);

    // ---- Experimental DDS cache setup ----
    statPngSeen = statCacheHit = statCacheMiss = 0;
    statCacheWriteOk = statCacheWriteSkip = statCacheWriteFail = 0;
    statDdsHitFallbackToPng = 0;
    statPngBytes = statDdsBytes = statPngMs = statDdsMs = 0;
    ddsCacheReady = false;
    ddsCacheDir.clear();
    LoadCacheConfig();
    const char *fidName = g_cfgFidelity == 0 ? "lossless" : (g_cfgFidelity == 2 ? "fast" : "balanced");
    if (DdsCache::ENABLED && g_cfgCacheOn) {
        // V1.2: top-level dir, OUTSIDE both scan trees (modloader/* and
        // models\effects) - keeps the game install clean and the cache can
        // never be picked up as an effect source (no exclusion hack needed).
        // ini CacheDir overrides the default (absolute, or relative to gameDir).
        ddsCacheDir = ResolveCacheDir(gameDir);
        if (ddsCacheDir.size() + 64 >= MAX_PATH) {
            // All cache file paths derive from this dir - refuse upfront
            // rather than act on truncated paths later.
            LogFile::WriteFormattedLine("DDS-CACHE DISABLED for this session - game path too long (%zu chars)",
                gameDir.size());
            ddsCacheDir.clear();
        } else if (DdsCache::EnsureCacheDir(ddsCacheDir.c_str())) {
            ddsCacheReady = true;
            LogFile::WriteFormattedLine("DDS-CACHE enabled, dir=\"%s\"%s (v2-eng fidelity=%s mipsNPOT=%d maxDim=%d)",
                ddsCacheDir.c_str(), g_cfgCacheDir.empty() ? "" : " [CacheDir override]",
                fidName, g_cfgMipsNPOT, g_cfgMaxDim);
            // Cache format self-clean: wipe once when the stored version
            // mismatches (stale-format files would otherwise HIT-FAILED forever).
            {
                char verPath[MAX_PATH];
                snprintf(verPath, sizeof(verPath), "%s\\cache.ver", ddsCacheDir.c_str());
                int cacheVer = 0;
                FILE *vf = fopen(verPath, "r");
                if (vf) {
                    fscanf(vf, "%d", &cacheVer);
                    fclose(vf);
                }
                if (cacheVer != DdsCache::CACHE_VERSION) {
                    char findPat[MAX_PATH];
                    snprintf(findPat, sizeof(findPat), "%s\\*.dds", ddsCacheDir.c_str());
                    WIN32_FIND_DATAA fd;
                    HANDLE hFind = FindFirstFileA(findPat, &fd);
                    int wiped = 0;
                    if (hFind != INVALID_HANDLE_VALUE) {
                        do {
                            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.')
                                continue;
                            char full[MAX_PATH];
                            snprintf(full, sizeof(full), "%s\\%s", ddsCacheDir.c_str(), fd.cFileName);
                            if (DeleteFileA(full))
                                wiped++;
                        } while (FindNextFileA(hFind, &fd));
                        FindClose(hFind);
                    }
                    vf = fopen(verPath, "w");
                    if (vf) {
                        fprintf(vf, "%d", DdsCache::CACHE_VERSION);
                        fclose(vf);
                    }
                    LogFile::WriteFormattedLine("DDS-CACHE VERSION wipe: stored v%d -> v%d, deleted %d stale .dds",
                        cacheVer, DdsCache::CACHE_VERSION, wiped);
                }
            }
            // One-time migration from legacy locations (models\effects\cache, effects-loader-cache).
            std::vector<std::string> oldLocations = {
                gameDir + "\\models\\effects\\cache",
                gameDir + "\\effects-loader-cache"
            };
            for (const auto &oldCache : oldLocations) {
                if (_stricmp(oldCache.c_str(), ddsCacheDir.c_str()) != 0 &&
                    Search::DirectoryExists(oldCache.c_str())) {
                    char findPat[MAX_PATH];
                    snprintf(findPat, sizeof(findPat), "%s\\*.dds", oldCache.c_str());
                    WIN32_FIND_DATAA fd;
                    HANDLE hFind = FindFirstFileA(findPat, &fd);
                    int moved = 0, droppedDup = 0, failed = 0;
                    if (hFind != INVALID_HANDLE_VALUE) {
                        do {
                            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.')
                                continue;
                            char src[MAX_PATH], dst[MAX_PATH];
                            if (!DdsCache::JoinPath(src, sizeof(src), oldCache.c_str(), fd.cFileName) ||
                                !DdsCache::JoinPath(dst, sizeof(dst), ddsCacheDir.c_str(), fd.cFileName)) {
                                failed++; // path too long - never touch a truncated path
                                continue;
                            }
                            if (GetFileAttributesA(dst) == INVALID_FILE_ATTRIBUTES) {
                                if (MoveFileA(src, dst))
                                    moved++;
                                else {
                                    failed++;
                                    LogFile::WriteFormattedLine("DDS-CACHE MIGRATE FAILED \"%s\" (err=%lu)", src, GetLastError());
                                }
                            } else if (DeleteFileA(src)) {
                                droppedDup++; // new location already has it
                            } else {
                                failed++;
                            }
                        } while (FindNextFileA(hFind, &fd));
                        FindClose(hFind);
                    }
                    RemoveDirectoryA(oldCache.c_str()); // succeeds only when empty
                    LogFile::WriteFormattedLine("DDS-CACHE MIGRATE old=\"%s\" moved=%d dropped-dup=%d failed=%d",
                        oldCache.c_str(), moved, droppedDup, failed);
                }
            }
        } else {
            LogFile::WriteFormattedLine("DDS-CACHE DISABLED for this session - cannot create dir=\"%s\"", ddsCacheDir.c_str());
            ddsCacheDir.clear();
        }
    } else if (!DdsCache::ENABLED) {
        LogFile::WriteLine("DDS-CACHE disabled by build flag.");
    } else {
        LogFile::WriteLine("DDS-CACHE disabled by effects-loader.ini ([Cache] Enabled=0).");
    }

    // Discovery happened silently before the log existed - echo the results now.
    if (Search::DirectoryExists((gameDir + "\\modloader").c_str()))
        LogFile::WriteFormattedLine("Scanning ModLoader: \"%s\\modloader\"", gameDir.c_str());
    else
        LogFile::WriteLine("ModLoader directory not found.");
    LogFile::WriteFormattedLine("Discovered %zu effect source folder(s):", effectFolders.size());
    for (const auto &folder : effectFolders)
        LogFile::WriteFormattedLine(" - %s  [%s]", folder.path.c_str(), folder.matchedBy);
    if (!g_mlIgnoredFolders.empty()) {
        LogFile::WriteFormattedLine("ModLoader ignored effect folder(s) (%zu):", g_mlIgnoredFolders.size());
        for (const auto &ignored : g_mlIgnoredFolders)
            LogFile::WriteFormattedLine(" - [IGNORED] %s", ignored.c_str());
    }

    // 1. Load textures (DDS and PNG) from all discovered effect paths.
    //    Pattern-3 ("loose layout") folders filter textures against the mod's .fxs whitelist;
    //    dedicated effect folders keep the plain recursive sweep (also covers packs that
    //    retexture stock systems without shipping any .fxs).
    void *txd = CTxdStore::ms_pTxdPool->GetAt(this->m_nFxTxdIndex);
    for (const auto &folder : effectFolders) {
        currentLoadingFolder = &folder;
        Search::ForAllFilesRecursively(folder.path.c_str(), "dds", LoadDDSTextureCB, txd);
        Search::ForAllFilesRecursively(folder.path.c_str(), "png", LoadPNGTextureCB, txd);
    }
    currentLoadingFolder = nullptr;

    CTxdStore::AddRef(this->m_nFxTxdIndex);
    CTxdStore::PushCurrentTxd();
    CTxdStore::SetCurrentTxd(this->m_nFxTxdIndex);

    // 2. Load custom fx system files (.fxs) from all discovered effect paths recursively
    s_declaredFxsTextures.clear();
    for (const auto &folder : effectFolders) {
        Search::ForAllFilesRecursively(folder.path.c_str(), "fxs", LoadFxSystemFileCB, this);
    }

    // 3. Load default effects from effects.fxp
    int file = CFileMgr::OpenFile(fxFileName, "r");
    if (file != 0) {
        char linebuf[256];
        char header[32] = { 0 };
        CFileMgr::ReadLine(file, linebuf, 256); // FX_PROJECT_DATA:
        CFileMgr::ReadLine(file, linebuf, 256); // 
        CFileMgr::ReadLine(file, linebuf, 256); // FX_SYSTEM_DATA:
        header[0] = '\0';
        sscanf(linebuf, "%31s", header);
        if (!strncmp(header, "FX_SYSTEM_DATA:", 15)) {
            do {
                unsigned int key = GetSystemNameKey(file);
                if (!IsThisParticleLoaded(key)) {
                    LogFile::WriteFormattedLine("Loading default system \"%s\"", tempSystemName);
                    this->LoadFxSystemBP(fxFileName, file);
                    CFileMgr::ReadLine(file, linebuf, 256); // 
                    CFileMgr::ReadLine(file, linebuf, 256); // FX_SYSTEM_DATA:
                    header[0] = '\0';
                    sscanf(linebuf, "%31s", header);
                }
                else {
                    LogFile::WriteFormattedLine("Loading default system \"%s\" - this system was already loaded.", tempSystemName);
                    while (CFileMgr::ReadLine(file, linebuf, 256)) {
                        header[0] = '\0';
                        sscanf(linebuf, "%31s", header);
                        if (!strncmp(header, "FX_SYSTEM_DATA:", 15))
                            break;
                    }
                }
            } while (!strncmp(header, "FX_SYSTEM_DATA:", 15));
        }
        CFileMgr::CloseFile(file);
    }

    // Texture integrity check: verify all textures declared in loaded .fxs files
    if (!s_declaredFxsTextures.empty()) {
        TxdDef *pFxTxd = reinterpret_cast<TxdDef *>(CTxdStore::ms_pTxdPool->GetAt(this->m_nFxTxdIndex));
        RwTexDictionary *pDict = pFxTxd ? pFxTxd->m_pRwDictionary : nullptr;
        if (pDict) {
            int missingCount = 0;
            for (const auto &pair : s_declaredFxsTextures) {
                const std::string &lowerKey = pair.first;
                const DeclaredTexture &decl = pair.second;
                RwTexture *tex = RwTexDictionaryFindNamedTexture(pDict, lowerKey.c_str());
                if (!tex) {
                    missingCount++;
                    LogFile::WriteFormattedLine("[WARN] Missing texture \"%s\" declared in \"%s\" (not found in mod textures or stock effectsPC.txd)",
                        decl.originalName.c_str(), decl.fxsSourcePath.c_str());
                }
            }
            if (missingCount == 0) {
                LogFile::WriteFormattedLine("Texture integrity check: all %zu declared .fxs texture(s) successfully verified.",
                    s_declaredFxsTextures.size());
            } else {
                LogFile::WriteFormattedLine("[WARN] Texture integrity check: %d of %zu declared texture(s) MISSING!",
                    missingCount, s_declaredFxsTextures.size());
            }
        }
    }

    CTxdStore::PopCurrentTxd();

    // 4. Memory pool diagnostics: sample usage before Optimise() reallocates to fit
    unsigned int poolUsedBytes = this->m_pool.position;
    unsigned int poolTotalBytes = this->m_pool.size;
    float usagePct = poolTotalBytes > 0 ? ((float)poolUsedBytes / (float)poolTotalBytes) * 100.0f : 0.0f;
    float usedMB = (float)poolUsedBytes / (1024.0f * 1024.0f);
    float totalMB = (float)poolTotalBytes / (1024.0f * 1024.0f);
    float freeMB = totalMB - usedMB;

    this->m_pool.Optimise();

    LogFile::WriteFormattedLine("FxMemoryPool Health: %.2f MB / %.2f MB used (%.1f%%), %.2f MB free before Optimise()",
        usedMB, totalMB, usagePct, freeMB);
    if (usagePct > 85.0f) {
        LogFile::WriteFormattedLine("[WARNING] FxMemoryPool usage is high (%.1f%% > 85%%). Consider increasing FxPoolSizeMB in effects-loader.ini (current: %u MB).",
            usagePct, configuredPoolSizeMB);
    }
    // V1.2: sweep stale cache files BEFORE the name lists are cleared below.
    // A cache .dds whose texName was not referenced by any source this session
    // (mod removed/disabled) is deleted so the cache never grows stale.
    int sweepDeleted = 0;
    if (ddsCacheReady) {
        char findPat[MAX_PATH];
        snprintf(findPat, sizeof(findPat), "%s\\*.dds", ddsCacheDir.c_str());
        WIN32_FIND_DATAA fd;
        HANDLE hFind = FindFirstFileA(findPat, &fd);
        if (hFind != INVALID_HANDLE_VALUE) {
            do {
                if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.')
                    continue;
                char base[MAX_PATH];
                strncpy(base, fd.cFileName, sizeof(base) - 1);
                base[sizeof(base) - 1] = '\0';
                char *dot = strrchr(base, '.');
                if (dot)
                    *dot = '\0';
                // Set holds lowercased keys - normalize the file side too.
                bool referenced = customTexturesNames.find(LowerStr(base)) != customTexturesNames.end();
                if (!referenced) {
                    char full[MAX_PATH];
                    if (!DdsCache::JoinPath(full, sizeof(full), ddsCacheDir.c_str(), fd.cFileName))
                        continue; // path too long - never delete via a truncated path
                    if (DeleteFileA(full)) {
                        sweepDeleted++;
                        LogFile::WriteFormattedLine("DDS-CACHE SWEEP deleted stale \"%s\"", full);
                    }
                }
            } while (FindNextFileA(hFind, &fd));
            FindClose(hFind);
        }
    }
    customParticlesKeys.clear();
    customTexturesNames.clear();
    s_declaredFxsTextures.clear();

    LogFile::WriteFormattedLine("DDS-CACHE SUMMARY: pngSeen=%d hit=%d miss=%d writeOk=%d writeSkip=%d writeFail=%d ddsHitFallbackToPng=%d sweep=%d",
        statPngSeen, statCacheHit, statCacheMiss, statCacheWriteOk, statCacheWriteSkip, statCacheWriteFail, statDdsHitFallbackToPng, sweepDeleted);
    LogFile::WriteFormattedLine("DDS-CACHE SUMMARY: pngBytes=%llu ddsBytes=%llu pngDecodeMs=%llu ddsLoadMs=%llu",
        statPngBytes, statDdsBytes, statPngMs, statDdsMs);
    LogFile::WriteLine("EffectsLoader - Finished loading effects");
    LogFile::WriteLine("========================================");
    LogFile::Close();
    return true;
}