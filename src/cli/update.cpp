#include "update.h"
#include "pingkk/language.h"
#include "pingkk/version.h"
#include "pingkk/updater_scripts.h"

#include <cstring>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <cstdlib>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#endif

namespace pingkk { namespace cli {
namespace {

int preparationError() {
    std::cerr << text("无法准备更新程序，请检查临时目录和系统权限。\n");
    return 1;
}

#ifdef _WIN32
std::wstring quoteArgument(const std::wstring& value) {
    std::wstring quoted = L"\"";
    std::size_t slashes = 0;
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] == L'\\') {
            ++slashes;
        } else {
            quoted.append(value[index] == L'"' ? slashes * 2 + 1 : slashes, L'\\');
            slashes = 0;
            quoted += value[index];
        }
    }
    quoted.append(slashes * 2, L'\\');
    return quoted + L'"';
}

bool writeScript(HANDLE file, const std::string& script) {
    std::size_t written = 0;
    while (written < script.size()) {
        DWORD count = 0;
        if (!WriteFile(file, script.data() + written,
                       static_cast<DWORD>(script.size() - written), &count, NULL) || count == 0) return false;
        written += count;
    }
    return true;
}
#else
std::string currentExecutable() {
#ifdef __APPLE__
    std::uint32_t size = 0;
    _NSGetExecutablePath(NULL, &size);
    std::vector<char> buffer(size + 1, 0);
    if (_NSGetExecutablePath(&buffer[0], &size) != 0) return std::string();
    const std::string path(&buffer[0]);
#else
    std::vector<char> buffer(32768, 0);
    const ssize_t size = readlink("/proc/self/exe", &buffer[0], buffer.size());
    if (size <= 0 || static_cast<std::size_t>(size) >= buffer.size()) return std::string();
    const std::string path(&buffer[0], static_cast<std::size_t>(size));
#endif
    char* resolved = realpath(path.c_str(), NULL);
    if (resolved == NULL) return std::string();
    const std::string result(resolved);
    std::free(resolved);
    return result;
}

bool writeScript(int file, const char* script) {
    std::size_t written = 0;
    const std::size_t length = std::strlen(script);
    while (written < length) {
        const ssize_t count = write(file, script + written, length - written);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        written += static_cast<std::size_t>(count);
    }
    return true;
}
#endif

}  // namespace

int updateCommand() {
#ifdef _WIN32
    std::vector<wchar_t> executable(32768, 0);
    const DWORD length = GetModuleFileNameW(NULL, &executable[0], static_cast<DWORD>(executable.size()));
    if (length == 0 || length >= executable.size()) return preparationError();
    const std::wstring target(&executable[0], length);
    wchar_t directory[MAX_PATH] = {0};
    wchar_t temporary[MAX_PATH] = {0};
    const DWORD directoryLength = GetTempPathW(MAX_PATH, directory);
    if (directoryLength == 0 || directoryLength >= MAX_PATH ||
        GetTempFileNameW(directory, L"pku", 0, temporary) == 0) return preparationError();
    const std::wstring scriptPath = std::wstring(temporary) + L".ps1";
    if (!MoveFileExW(temporary, scriptPath.c_str(), 0)) {
        DeleteFileW(temporary);
        return preparationError();
    }
    HANDLE file = CreateFileW(scriptPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, NULL,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        DeleteFileW(scriptPath.c_str());
        return preparationError();
    }
    // Windows PowerShell 5.1 needs a BOM to reliably read Chinese UTF-8 literals.
    const bool written = writeScript(file, std::string("\xef\xbb\xbf") + kWindowsUpdater);
    CloseHandle(file);
    if (!written) {
        DeleteFileW(scriptPath.c_str());
        return preparationError();
    }
    wchar_t systemDirectory[MAX_PATH] = {0};
    const UINT systemLength = GetSystemDirectoryW(systemDirectory, MAX_PATH);
    if (systemLength == 0 || systemLength >= MAX_PATH) {
        DeleteFileW(scriptPath.c_str());
        return preparationError();
    }
    const std::wstring powershell = std::wstring(systemDirectory) + L"\\WindowsPowerShell\\v1.0\\powershell.exe";
    std::wstring command = quoteArgument(powershell) + L" -NoProfile -ExecutionPolicy Bypass -File " +
        quoteArgument(scriptPath) + L" -Target " + quoteArgument(target) +
        L" -CurrentVersion " + std::wstring(PINGKK_VERSION, PINGKK_VERSION + std::strlen(PINGKK_VERSION)) +
        L" -ParentPid " + std::to_wstring(GetCurrentProcessId());
    if (isEnglish()) command += L" -English";
    std::vector<wchar_t> raw(command.begin(), command.end());
    raw.push_back(0);
    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = {};
    if (!CreateProcessW(powershell.c_str(), &raw[0], NULL, NULL, FALSE, 0, NULL, NULL, &startup, &process)) {
        DeleteFileW(scriptPath.c_str());
        return preparationError();
    }
    CloseHandle(process.hThread);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hProcess);
    DeleteFileW(scriptPath.c_str());
    return exitCode == 0 ? 0 : 1;
#else
    const std::string target = currentExecutable();
    if (target.empty()) return preparationError();
    const char* temporaryDirectory = std::getenv("TMPDIR");
    const std::string directory = temporaryDirectory != NULL && *temporaryDirectory != '\0'
        ? temporaryDirectory : "/tmp";
    const std::string pattern = directory + "/pingkk-updater.XXXXXX";
    std::vector<char> path(pattern.begin(), pattern.end());
    path.push_back(0);
    const int file = mkstemp(&path[0]);
    if (file < 0) return preparationError();
    const bool written = writeScript(file, kUnixUpdater);
    close(file);
    if (!written) {
        unlink(&path[0]);
        return preparationError();
    }
    const pid_t child = fork();
    if (child == 0) {
        execl("/bin/sh", "sh", &path[0], target.c_str(), PINGKK_VERSION, isEnglish() ? "en" : "zh",
              static_cast<char*>(NULL));
        _exit(127);
    }
    int status = 0;
    pid_t completed = -1;
    if (child > 0) {
        do { completed = waitpid(child, &status, 0); } while (completed < 0 && errno == EINTR);
    }
    unlink(&path[0]);
    if (completed < 0) return preparationError();
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
#endif
}

} }
