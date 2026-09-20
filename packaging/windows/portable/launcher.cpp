#include <windows.h>
#include <setupapi.h>
#include <shellapi.h>
#include <objbase.h>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <cwchar>
#include "payload.h"

namespace fs = std::filesystem;

struct Extraction {
    fs::path root;
    size_t count = 0;
};

UINT CALLBACK extractFile(PVOID context, UINT notification, UINT_PTR param1, UINT_PTR) {
    auto& extraction = *static_cast<Extraction*>(context);
    if (notification == SPFILENOTIFY_FILEINCABINET) {
        auto* info = reinterpret_cast<FILE_IN_CABINET_INFO_W*>(param1);
        for (const auto& file : payloadFiles) {
            if (std::wcscmp(info->NameInCabinet, file.name) != 0) continue;
            const fs::path target = extraction.root / file.path;
            std::error_code error;
            fs::create_directories(target.parent_path(), error);
            if (error || target.native().size() >= MAX_PATH) return FILEOP_ABORT;
            wcscpy_s(info->FullTargetName, target.c_str());
            return FILEOP_DOIT;
        }
        return FILEOP_ABORT;
    }
    if (notification == SPFILENOTIFY_FILEEXTRACTED) {
        const auto* info = reinterpret_cast<FILEPATHS_W*>(param1);
        if (info->Win32Error) return info->Win32Error;
        ++extraction.count;
    }
    if (notification == SPFILENOTIFY_NEEDNEWCABINET) return ERROR_INVALID_DATA;
    return NO_ERROR;
}

// Windows command-line quoting, including trailing backslashes.
std::wstring quote(const std::wstring& value) {
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (wchar_t character : value) {
        if (character == L'\\') { ++slashes; continue; }
        result.append(slashes * (character == L'"' ? 2 : 1), L'\\');
        slashes = 0;
        if (character == L'"') result += L'\\';
        result += character;
    }
    result.append(slashes * 2, L'\\');
    return result + L'"';
}

struct TemporaryDirectory {
    fs::path path;
    ~TemporaryDirectory() {
        // Only remove the unique directory that this invocation created.
        std::error_code error;
        if (!path.empty()) fs::remove_all(path, error);
    }
};

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    bool smokeTest = false;
    try {
        SetDllDirectoryW(L"");
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (!argv) throw std::runtime_error("arguments");
        std::wstring arguments;
        fs::path destination;
        bool extractOnly = argc == 3 && std::wcscmp(argv[1], L"--extract") == 0;
        if (extractOnly) destination = fs::absolute(argv[2]);
        for (int i = 1; !extractOnly && i < argc; ++i) {
            smokeTest |= std::wcscmp(argv[i], L"--smoke-test") == 0;
            arguments += L" " + quote(argv[i]);
        }
        LocalFree(argv);

        GUID guid;
        if (FAILED(CoCreateGuid(&guid))) throw std::runtime_error("temporary directory");
        wchar_t guidText[40];
        StringFromGUID2(guid, guidText, 40);
        TemporaryDirectory temporary;
        const fs::path candidate = fs::temp_directory_path() / (std::wstring(L"pingkk-") + guidText);
        if (!fs::create_directory(candidate)) throw std::runtime_error("temporary directory");
        temporary.path = candidate;
        const fs::path cabinet = temporary.path / L"payload.cab";
        HRSRC resource = FindResourceW(instance, MAKEINTRESOURCEW(101), RT_RCDATA);
        if (!resource) throw std::runtime_error("payload");
        const DWORD size = SizeofResource(instance, resource);
        const void* data = LockResource(LoadResource(instance, resource));
        if (!data || !size) throw std::runtime_error("payload");
        std::ofstream output(cabinet, std::ios::binary);
        output.write(static_cast<const char*>(data), size);
        output.close();
        if (!output) throw std::runtime_error("write payload");

        if (extractOnly) {
            // Never overwrite an existing installation or user files.
            if (!fs::create_directory(destination)) throw std::runtime_error("destination exists");
        } else {
            destination = temporary.path / L"app";
            fs::create_directory(destination);
        }
        Extraction extraction{destination};
        if (!SetupIterateCabinetW(cabinet.c_str(), 0, extractFile, &extraction) ||
            extraction.count != sizeof(payloadFiles) / sizeof(payloadFiles[0]))
            throw std::runtime_error("extract payload");
        if (extractOnly) return 0;

        const fs::path executable = destination / L"pingkk-gui.exe";
        std::wstring command = quote(executable.native()) + arguments;
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(executable.c_str(), &command[0], nullptr, nullptr, FALSE,
                            0, nullptr, nullptr, &startup, &process))
            throw std::runtime_error("start GUI");
        CloseHandle(process.hThread);
        WaitForSingleObject(process.hProcess, INFINITE);
        DWORD result = 1;
        GetExitCodeProcess(process.hProcess, &result);
        CloseHandle(process.hProcess);
        return static_cast<int>(result);
    } catch (...) {
        if (!smokeTest)
            MessageBoxW(nullptr, L"无法启动 ping看看。请检查临时目录的可用空间，或重新下载程序。\n"
                                L"如需手动释放文件，可运行：程序.exe --extract 新目录",
                        L"ping看看", MB_OK | MB_ICONERROR);
        return 1;
    }
}
