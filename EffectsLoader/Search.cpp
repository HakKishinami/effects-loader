#include "Search.h"
#include <Windows.h>
#include <stdio.h>
#include <string.h>

bool Search::DirectoryExists(const char *path) {
    if (!path || !path[0])
        return false;
    DWORD attr = GetFileAttributesA(path);
    return (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY));
}

bool Search::HasAnyEffectFilesRecursively(const char *folderpath) {
    if (!folderpath || !folderpath[0])
        return false;
    char search_path[MAX_PATH];
    snprintf(search_path, sizeof(search_path), "%s\\*.*", folderpath);
    WIN32_FIND_DATA fd;
    HANDLE hFind = FindFirstFile(search_path, &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (fd.cFileName[0] == '.')
                continue;

            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                    char subPath[MAX_PATH];
                    snprintf(subPath, sizeof(subPath), "%s\\%s", folderpath, fd.cFileName);
                    if (HasAnyEffectFilesRecursively(subPath)) {
                        FindClose(hFind);
                        return true;
                    }
                }
            } else {
                const char *dot = strrchr(fd.cFileName, '.');
                if (dot) {
                    if (!_stricmp(dot + 1, "fxs") || !_stricmp(dot + 1, "png") || !_stricmp(dot + 1, "dds")) {
                        FindClose(hFind);
                        return true;
                    }
                }
            }
        } while (FindNextFile(hFind, &fd));
        FindClose(hFind);
    }
    return false;
}

bool Search::HasFileWithExtensionRecursively(const char *folderpath, const char *extension) {
    if (!folderpath || !folderpath[0])
        return false;
    char search_path[MAX_PATH];
    snprintf(search_path, sizeof(search_path), "%s\\*.*", folderpath);
    WIN32_FIND_DATA fd;
    HANDLE hFind = FindFirstFile(search_path, &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (fd.cFileName[0] == '.')
                continue;

            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                    char subPath[MAX_PATH];
                    snprintf(subPath, sizeof(subPath), "%s\\%s", folderpath, fd.cFileName);
                    if (HasFileWithExtensionRecursively(subPath, extension)) {
                        FindClose(hFind);
                        return true;
                    }
                }
            } else {
                const char *dot = strrchr(fd.cFileName, '.');
                if (dot && !_stricmp(dot + 1, extension)) {
                    FindClose(hFind);
                    return true;
                }
            }
        } while (FindNextFile(hFind, &fd));
        FindClose(hFind);
    }
    return false;
}

bool Search::GetFirstFile(char *outPath, const char *folderpath, const char *extension) {
    if (!folderpath || !folderpath[0])
        return false;
    WIN32_FIND_DATA fd;
    char search_path[MAX_PATH];
    snprintf(search_path, sizeof(search_path), "%s\\*.%s", folderpath, extension);
    HANDLE hFind = FindFirstFile(search_path, &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && fd.cFileName[0] != '.') {
                if (outPath) {
                    strncpy(outPath, fd.cFileName, MAX_PATH - 1);
                    outPath[MAX_PATH - 1] = '\0';
                }
                FindClose(hFind);
                return true;
            }
        } while (FindNextFile(hFind, &fd));
        FindClose(hFind);
    }
    return false;
}

void Search::ForAllFolders(const char *path, void(*callback)(const char *, void *), void *data) {
    if (!path || !path[0])
        return;
    char search_path[MAX_PATH];
    snprintf(search_path, sizeof(search_path), "%s\\*.*", path);
    WIN32_FIND_DATA fd;
    HANDLE hFind = FindFirstFile(search_path, &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && fd.cFileName[0] != '.')
                callback(fd.cFileName, data);
        } while (FindNextFile(hFind, &fd));
        FindClose(hFind);
    }
}

void Search::ForAllFiles(const char *folderpath, const char *extension, void(*callback)(const char *, void *), void *data) {
    if (!folderpath || !folderpath[0])
        return;
    char search_path[MAX_PATH];
    snprintf(search_path, sizeof(search_path), "%s\\*.%s", folderpath, extension);
    WIN32_FIND_DATA fd;
    HANDLE hFind = FindFirstFile(search_path, &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && fd.cFileName[0] != '.') {
                char path[MAX_PATH];
                snprintf(path, sizeof(path), "%s\\%s", folderpath, fd.cFileName);
                callback(path, data);
            }
        } while (FindNextFile(hFind, &fd));
        FindClose(hFind);
    }
}

void Search::ForAllFilesRecursively(const char *folderpath, const char *extension, void(*callback)(const char *, void *), void *data) {
    if (!folderpath || !folderpath[0])
        return;
    char search_path[MAX_PATH];
    snprintf(search_path, sizeof(search_path), "%s\\*.*", folderpath);
    WIN32_FIND_DATA fd;
    HANDLE hFind = FindFirstFile(search_path, &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (fd.cFileName[0] == '.')
                continue;

            char fullPath[MAX_PATH];
            snprintf(fullPath, sizeof(fullPath), "%s\\%s", folderpath, fd.cFileName);

            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                    ForAllFilesRecursively(fullPath, extension, callback, data);
            } else {
                const char *dot = strrchr(fd.cFileName, '.');
                if (dot && !_stricmp(dot + 1, extension)) {
                    callback(fullPath, data);
                }
            }
        } while (FindNextFile(hFind, &fd));
        FindClose(hFind);
    }
}