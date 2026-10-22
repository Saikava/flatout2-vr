#include "dialog.h"

#include "util.h"

namespace {

using TaskDialogIndirectFn = HRESULT(WINAPI*)(const TASKDIALOGCONFIG*, int*, int*, BOOL*);

struct DialogCall
{
    const TASKDIALOGCONFIG* config;
    int button = 0;
    BOOL checked = FALSE;
    HRESULT hr = E_NOTIMPL;
};

DWORD WINAPI DialogThread(void* param)
{
    DialogCall& call = *(DialogCall*)param;
    // The mod's manifest (resource 2, src/fo2vr.manifest) names comctl32 6; while its activation context is active,
    // comctl32.dll loads as that version (beside the game's 5) and has TaskDialogIndirect.
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&DialogThread, &self);
    ACTCTXW ac = { sizeof(ac) };
    ac.dwFlags = ACTCTX_FLAG_HMODULE_VALID | ACTCTX_FLAG_RESOURCE_NAME_VALID;
    ac.hModule = self;
    ac.lpResourceName = MAKEINTRESOURCEW(2);
    HANDLE ctx = CreateActCtxW(&ac);
    ULONG_PTR cookie = 0;
    if (ctx == INVALID_HANDLE_VALUE || !ActivateActCtx(ctx, &cookie))
    {
        Log("dialog: no activation context for comctl32 6 (error %u)", (unsigned)GetLastError());
        if (ctx != INVALID_HANDLE_VALUE) ReleaseActCtx(ctx);
        return 0;
    }
    HMODULE comctl = LoadLibraryW(L"comctl32.dll");
    auto taskDialog = comctl ? (TaskDialogIndirectFn)GetProcAddress(comctl, "TaskDialogIndirect") : nullptr;
    if (taskDialog)
        call.hr = taskDialog(call.config, &call.button, nullptr, &call.checked);
    else
        Log("dialog: comctl32 6 has no TaskDialogIndirect");
    DeactivateActCtx(0, cookie);
    ReleaseActCtx(ctx);
    return 0;
}

}  // namespace

HRESULT ShowTaskDialog(const TASKDIALOGCONFIG& config, int* button, BOOL* checked)
{
    DialogCall call{ &config };
    HANDLE thread = CreateThread(nullptr, 0, DialogThread, &call, 0, nullptr);
    if (!thread) return HRESULT_FROM_WIN32(GetLastError());
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    if (FAILED(call.hr)) Log("dialog: not shown (%08X)", (unsigned)call.hr);
    if (button) *button = call.button;
    if (checked) *checked = call.checked;
    return call.hr;
}
