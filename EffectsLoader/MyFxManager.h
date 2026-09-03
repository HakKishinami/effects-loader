#pragma once
#include <vector>
#include <string>
#include <unordered_set>
#include "game_sa\FxManager_c.h"

/*  FXP - FX Project
    FXS - FX System
    Load custom effects (fxs files from modloader/ or models/effects/) > Load effects.fxp file (ignore effect if it was already loaded as custom) */

class MyFxManager : public FxManager_c {
    // Dedup sets (O(1) lookup). Texture keys are stored LOWERCASED - the same
    // rule the old linear _stricmp scan enforced. Game-facing uses (dictionary
    // names, cache file names, log lines) keep the ORIGINAL case untouched:
    // .fxs files reference mixed-case names like "Spark1"/"sphere_CJ".
    static std::unordered_set<unsigned int> customParticlesKeys;
    static std::unordered_set<std::string> customTexturesNames;
    static char tempSystemName[256];

    // ---- Experimental DDS cache (v1, see DdsCache.h) ----
    static std::string ddsCacheDir;   // "<gameDir>\models\effects\cache", "" = disabled
    static bool ddsCacheReady;
    // Session counters for the end-of-load summary block in the log.
    static int statPngSeen;
    static int statCacheHit;
    static int statCacheMiss;
    static int statCacheWriteOk;
    static int statCacheWriteSkip;
    static int statCacheWriteFail;
    static int statDdsHitFallbackToPng; // cache .dds failed to load -> PNG fallback
    static unsigned long long statPngBytes;
    static unsigned long long statDdsBytes;
    static unsigned long long statPngMs;
    static unsigned long long statDdsMs;

    // Retrieves absolute game root directory from executable path
    static std::string GetGameDirectory();

    // One discovered custom-effect source folder plus the layout rule that found it
    // ("modloader/<mod>/models/effects", "modloader/<mod>/effects",
    //  "modloader/<mod> recursive .fxs" or "models/effects"), for the log.
    // "pairedTextures": loose Pattern-3 folders only take textures from directories
    // that also contain a .fxs, so unrelated images elsewhere in the mod stay untouched.
    struct EffectFolder {
        std::string path;
        const char *matchedBy;
        bool pairedTextures;
    };

    // Scans ModLoader and game root for all valid custom effect source directories.
    // Stays silent on purpose: it runs BEFORE LogFile::Open (the log location depends
    // on its result), so anything written here would be lost - the caller echoes the
    // results after opening the log.
    static std::vector<EffectFolder> CollectEffectFolders();

    // Checks if a texture name has already been registered
    static bool TextureAlreadyLoaded(const char *name);

    // True when "path" lives inside the DDS cache dir (V1.1: the cache must
    // never be scanned as an effect source - it only holds generated .dds).
    static bool IsInCacheDir(const char *path);

    // Callbacks for texture loading
    static void LoadPNGTextureCB(const char *path, void *dictionary);
    static void LoadDDSTextureCB(const char *path, void *dictionary);

    // Helper functions for particle system parsing
    static unsigned int GetSystemNameKey(int file);
    static bool IsThisParticleLoaded(unsigned int key);
    static void LoadFxSystemFileCB(const char *path, void *data);
public:
    // Hooks FxManager_c::LoadProject (0x5C2420)
    bool LoadProject(char *fxFileName);
};