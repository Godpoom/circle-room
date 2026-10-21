// Windows helpers. All strings are UTF-8 so paths with Thai characters work.
#pragma once
#include <cstdio>
#include <string>
#include <vector>

std::wstring utf8ToWide(const std::string& s);
std::string wideToUtf8(const std::wstring& s);

FILE* ufopen(const std::string& path, const char* mode);
bool readFileBytes(const std::string& path, std::vector<unsigned char>& out);
bool writeTextFile(const std::string& path, const std::string& text);
bool readTextFile(const std::string& path, std::string& out);
bool fileExists(const std::string& path);
bool deleteFileU(const std::string& path);
bool ensureDir(const std::string& path);
std::vector<std::string> listFiles(const std::string& dir, const char* ext);  // ext like ".plook"
std::vector<std::string> listImages(const std::string& dir);  // jpg/png/tif/bmp/webp/heic, sorted
bool isDirectory(const std::string& path);
bool fileInfo(const std::string& path, unsigned long long& size, long long& mtime);
bool replaceFileU(const std::string& from, const std::string& to);
bool setHiddenU(const std::string& path);
bool copyFileU(const std::string& from, const std::string& to);  // never overwrites

std::string exeDir();
std::string dirOf(const std::string& path);
std::string fileName(const std::string& path);
std::string fileStem(const std::string& path);

// Native dialogs; return "" when cancelled.
std::string openImageDialog(void* hwnd, const std::string& initialDir);
std::string saveImageDialog(void* hwnd, const std::string& defaultPath, bool png);
std::vector<std::string> openImagesDialog(void* hwnd, const std::string& initialDir);  // multi-select
std::string pickFolderDialog(void* hwnd, const std::string& initialDir);

std::vector<std::string> commandLineArgs();
void attachParentConsole();
size_t processPrivateBytes();
size_t processWorkingSet();
size_t processPrivateWorkingSet();
void trimWorkingSet();
void* getProcAddressU(const char* dll, const char* fn);  // loads the DLL if needed
