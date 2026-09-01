#pragma once

class Search {
public:
    // Checks if a directory exists on disk
    static bool DirectoryExists(const char *path);

    // Recursively checks if a folder contains any .fxs, .png, or .dds effect files
    static bool HasAnyEffectFilesRecursively(const char *folderpath);

    // Recursively checks if a folder contains any file matching the specified extension
    static bool HasFileWithExtensionRecursively(const char *folderpath, const char *extension);

    // True when "folderpath" itself (NOT recursively) contains a file with "extension"
    static bool ContainsFileWithExtension(const char *folderpath, const char *extension);

    // Finds the first file matching the specified extension in a single directory
    static bool GetFirstFile(char *outPath, const char *folderpath, const char *extension);

    // Iterates through all top-level subdirectories in a directory
    static void ForAllFolders(const char *path, void(*callback)(const char *, void *), void *data);

    // Iterates through all files in a single directory matching the specified extension
    static void ForAllFiles(const char *folderpath, const char *extension, void(*callback)(const char *, void *), void *data);

    // Recursively iterates through all files matching the specified extension in all subdirectories
    static void ForAllFilesRecursively(const char *folderpath, const char *extension, void(*callback)(const char *, void *), void *data);

    // Like ForAllFilesRecursively, but a file is only reported when its own directory also
    // contains a file with "pairedExtension" (directory-level pairing, used for loose mod
    // layouts so unrelated images elsewhere in a mod are never pulled in). Subdirectories
    // are always visited regardless.
    static void ForAllFilesPairedRecursively(const char *folderpath, const char *extension, const char *pairedExtension, void(*callback)(const char *, void *), void *data);
};