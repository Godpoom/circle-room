#include "platform_win.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>
#include <psapi.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <algorithm>

std::wstring utf8ToWide(const std::string& s) {
  if (s.empty()) return std::wstring();
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring w(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
  return w;
}

std::string wideToUtf8(const std::wstring& w) {
  if (w.empty()) return std::string();
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
  std::string s(n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
  return s;
}

FILE* ufopen(const std::string& path, const char* mode) {
  return _wfopen(utf8ToWide(path).c_str(), utf8ToWide(mode).c_str());
}

bool readFileBytes(const std::string& path, std::vector<unsigned char>& out) {
  FILE* f = ufopen(path, "rb");
  if (!f) return false;
  _fseeki64(f, 0, SEEK_END);
  long long size = _ftelli64(f);
  _fseeki64(f, 0, SEEK_SET);
  if (size < 0) { fclose(f); return false; }
  out.resize((size_t)size);
  size_t got = size ? fread(out.data(), 1, (size_t)size, f) : 0;
  fclose(f);
  return got == (size_t)size;
}

// Written next to the target and swapped in only once complete: a full disk or a crash
// mid-write leaves the previous file intact instead of a truncated one.
bool writeTextFile(const std::string& path, const std::string& text) {
  std::string tmp = path + ".tmp";
  FILE* f = ufopen(tmp, "wb");
  if (!f) return false;
  bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
  ok = (fclose(f) == 0) && ok;
  if (ok) ok = replaceFileU(tmp, path);
  if (!ok) deleteFileU(tmp);
  return ok;
}

bool readTextFile(const std::string& path, std::string& out) {
  std::vector<unsigned char> b;
  if (!readFileBytes(path, b)) return false;
  out.assign(b.begin(), b.end());
  return true;
}

bool fileExists(const std::string& path) {
  DWORD a = GetFileAttributesW(utf8ToWide(path).c_str());
  return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool deleteFileU(const std::string& path) { return DeleteFileW(utf8ToWide(path).c_str()) != 0; }

bool ensureDir(const std::string& path) {
  if (CreateDirectoryW(utf8ToWide(path).c_str(), nullptr)) return true;
  return GetLastError() == ERROR_ALREADY_EXISTS;
}

std::vector<std::string> listFiles(const std::string& dir, const char* ext) {
  std::vector<std::string> out;
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW(utf8ToWide(dir + "\\*" + ext).c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return out;
  do {
    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) out.push_back(dir + "\\" + wideToUtf8(fd.cFileName));
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  return out;
}

std::string exeDir() {
  wchar_t buf[MAX_PATH * 4];
  DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
  return dirOf(wideToUtf8(std::wstring(buf, n)));
}

std::string dirOf(const std::string& path) {
  size_t p = path.find_last_of("\\/");
  return p == std::string::npos ? std::string(".") : path.substr(0, p);
}

std::string fileName(const std::string& path) {
  size_t p = path.find_last_of("\\/");
  return p == std::string::npos ? path : path.substr(p + 1);
}

std::string fileStem(const std::string& path) {
  std::string n = fileName(path);
  size_t d = n.find_last_of('.');
  return d == std::string::npos ? n : n.substr(0, d);
}

std::string openImageDialog(void* hwnd, const std::string& initialDir) {
  wchar_t file[MAX_PATH * 4] = L"";
  std::wstring dir = utf8ToWide(initialDir);
  OPENFILENAMEW ofn = {};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = (HWND)hwnd;
  ofn.lpstrFilter = L"Images (*.jpg;*.jpeg;*.png;*.bmp;*.tga;*.psd;*.gif)\0*.jpg;*.jpeg;*.png;*.bmp;*.tga;*.psd;*.gif\0All files\0*.*\0";
  ofn.lpstrFile = file;
  ofn.nMaxFile = (DWORD)(sizeof(file) / sizeof(file[0]));
  ofn.lpstrInitialDir = dir.empty() ? nullptr : dir.c_str();
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
  if (!GetOpenFileNameW(&ofn)) return std::string();
  return wideToUtf8(file);
}

std::string saveImageDialog(void* hwnd, const std::string& defaultPath, bool png) {
  wchar_t file[MAX_PATH * 4] = L"";
  std::wstring def = utf8ToWide(fileName(defaultPath));
  std::wstring dir = utf8ToWide(dirOf(defaultPath));
  wcsncpy(file, def.c_str(), MAX_PATH * 4 - 1);
  OPENFILENAMEW ofn = {};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = (HWND)hwnd;
  ofn.lpstrFilter = png ? L"PNG (*.png)\0*.png\0" : L"JPEG (*.jpg)\0*.jpg;*.jpeg\0";
  ofn.lpstrDefExt = png ? L"png" : L"jpg";
  ofn.lpstrFile = file;
  ofn.nMaxFile = (DWORD)(sizeof(file) / sizeof(file[0]));
  ofn.lpstrInitialDir = dir.c_str();
  ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
  if (!GetSaveFileNameW(&ofn)) return std::string();
  return wideToUtf8(file);
}

std::vector<std::string> commandLineArgs() {
  std::vector<std::string> out;
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  for (int i = 0; i < argc; i++) out.push_back(wideToUtf8(argv[i]));
  LocalFree(argv);
  return out;
}

void attachParentConsole() {
  HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
  if (h && h != INVALID_HANDLE_VALUE && GetFileType(h) != FILE_TYPE_UNKNOWN) return;  // already redirected
  if (AttachConsole(ATTACH_PARENT_PROCESS)) {
    freopen("CONOUT$", "w", stdout);
    freopen("CONOUT$", "w", stderr);
  }
}

size_t processPrivateBytes() {
  PROCESS_MEMORY_COUNTERS_EX pmc = {};
  pmc.cb = sizeof(pmc);
  if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc))) return pmc.PrivateUsage;
  return 0;
}

size_t processWorkingSet() {
  PROCESS_MEMORY_COUNTERS pmc = {};
  pmc.cb = sizeof(pmc);
  if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) return pmc.WorkingSetSize;
  return 0;
}

size_t processPrivateWorkingSet() {
  // PROCESS_MEMORY_COUNTERS_EX2 (Windows 10 1809+): the value Task Manager shows as "Memory".
  struct Ex2 {
    DWORD cb, PageFaultCount;
    SIZE_T PeakWorkingSetSize, WorkingSetSize, QuotaPeakPagedPoolUsage, QuotaPagedPoolUsage,
        QuotaPeakNonPagedPoolUsage, QuotaNonPagedPoolUsage, PagefileUsage, PeakPagefileUsage, PrivateUsage,
        PrivateWorkingSetSize;
    ULONG64 SharedCommitUsage;
  } pmc = {};
  pmc.cb = sizeof(pmc);
  if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc)) && pmc.PrivateWorkingSetSize)
    return pmc.PrivateWorkingSetSize;
  return processWorkingSet();
}

void trimWorkingSet() { SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1); }

std::vector<std::string> listImages(const std::string& dir) {
  static const char* exts[] = {".jpg", ".jpeg", ".png", ".tif", ".tiff", ".bmp", ".webp", ".heic", ".heif", ".jxr"};
  std::vector<std::string> out;
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW(utf8ToWide(dir + "\\*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return out;
  do {
    if (fd.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_HIDDEN)) continue;
    std::string name = wideToUtf8(fd.cFileName);
    size_t dot = name.find_last_of('.');
    if (dot == std::string::npos) continue;
    std::string ext = name.substr(dot);
    for (char& c : ext) c = (char)tolower((unsigned char)c);
    for (const char* e : exts)
      if (ext == e) {
        out.push_back(dir + "\\" + name);
        break;
      }
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  std::sort(out.begin(), out.end());
  return out;
}

bool isDirectory(const std::string& path) {
  DWORD a = GetFileAttributesW(utf8ToWide(path).c_str());
  return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool fileInfo(const std::string& path, unsigned long long& size, long long& mtime) {
  WIN32_FILE_ATTRIBUTE_DATA d;
  if (!GetFileAttributesExW(utf8ToWide(path).c_str(), GetFileExInfoStandard, &d)) return false;
  size = ((unsigned long long)d.nFileSizeHigh << 32) | d.nFileSizeLow;
  mtime = (long long)(((unsigned long long)d.ftLastWriteTime.dwHighDateTime << 32) | d.ftLastWriteTime.dwLowDateTime);
  return true;
}

bool replaceFileU(const std::string& from, const std::string& to) {
  return MoveFileExW(utf8ToWide(from).c_str(), utf8ToWide(to).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

bool setHiddenU(const std::string& path) {
  std::wstring w = utf8ToWide(path);
  DWORD a = GetFileAttributesW(w.c_str());
  if (a == INVALID_FILE_ATTRIBUTES) return false;
  return SetFileAttributesW(w.c_str(), a | FILE_ATTRIBUTE_HIDDEN) != 0;
}

bool copyFileU(const std::string& from, const std::string& to) {
  return CopyFileW(utf8ToWide(from).c_str(), utf8ToWide(to).c_str(), TRUE) != 0;
}

std::vector<std::string> openImagesDialog(void* hwnd, const std::string& initialDir) {
  std::vector<std::string> out;
  std::vector<wchar_t> buf(1 << 16, 0);
  std::wstring dir = utf8ToWide(initialDir);
  OPENFILENAMEW ofn = {};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = (HWND)hwnd;
  ofn.lpstrFilter = L"Images\0*.jpg;*.jpeg;*.png;*.tif;*.tiff;*.bmp;*.webp;*.heic\0All files\0*.*\0";
  ofn.lpstrFile = buf.data();
  ofn.nMaxFile = (DWORD)buf.size();
  ofn.lpstrInitialDir = dir.empty() ? nullptr : dir.c_str();
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_ALLOWMULTISELECT | OFN_EXPLORER;
  if (!GetOpenFileNameW(&ofn)) return out;
  std::wstring first(buf.data());
  const wchar_t* p = buf.data() + first.size() + 1;
  if (!*p) {
    out.push_back(wideToUtf8(first));  // single file: full path
    return out;
  }
  for (; *p; p += wcslen(p) + 1) out.push_back(wideToUtf8(first + L"\\" + p));
  return out;
}

std::string pickFolderDialog(void* hwnd, const std::string& initialDir) {
  std::string result;
  HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  IFileOpenDialog* dlg = nullptr;
  if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) {
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    IShellItem* start = nullptr;
    if (!initialDir.empty() &&
        SUCCEEDED(SHCreateItemFromParsingName(utf8ToWide(initialDir).c_str(), nullptr, IID_PPV_ARGS(&start)))) {
      dlg->SetFolder(start);
      start->Release();
    }
    if (SUCCEEDED(dlg->Show((HWND)hwnd))) {
      IShellItem* item = nullptr;
      if (SUCCEEDED(dlg->GetResult(&item))) {
        PWSTR p = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
          result = wideToUtf8(p);
          CoTaskMemFree(p);
        }
        item->Release();
      }
    }
    dlg->Release();
  }
  if (SUCCEEDED(init)) CoUninitialize();
  return result;
}

void hardenDllSearch() { SetDllDirectoryW(L""); }

void* getProcAddressU(const char* dll, const char* fn) {
  HMODULE h = GetModuleHandleA(dll);
  if (!h) h = LoadLibraryA(dll);
  return h ? (void*)GetProcAddress(h, fn) : nullptr;
}
