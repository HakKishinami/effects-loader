#include "plugin.h"
#include "MyFxManager.h"
#include "LogFile.h"

using namespace plugin;

class EffectsLoader {
public:
    static bool __fastcall MyLoadProject(MyFxManager *fxMan, int, char *fileName) {
        return fxMan->LoadProject(fileName);
    }

    EffectsLoader() {
        // Resolve configuration file path (next to ASI module, fallback to game root)
        std::string dir = MyFxManager::GetOwnModuleDir();
        std::string ini = dir.empty() ? "effects-loader.ini" : (dir + "\\effects-loader.ini");
        if (GetFileAttributesA(ini.c_str()) == INVALID_FILE_ATTRIBUTES) {
            std::string gameDir = MyFxManager::GetGameDirectory();
            ini = gameDir + "\\effects-loader.ini";
        }

        // Read FxMemoryPool capacity in MB (default 16MB, clamped to [1..256])
        int poolMB = GetPrivateProfileIntA("Memory", "FxPoolSizeMB", 16, ini.c_str());
        if (poolMB < 1)
            poolMB = 1;
        else if (poolMB > 256)
            poolMB = 256;

        MyFxManager::configuredPoolSizeMB = (unsigned int)poolMB;
        unsigned int poolBytes = MyFxManager::configuredPoolSizeMB * 1024 * 1024;

        // Expand FxMemoryPool_c initial buffer (stock game allocates 1MB):
        // 0x4A9C36 is opcode 0x68 (push imm32), operand is at 0x4A9C37
        // 0x4A9C3B is opcode C7 46 04 (mov [esi+4], imm32), operand is at 0x4A9C3E
        plugin::patch::SetUInt(0x4A9C37, poolBytes);
        plugin::patch::SetUInt(0x4A9C3E, poolBytes);

        // Hook FxManager_c::LoadProject (0x5C2420)
        plugin::patch::RedirectJump(0x5C2420, MyLoadProject);
    }
} plgInst;