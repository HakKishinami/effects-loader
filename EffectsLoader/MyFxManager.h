#pragma once
#include <vector>
#include <string>
#include "game_sa\FxManager_c.h"

/*  FXP - FX Project
    FXS - FX System
    Load custom effects (fxs files from modloader/ or models/effects/) > Load effects.fxp file (ignore effect if it was already loaded as custom) */

class MyFxManager : public FxManager_c {
    static std::vector<unsigned int> customParticlesKeys;
    static std::vector<std::string> customTexturesNames;
    static char tempSystemName[256];

    // Retrieves absolute game root directory from executable path
    static std::string GetGameDirectory();

    // Scans ModLoader and game root to discover all valid custom effect source directories
    static std::vector<std::string> CollectEffectFolders();

    // Checks if a texture name has already been registered
    static bool TextureAlreadyLoaded(const char *name);

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