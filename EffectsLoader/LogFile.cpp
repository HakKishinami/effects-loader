#include "LogFile.h"
#include <Windows.h>

FILE *LogFile::m_pFile = nullptr;

void LogFile::Open(const char *customPath) {
    if (!WRITELOG)
        return;

    if (m_pFile) {
        fclose(m_pFile);
        m_pFile = nullptr;
    }

    if (customPath && customPath[0]) {
        m_pFile = fopen(customPath, "wt");
        if (m_pFile)
            return;
    }

    // Default fallback: game root directory effects-loader.log
    char exePath[MAX_PATH];
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    char *lastSlash = strrchr(exePath, '\\');
    if (lastSlash) *lastSlash = '\0';

    char fallbackPath[MAX_PATH];
    snprintf(fallbackPath, sizeof(fallbackPath), "%s\\effects-loader.log", exePath);
    m_pFile = fopen(fallbackPath, "wt");
}

void LogFile::Close() {
    if (WRITELOG && m_pFile) {
        fclose(m_pFile);
        m_pFile = nullptr;
    }
}

void LogFile::MakeNewLine() {
    if (WRITELOG && m_pFile) {
        fputc('\n', m_pFile);
        fflush(m_pFile);
    }
}

void LogFile::WriteLine(const char *text) {
    if (WRITELOG && m_pFile) {
        fputs(text, m_pFile);
        fputc('\n', m_pFile);
        fflush(m_pFile);
    }
}

void LogFile::WriteText(const char *text) {
    if (WRITELOG && m_pFile) {
        fputs(text, m_pFile);
        fflush(m_pFile);
    }
}