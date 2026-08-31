#include "MyFxManager.h"
#include "Search.h"
#include "LogFile.h"
#include <stdio.h>
#include <Windows.h>
#include "game_sa\FxManager_c.h"
#include "game_sa\CTxdStore.h"
#include "game_sa\CFileMgr.h"
#include "game_sa\CKeyGen.h"

std::vector<unsigned int> MyFxManager::customParticlesKeys;
char MyFxManager::tempSystemName[256];
std::vector<std::string> MyFxManager::customTexturesNames;

std::string MyFxManager::GetGameDirectory() {
    char exePath[MAX_PATH];
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    char *lastSlash = strrchr(exePath, '\\');
    if (lastSlash) {
        *lastSlash = '\0';
    }
    return std::string(exePath);
}

std::vector<std::string> MyFxManager::CollectEffectFolders() {
    std::vector<std::string> list;
    std::string gameDir = GetGameDirectory();

    LogFile::WriteFormattedLine("Game Directory: \"%s\"", gameDir.c_str());

    // 1. Scan ModLoader directory if it exists
    std::string modloaderDir = gameDir + "\\modloader";
    if (Search::DirectoryExists(modloaderDir.c_str())) {
        LogFile::WriteFormattedLine("Scanning ModLoader: \"%s\"", modloaderDir.c_str());
        Search::ForAllFolders(modloaderDir.c_str(), [](const char *folderName, void *data) {
            auto pList = reinterpret_cast<std::vector<std::string>*>(data);
            if (!folderName || folderName[0] == '.')
                return;

            std::string gameDir = GetGameDirectory();
            char modRoot[MAX_PATH];
            sprintf(modRoot, "%s\\modloader\\%s", gameDir.c_str(), folderName);

            char pathModelsEffects[MAX_PATH];
            sprintf(pathModelsEffects, "%s\\models\\effects", modRoot);

            char pathEffects[MAX_PATH];
            sprintf(pathEffects, "%s\\effects", modRoot);

            bool found = false;
            // Pattern 1: modloader/<mod>/models/effects
            if (Search::DirectoryExists(pathModelsEffects) && Search::HasAnyEffectFilesRecursively(pathModelsEffects)) {
                LogFile::WriteFormattedLine("Found effects in: \"%s\"", pathModelsEffects);
                pList->push_back(pathModelsEffects);
                found = true;
            }
            // Pattern 2: modloader/<mod>/effects
            if (!found && Search::DirectoryExists(pathEffects) && Search::HasAnyEffectFilesRecursively(pathEffects)) {
                LogFile::WriteFormattedLine("Found effects in: \"%s\"", pathEffects);
                pList->push_back(pathEffects);
                found = true;
            }
            // Pattern 3: modloader/<mod>/ ONLY if it contains actual .fxs particle files
            // (Prevents scanning unrelated vehicle paintjobs or HUD sprites as effects!)
            if (!found && Search::HasFileWithExtensionRecursively(modRoot, "fxs")) {
                LogFile::WriteFormattedLine("Found .fxs effects in Mod root: \"%s\"", modRoot);
                pList->push_back(modRoot);
            }
        }, &list);
    } else {
        LogFile::WriteLine("ModLoader directory not found.");
    }

    // 2. Scan root directory fallback (models\effects)
    std::string rootEffects = gameDir + "\\models\\effects";
    if (Search::DirectoryExists(rootEffects.c_str()) && Search::HasAnyEffectFilesRecursively(rootEffects.c_str())) {
        LogFile::WriteFormattedLine("Found effects in root: \"%s\"", rootEffects.c_str());
        list.push_back(rootEffects);
    }

    return list;
}

bool MyFxManager::TextureAlreadyLoaded(const char *name) {
    for (auto &i : customTexturesNames) {
        if (!_stricmp(i.c_str(), name))
            return true;
    }
    return false;
}

void MyFxManager::LoadPNGTextureCB(const char *path, void *dictionary) {
    char texName[MAX_PATH];
    _splitpath(path, NULL, NULL, texName, NULL);
    texName[31] = '\0';
    if (TextureAlreadyLoaded(texName)) {
        LogFile::WriteFormattedLine("Loading PNG texture \"%s\" - texture was already loaded.", path);
        return;
    }
    LogFile::WriteFormattedLine("Loading PNG texture \"%s\"", path);
    int width, height, depth, flags;
    RwImage *image = RtPNGImageRead(path);
    if (!image) {
        LogFile::WriteFormattedLine("\"%s\" - FAILED to load texture", path);
        return;
    }
    customTexturesNames.push_back(texName);
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
    RwTextureSetName(tex, texName);
    if ((tex->raster->cFormat & 0x80) == 0)
        RwTextureSetFilterMode(tex, rwFILTERLINEAR);
    else
        RwTextureSetFilterMode(tex, rwFILTERLINEARMIPLINEAR);
    RwTextureSetAddressing(tex, rwTEXTUREADDRESSWRAP);
    RwTexture *oldTex = RwTexDictionaryFindNamedTexture(reinterpret_cast<TxdDef *>(dictionary)->m_pRwDictionary, texName);
    if (oldTex) {
        LogFile::WriteFormattedLine("Removing old texture \"%s\"", texName);
        RwTexDictionaryRemoveTexture(oldTex);
        RwTextureDestroy(oldTex);
    }
    RwTexDictionaryAddTexture(reinterpret_cast<TxdDef *>(dictionary)->m_pRwDictionary, tex);
}

void MyFxManager::LoadDDSTextureCB(const char *path, void *dictionary) {
    char texName[MAX_PATH];
    _splitpath(path, NULL, NULL, texName, NULL);
    texName[31] = '\0';
    if (TextureAlreadyLoaded(texName)) {
        LogFile::WriteFormattedLine("Loading DDS texture \"%s\" - texture was already loaded.", path);
        return;
    }
    LogFile::WriteFormattedLine("Loading DDS texture \"%s\"", path);
    char ddsPath[MAX_PATH];
    strncpy(ddsPath, path, sizeof(ddsPath) - 1);
    ddsPath[sizeof(ddsPath) - 1] = '\0';
    char *dot = strrchr(ddsPath, '.');
    if (dot) {
        *dot = '\0';
    }
    RwTexture *tex = RwD3D9DDSTextureRead(ddsPath, NULL);
    if (!tex) {
        LogFile::WriteFormattedLine("\"%s\" - FAILED to load texture", path);
        return;
    }
    RwTextureSetAddressing(tex, rwTEXTUREADDRESSWRAP);
    customTexturesNames.push_back(texName);
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
    sscanf(line, "%*s %s", tempSystemName);
    CFileMgr::Seek(file, posn, SEEK_SET);
    return CKeyGen::GetUppercaseKey(tempSystemName);
}

bool MyFxManager::IsThisParticleLoaded(unsigned int key) {
    for (auto i : customParticlesKeys) {
        if (i == key)
            return true;
    }
    return false;
}

void MyFxManager::LoadFxSystemFileCB(const char *path, void *data) {
    char linebuf[256];
    int file = CFileMgr::OpenFile(const_cast<char*>(path), "r");
    if (file != 0) {
        CFileMgr::ReadLine(file, linebuf, 256);
        char *pHeader = linebuf;
        // Skip UTF-8 BOM if present
        if ((unsigned char)pHeader[0] == 0xEF && (unsigned char)pHeader[1] == 0xBB && (unsigned char)pHeader[2] == 0xBF) {
            pHeader += 3;
        }
        while (*pHeader == ' ' || *pHeader == '\t') pHeader++;

        if (!strncmp(pHeader, "FX_SYSTEM_DATA:", 15)) {
            unsigned int key = GetSystemNameKey(file);
            if (!IsThisParticleLoaded(key)) {
                customParticlesKeys.push_back(key);
                LogFile::WriteFormattedLine("Loading custom system \"%s\" (\"%s\")", path, tempSystemName);
                reinterpret_cast<MyFxManager *>(data)->LoadFxSystemBP(const_cast<char*>(path), file);
            }
            else {
                LogFile::WriteFormattedLine("Loading custom system \"%s\" (\"%s\") - this system was already loaded.", path, tempSystemName);
            }
        }
        else {
            LogFile::WriteFormattedLine("Skipping \"%s\" - header is not FX_SYSTEM_DATA: (found: \"%s\")", path, pHeader);
        }
        CFileMgr::CloseFile(file);
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
        logFilePath = effectFolders[0] + "\\log.txt";
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

    LogFile::WriteFormattedLine("Discovered %zu effect source folder(s):", effectFolders.size());
    for (const auto &folder : effectFolders) {
        LogFile::WriteFormattedLine(" - %s", folder.c_str());
    }

    // 1. Load textures (DDS and PNG) from all discovered effect paths recursively
    for (const auto &folder : effectFolders) {
        Search::ForAllFilesRecursively(folder.c_str(), "dds", LoadDDSTextureCB, CTxdStore::ms_pTxdPool->GetAt(this->m_nFxTxdIndex));
        Search::ForAllFilesRecursively(folder.c_str(), "png", LoadPNGTextureCB, CTxdStore::ms_pTxdPool->GetAt(this->m_nFxTxdIndex));
    }

    CTxdStore::AddRef(this->m_nFxTxdIndex);
    CTxdStore::PushCurrentTxd();
    CTxdStore::SetCurrentTxd(this->m_nFxTxdIndex);

    // 2. Load custom fx system files (.fxs) from all discovered effect paths recursively
    for (const auto &folder : effectFolders) {
        Search::ForAllFilesRecursively(folder.c_str(), "fxs", LoadFxSystemFileCB, this);
    }

    // 3. Load default effects from effects.fxp
    int file = CFileMgr::OpenFile(fxFileName, "r");
    if (file != 0) {
        char linebuf[256];
        char header[32];
        CFileMgr::ReadLine(file, linebuf, 256); // FX_PROJECT_DATA:
        CFileMgr::ReadLine(file, linebuf, 256); // 
        CFileMgr::ReadLine(file, linebuf, 256); // FX_SYSTEM_DATA:
        sscanf(linebuf, "%s", header);
        if (!strncmp(header, "FX_SYSTEM_DATA:", 15)) {
            do {
                unsigned int key = GetSystemNameKey(file);
                if (!IsThisParticleLoaded(key)) {
                    LogFile::WriteFormattedLine("Loading default system \"%s\"", tempSystemName);
                    this->LoadFxSystemBP(fxFileName, file);
                    CFileMgr::ReadLine(file, linebuf, 256); // 
                    CFileMgr::ReadLine(file, linebuf, 256); // FX_SYSTEM_DATA:
                    sscanf(linebuf, "%s", header);
                }
                else {
                    LogFile::WriteFormattedLine("Loading default system \"%s\" - this system was already loaded.", tempSystemName);
                    while (CFileMgr::ReadLine(file, linebuf, 256)) {
                        sscanf(linebuf, "%s", header);
                        if (!strncmp(header, "FX_SYSTEM_DATA:", 15))
                            break;
                    }
                }
            } while (!strncmp(header, "FX_SYSTEM_DATA:", 15));
        }
        CFileMgr::CloseFile(file);
    }
    CTxdStore::PopCurrentTxd();
    this->m_pool.Optimise();
    customParticlesKeys.clear();
    customTexturesNames.clear();

    LogFile::WriteLine("EffectsLoader - Finished loading effects");
    LogFile::WriteLine("========================================");
    LogFile::Close();
    return true;
}