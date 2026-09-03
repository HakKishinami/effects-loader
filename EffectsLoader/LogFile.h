#pragma once
#include <stdio.h>
#include <string>
#include <vector>

const bool WRITELOG = true;

// Buffered log: lines accumulate in memory and hit the disk every
// FLUSH_EVERY_LINES lines (plus once at Close). The old code did
// fprintf+fflush per line - over a thousand disk syncs per launch.
// Worst case a crash loses the last <50 lines; content/order otherwise identical.
class LogFile {
    static FILE *m_pFile;
    static std::string m_buffer;
    static int m_pendingLines;
    static const int FLUSH_EVERY_LINES = 50;

    template<typename... ArgTypes>
    static void AppendFormatted(const char *format, ArgTypes... args) {
        char stack[1024];
        int n = snprintf(stack, sizeof(stack), format, args...);
        if (n <= 0)
            return;
        if ((size_t)n < sizeof(stack)) {
            m_buffer.append(stack, (size_t)n);
            return;
        }
        std::vector<char> heap((size_t)n + 1);
        snprintf(heap.data(), heap.size(), format, args...);
        m_buffer.append(heap.data(), (size_t)n);
    }

    static void BumpAndMaybeFlush() {
        if (++m_pendingLines >= FLUSH_EVERY_LINES)
            Flush();
    }
public:
    static void Open(const char *customPath = nullptr);
    static void Close();
    static void Flush(); // write buffer + fflush, keep file open
    static void MakeNewLine();
    static void WriteLine(const char *text);
    static void WriteText(const char *text);

    template<typename... ArgTypes>
    static void WriteFormattedLine(const char *format, ArgTypes... args) {
        if (WRITELOG && m_pFile) {
            AppendFormatted(format, args...);
            m_buffer.push_back('\n');
            BumpAndMaybeFlush();
        }
    }

    template<typename... ArgTypes>
    static void WriteFormattedText(const char *format, ArgTypes... args) {
        if (WRITELOG && m_pFile) {
            AppendFormatted(format, args...);
            BumpAndMaybeFlush();
        }
    }
};
