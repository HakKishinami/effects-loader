#include "LogFile.h"
#include <Windows.h>

FILE *LogFile::m_pFile = nullptr;
std::string LogFile::m_buffer;
int LogFile::m_pendingLines = 0;

void LogFile::Open(const char *customPath) {
    if (!WRITELOG)
        return;

    if (m_pFile) {
        fclose(m_pFile);
        m_pFile = nullptr;
    }
    m_buffer.clear();
    m_pendingLines = 0;

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
        Flush();
        fclose(m_pFile);
        m_pFile = nullptr;
    }
}

void LogFile::Flush() {
    if (WRITELOG && m_pFile) {
        if (!m_buffer.empty()) {
            fwrite(m_buffer.data(), 1, m_buffer.size(), m_pFile);
            m_buffer.clear(); // keep capacity for the next batch
        }
        fflush(m_pFile);
    }
    m_pendingLines = 0;
}

void LogFile::MakeNewLine() {
    if (WRITELOG && m_pFile) {
        m_buffer.push_back('\n');
        BumpAndMaybeFlush();
    }
}

void LogFile::WriteLine(const char *text) {
    if (WRITELOG && m_pFile) {
        if (text)
            m_buffer.append(text);
        m_buffer.push_back('\n');
        BumpAndMaybeFlush();
    }
}

void LogFile::WriteText(const char *text) {
    if (WRITELOG && m_pFile) {
        if (text)
            m_buffer.append(text);
        BumpAndMaybeFlush();
    }
}