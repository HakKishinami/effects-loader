#pragma once
#include <stdio.h>

const bool WRITELOG = true;

class LogFile {
    static FILE *m_pFile;
public:
    static void Open(const char *customPath = nullptr);
    static void Close();
    static void MakeNewLine();
    static void WriteLine(const char *text);
    static void WriteText(const char *text);

    template<typename... ArgTypes>
    static void WriteFormattedLine(const char *format, ArgTypes... args) {
        if (WRITELOG && m_pFile) {
            fprintf(m_pFile, format, args...);
            fputc('\n', m_pFile);
            fflush(m_pFile);
        }
    }

    template<typename... ArgTypes>
    static void WriteFormattedText(const char *format, ArgTypes... args) {
        if (WRITELOG && m_pFile) {
            fprintf(m_pFile, format, args...);
            fflush(m_pFile);
        }
    }
};