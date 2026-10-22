#include "largeaddress.h"

#include "background.h"
#include "dialog.h"
#include "util.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

// FlatOut2.exe is not marked large address aware, so Windows gives the game 2 GB of addresses, which it shares with
// DXVK, the graphics driver and the VR runtime (SteamVR's client takes a good part). With the eyes' targets a large
// track could use them up while it loaded: DXVK failed to allocate ("Memory allocation failed" in
// FlatOut2_d3d9.log, with gigabytes of video memory free) and the game stood still at the loading screen. Marked, the
// game has 4 GB on 64-bit Windows. The flag is one bit in the file's header and changes nothing else; the game's code
// and the mod's checks of it are the same.

namespace {

const wchar_t* kRelaunched = L"FO2VR_RELAUNCHED";  // set for the game started anew: the old one's process id

bool ImageLargeAddressAware()
{
    const BYTE* img = (const BYTE*)GetModuleHandleW(nullptr);
    const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(img + ((const IMAGE_DOS_HEADER*)img)->e_lfanew);
    return (nt->FileHeader.Characteristics & IMAGE_FILE_LARGE_ADDRESS_AWARE) != 0;
}

std::wstring ErrorText(DWORD error)
{
    wchar_t* text = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error, 0,
                   (wchar_t*)&text, 0, nullptr);
    std::wstring s = text ? text : L"";
    if (text) LocalFree(text);
    while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r' || s.back() == L' ' || s.back() == L'.')) s.pop_back();
    return s.empty() ? L"error " + std::to_wstring(error) : s;
}

// Windows lets a running program's file be renamed, not written: the running FlatOut2.exe becomes the backup and the
// marked copy is written in its place. Returns what went wrong (empty: marked).
std::wstring MarkFile(const std::wstring& exe, const std::wstring& backup)
{
    HANDLE f = CreateFileW(exe.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return L"it cannot be read (" + ErrorText(GetLastError()) + L")";
    LARGE_INTEGER size = {};
    GetFileSizeEx(f, &size);
    std::vector<BYTE> data((size_t)size.QuadPart);
    DWORD got = 0;
    BOOL read = ReadFile(f, data.data(), (DWORD)data.size(), &got, nullptr);
    CloseHandle(f);
    if (!read || got != data.size()) return L"it cannot be read";
    DWORD pe = data.size() >= 0x40 && data[0] == 'M' && data[1] == 'Z' ? *(const DWORD*)&data[0x3C] : 0;
    if (!pe || pe + 24 > data.size() || memcmp(&data[pe], "PE\0\0", 4) != 0) return L"it is not a program's file";
    *(WORD*)&data[pe + 4 + 18] |= IMAGE_FILE_LARGE_ADDRESS_AWARE;  // IMAGE_FILE_HEADER::Characteristics

    if (!MoveFileExW(exe.c_str(), backup.c_str(), MOVEFILE_REPLACE_EXISTING))
        return L"it cannot be renamed (" + ErrorText(GetLastError()) + L")";
    HANDLE w = CreateFileW(exe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD put = 0;
    bool written = w != INVALID_HANDLE_VALUE && WriteFile(w, data.data(), (DWORD)data.size(), &put, nullptr) && put == data.size() &&
                   FlushFileBuffers(w);
    DWORD error = GetLastError();
    if (w != INVALID_HANDLE_VALUE) CloseHandle(w);
    if (!written)
    {
        DeleteFileW(exe.c_str());
        MoveFileExW(backup.c_str(), exe.c_str(), 0);  // the original back
        return L"the game's folder cannot be written (" + ErrorText(error) + L")";
    }
    return L"";
}

// The game anew, with the same command line and folder: it waits for this process to end (WaitForOldGame).
bool StartAnew(const std::wstring& exe)
{
    SetEnvironmentVariableW(kRelaunched, std::to_wstring(GetCurrentProcessId()).c_str());
    std::wstring cmd = GetCommandLineW();
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
    {
        Log("memory: the game could not be started anew (%ls) - 4 GB from the next start", ErrorText(GetLastError()).c_str());
        SetEnvironmentVariableW(kRelaunched, nullptr);
        return false;
    }
    Log("memory: the game started anew with 4 GB (process %u); this one ends", (unsigned)pi.dwProcessId);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

}  // namespace

void WaitForOldGame()
{
    wchar_t id[16] = L"";
    if (!GetEnvironmentVariableW(kRelaunched, id, 16)) return;
    if (HANDLE old = OpenProcess(SYNCHRONIZE, FALSE, wcstoul(id, nullptr, 10)))
    {
        WaitForSingleObject(old, 10000);
        CloseHandle(old);
    }
}

void EnsureLargeAddressAware()
{
    if (ImageLargeAddressAware())
    {
        Log("memory: FlatOut2.exe is large address aware (4 GB)");
        return;
    }
    if (!g_cfg.vrLargeAddressAware)
    {
        Log("memory: FlatOut2.exe has 2 GB (LargeAddressAware=0 leaves it so)");
        return;
    }
    wchar_t path[MAX_PATH] = L"";
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring exe = path, backup = exe + L".2GB.bak";
    // A game started anew that still has 2 GB: the marked file did not take; it is not tried again (no loop).
    wchar_t id[16] = L"";
    std::wstring problem = GetEnvironmentVariableW(kRelaunched, id, 16) ? L"the marked file did not take effect" : MarkFile(exe, backup);
    if (problem.empty())
    {
        Log("memory: FlatOut2.exe marked large address aware (4 GB); the original kept as %ls", backup.c_str());
        if (StartAnew(exe)) TerminateProcess(GetCurrentProcess(), 0);
        return;
    }
    Log("memory: FlatOut2.exe has 2 GB and could not be marked large address aware: %ls", problem.c_str());
    if (BackgroundMode()) return;
    std::wstring content =
        L"FlatOut2.exe can use only 2 GB of memory, which the game shares with the VR runtime and the graphics driver. Larger "
        L"tracks may run out of it while they load: the game then stands still at the loading screen.\n\n"
        L"FO2VR tries to let it use 4 GB (one flag in FlatOut2.exe, the original kept as FlatOut2.exe.2GB.bak), but " + problem +
        L".\n\nAllow writing to the game's folder (or start the game once as administrator), or set the flag yourself: see "
        L"\"Memory\" in scripts\\FO2VR-README.md.";
    TASKDIALOGCONFIG c = { sizeof(c) };
    c.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
    c.pszWindowTitle = L"FlatOut 2 VR";
    c.pszMainIcon = TD_WARNING_ICON;
    c.pszMainInstruction = L"FlatOut 2 has only 2 GB of memory";
    c.pszContent = content.c_str();
    c.dwCommonButtons = TDCBF_OK_BUTTON;
    c.pszVerificationText = L"Do not try again (LargeAddressAware=0)";
    BOOL never = FALSE;
    if (SUCCEEDED(ShowTaskDialog(c, nullptr, &never)) && never)
    {
        SaveIniValue("VR", "LargeAddressAware", "0");
        Log("memory: LargeAddressAware=0 from now on, by the player's choice");
    }
}

void NoteAddressSpace()
{
    static double lastCheck;
    static SIZE_T logged;
    double now = NowSeconds();
    if (now - lastCheck < g_cfg.logIntervalSeconds) return;
    lastCheck = now;
    MEMORYSTATUSEX ms = { sizeof(ms) };
    if (!GlobalMemoryStatusEx(&ms)) return;
    SIZE_T used = (SIZE_T)(ms.ullTotalVirtual - ms.ullAvailVirtual);
    if (used < logged + (128u << 20)) return;
    logged = used;
    // The largest free block only when it is logged: the walk through the address space's thousands of regions takes
    // milliseconds (done every log interval, it cost a headset frame every time).
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    SIZE_T largest = 0;
    MEMORY_BASIC_INFORMATION m;
    for (const BYTE* p = (const BYTE*)si.lpMinimumApplicationAddress; p < si.lpMaximumApplicationAddress && VirtualQuery(p, &m, sizeof(m));
         p = (const BYTE*)m.BaseAddress + m.RegionSize)
        if (m.State == MEM_FREE) largest = std::max(largest, m.RegionSize);
    Log("memory: the game's address space %u MB used of %u, the largest free block %u MB", (unsigned)(used >> 20),
        (unsigned)(ms.ullTotalVirtual >> 20), (unsigned)(largest >> 20));
}
