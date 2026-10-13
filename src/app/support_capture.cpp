#define _WIN32_WINNT 0x0603
#include "jfg/runtime/support_snapshot.hpp"
#include <processsnapshot.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#include <cstdlib>

// A separate, deadline-bound process owns DbgHelp. No symbol downloads or memory
// dump are requested. The PSS clone allows unwinding without leaving game threads
// suspended if the launcher has to terminate this helper.
int wmain(int argc, wchar_t **argv) {
    if (argc != 3) return 64;
    wchar_t *end = nullptr;
    const auto pid = std::wcstoul(argv[1], &end, 10);
    if (pid == 0 || end == argv[1] || *end != 0) return 64;
    const auto expected = _wcstoui64(argv[2], &end, 10);
    if (expected == 0 || end == argv[2] || *end != 0) return 64;
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    jfg::support_line(output, "snapshot=hang-v1\n");
    HANDLE parent = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (parent == nullptr) return 2;
    FILETIME created{}, exited{}, kernel{}, user{};
    wchar_t parent_path[32768]{}; DWORD length = 32768;
    if (!GetProcessTimes(parent, &created, &exited, &kernel, &user) ||
        ((static_cast<unsigned long long>(created.dwHighDateTime) << 32) | created.dwLowDateTime) != expected ||
        !QueryFullProcessImageNameW(parent, 0, parent_path, &length)) { CloseHandle(parent); return 3; }
    // The game launcher process owns a direct native child. Prefer that child,
    // checking its parent, creation time and full executable path before capture.
    DWORD target = pid; FILETIME selected_created = created;
    HANDLE processes = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (processes != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry);
        if (Process32FirstW(processes, &entry)) do {
            if (entry.th32ParentProcessID != pid) continue;
            HANDLE child = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, entry.th32ProcessID);
            if (child == nullptr) continue;
            wchar_t child_path[32768]{}; length = 32768; FILETIME born{};
            if (GetProcessTimes(child, &born, &exited, &kernel, &user) && CompareFileTime(&born, &created) >= 0 &&
                QueryFullProcessImageNameW(child, 0, child_path, &length) && _wcsicmp(parent_path, child_path) == 0) { target = entry.th32ProcessID; selected_created = born; }
            CloseHandle(child);
        } while (Process32NextW(processes, &entry));
        CloseHandle(processes);
    }
    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_CREATE_PROCESS | PROCESS_DUP_HANDLE, FALSE, target);
    CloseHandle(parent);
    if (process == nullptr) return 4;
    // Revalidate after reopening to reject exit/PID reuse races.
    wchar_t target_path[32768]{}; length = 32768; FILETIME target_created{};
    if (!GetProcessTimes(process, &target_created, &exited, &kernel, &user) || CompareFileTime(&target_created, &selected_created) != 0 ||
        !QueryFullProcessImageNameW(process, 0, target_path, &length) || _wcsicmp(parent_path, target_path) != 0) { CloseHandle(process); return 3; }
    jfg::support_line(output, target == pid ? "capture_scope=direct\n" : "capture_scope=child\n");
    HPSS snapshot = nullptr;
    DWORD error = PssCaptureSnapshot(process, static_cast<PSS_CAPTURE_FLAGS>(PSS_CAPTURE_VA_CLONE | PSS_CAPTURE_THREADS | PSS_CAPTURE_THREAD_CONTEXT), CONTEXT_FULL, &snapshot);
    if (error != ERROR_SUCCESS) { char line[64]{}; std::snprintf(line, sizeof(line), "capture_error=0x%08lx\n", static_cast<unsigned long>(error)); jfg::support_line(output, line); CloseHandle(process); return 5; }
    PSS_VA_CLONE_INFORMATION clone{};
    error = PssQuerySnapshot(snapshot, PSS_QUERY_VA_CLONE_INFORMATION, &clone, sizeof(clone));
    // RGO: 256 frames ran out before the game's own threads (they come last),
    // so freezes showed only driver and helper threads. 900 frames of at most
    // 32 per thread keep the output under the launcher's 64 KiB limit.
    constexpr unsigned kMaxFrames = 900, kMaxThreads = 128, kMaxDepth = 32;
    unsigned frames = 0, threads = 0;
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS | SYMOPT_IGNORE_NT_SYMPATH);
    if (error == ERROR_SUCCESS && SymInitialize(clone.VaCloneHandle, "", TRUE)) {
        HPSSWALK marker = nullptr;
        if (PssWalkMarkerCreate(nullptr, &marker) == ERROR_SUCCESS) {
            PSS_THREAD_ENTRY entry{};
            while (threads < kMaxThreads && frames < kMaxFrames && PssWalkSnapshot(snapshot, PSS_WALK_THREADS, marker, &entry, sizeof(entry)) == ERROR_SUCCESS) {
                ++threads; if (entry.ContextRecord == nullptr) continue;
                CONTEXT context = *entry.ContextRecord;
                STACKFRAME64 stack{};
                stack.AddrPC.Offset = context.Rip; stack.AddrPC.Mode = AddrModeFlat;
                stack.AddrStack.Offset = context.Rsp; stack.AddrStack.Mode = AddrModeFlat;
                stack.AddrFrame.Offset = context.Rbp; stack.AddrFrame.Mode = AddrModeFlat;
                for (unsigned i = 0; i < kMaxDepth && frames < kMaxFrames; ++i) {
                    if (jfg::support_frame(output, clone.VaCloneHandle, entry.ThreadId, i, stack.AddrPC.Offset)) ++frames;
                    const auto previous_pc = stack.AddrPC.Offset, previous_sp = stack.AddrStack.Offset;
                    if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, clone.VaCloneHandle, nullptr, &stack, &context, nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) break;
                    // DbgHelp's first call can return the supplied frame. Prime
                    // it once before treating repeated PC/SP as a stalled walk.
                    if (i == 0 && stack.AddrPC.Offset == previous_pc && stack.AddrStack.Offset == previous_sp &&
                        !StackWalk64(IMAGE_FILE_MACHINE_AMD64, clone.VaCloneHandle, nullptr, &stack, &context, nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) break;
                    if (stack.AddrPC.Offset == 0 || (stack.AddrPC.Offset == previous_pc && stack.AddrStack.Offset == previous_sp)) break;
                }
            }
            PssWalkMarkerFree(marker);
        }
        SymCleanup(clone.VaCloneHandle);
    }
    PssFreeSnapshot(GetCurrentProcess(), snapshot); CloseHandle(process);
    jfg::support_line(output, frames > 0 ? "capture=complete\n" : "capture=failed\n");
    return frames > 0 ? 0 : 6;
}
