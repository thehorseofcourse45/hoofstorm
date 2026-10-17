# Sampling profiler for the viewer's main thread: suspend, walk the stack with dbghelp, resume, repeat.
# usage: python mainprof.py <pid> <samples> <interval_ms> [thread_id]
# MAINPROF_ANY=1 samples even while the viewer is not the foreground window (shutdown hangs, minimized).
import ctypes, ctypes.wintypes as wt, os, sys, time, collections

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
dbg = ctypes.WinDLL("dbghelp", use_last_error=True)

class ADDRESS64(ctypes.Structure):
    _fields_ = [("Offset", ctypes.c_uint64), ("Segment", ctypes.c_uint16), ("Mode", ctypes.c_uint32)]
class KDHELP64(ctypes.Structure):
    _fields_ = [("Thread", ctypes.c_uint64), ("ThCallbackStack", ctypes.c_uint32), ("ThCallbackBStore", ctypes.c_uint32),
                ("NextCallback", ctypes.c_uint32), ("FramePointer", ctypes.c_uint32), ("KiCallUserMode", ctypes.c_uint64),
                ("KeUserCallbackDispatcher", ctypes.c_uint64), ("SystemRangeStart", ctypes.c_uint64),
                ("KiUserExceptionDispatcher", ctypes.c_uint64), ("StackBase", ctypes.c_uint64), ("StackLimit", ctypes.c_uint64),
                ("BuildVersion", ctypes.c_uint32), ("RetpolineStubFunctionTableSize", ctypes.c_uint32),
                ("RetpolineStubFunctionTable", ctypes.c_uint64), ("RetpolineStubOffset", ctypes.c_uint32),
                ("RetpolineStubSize", ctypes.c_uint32), ("Reserved0", ctypes.c_uint64 * 2)]
class STACKFRAME64(ctypes.Structure):
    _fields_ = [("AddrPC", ADDRESS64), ("AddrReturn", ADDRESS64), ("AddrFrame", ADDRESS64), ("AddrStack", ADDRESS64),
                ("AddrBStore", ADDRESS64), ("FuncTableEntry", ctypes.c_void_p), ("Params", ctypes.c_uint64 * 4),
                ("Far", wt.BOOL), ("Virtual", wt.BOOL), ("Reserved", ctypes.c_uint64 * 3), ("KdHelp", KDHELP64)]
class SYMBOL_INFO(ctypes.Structure):
    _fields_ = [("SizeOfStruct", ctypes.c_uint32), ("TypeIndex", ctypes.c_uint32), ("Reserved", ctypes.c_uint64 * 2),
                ("Index", ctypes.c_uint32), ("Size", ctypes.c_uint32), ("ModBase", ctypes.c_uint64), ("Flags", ctypes.c_uint32),
                ("Value", ctypes.c_uint64), ("Address", ctypes.c_uint64), ("Register", ctypes.c_uint32), ("Scope", ctypes.c_uint32),
                ("Tag", ctypes.c_uint32), ("NameLen", ctypes.c_uint32), ("MaxNameLen", ctypes.c_uint32), ("Name", ctypes.c_char * 1024)]
class THREADENTRY32(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ThreadID", wt.DWORD), ("th32OwnerProcessID", wt.DWORD),
                ("tpBasePri", wt.LONG), ("tpDeltaPri", wt.LONG), ("dwFlags", wt.DWORD)]

dbg.StackWalk64.argtypes = [wt.DWORD, wt.HANDLE, wt.HANDLE, ctypes.POINTER(STACKFRAME64), ctypes.c_void_p,
                            ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]
dbg.SymFromAddr.argtypes = [wt.HANDLE, ctypes.c_uint64, ctypes.POINTER(ctypes.c_uint64), ctypes.POINTER(SYMBOL_INFO)]
dbg.SymInitialize.argtypes = [wt.HANDLE, ctypes.c_char_p, wt.BOOL]
k32.OpenProcess.restype = wt.HANDLE
k32.OpenThread.restype = wt.HANDLE
k32.CreateToolhelp32Snapshot.restype = wt.HANDLE
k32.GetThreadContext.argtypes = [wt.HANDLE, ctypes.c_void_p]

def main_thread(pid):
    # the earliest-created thread of the process is the main thread
    snap = k32.CreateToolhelp32Snapshot(4, 0)
    te = THREADENTRY32(); te.dwSize = ctypes.sizeof(te)
    best = None
    ok = k32.Thread32First(snap, ctypes.byref(te))
    while ok:
        if te.th32OwnerProcessID == pid:
            h = k32.OpenThread(0x0040, False, te.th32ThreadID)
            c, e, kt, ut = (wt.FILETIME() for _ in range(4))
            if h and k32.GetThreadTimes(h, ctypes.byref(c), ctypes.byref(e), ctypes.byref(kt), ctypes.byref(ut)):
                created = (c.dwHighDateTime << 32) | c.dwLowDateTime
                if best is None or created < best[0]: best = (created, te.th32ThreadID)
            if h: k32.CloseHandle(h)
        ok = k32.Thread32Next(snap, ctypes.byref(te))
    k32.CloseHandle(snap)
    return best[1]

def main():
    pid, count, interval = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3]) / 1000.0
    tid = int(sys.argv[4]) if len(sys.argv) > 4 else main_thread(pid)
    proc = k32.OpenProcess(0x0410, False, pid)              # QUERY_INFORMATION | VM_READ
    thread = k32.OpenThread(0x0002 | 0x0008 | 0x0040, False, tid)   # SUSPEND_RESUME | GET_CONTEXT | QUERY_INFORMATION
    if not proc or not thread: sys.exit("cannot open process/thread (error %d)" % ctypes.get_last_error())
    dbg.SymSetOptions(0x2 | 0x4 | 0x10)                      # UNDNAME | DEFERRED_LOADS | LOAD_LINES
    if not dbg.SymInitialize(proc, None, True): sys.exit("SymInitialize failed")
    table = ctypes.cast(dbg.SymFunctionTableAccess64, ctypes.c_void_p)
    modbase = ctypes.cast(dbg.SymGetModuleBase64, ctypes.c_void_p)
    raw = (ctypes.c_byte * (1232 + 16))()
    ctx = (ctypes.addressof(raw) + 15) & ~15
    stacks = []
    user32 = ctypes.WinDLL("user32")
    user32.GetForegroundWindow.restype = wt.HWND
    deadline = time.time() + 600
    while len(stacks) < count and time.time() < deadline:
        # only sample while the viewer is the foreground window; in the background it sleeps 40 ms a frame
        fg = wt.DWORD()
        user32.GetWindowThreadProcessId(user32.GetForegroundWindow(), ctypes.byref(fg))
        if fg.value != pid and not os.environ.get("MAINPROF_ANY"):
            time.sleep(0.1)
            continue
        if k32.SuspendThread(thread) == 0xFFFFFFFF: break
        try:
            ctypes.c_uint32.from_address(ctx + 0x30).value = 0x10000B   # CONTEXT_FULL (AMD64)
            if not k32.GetThreadContext(thread, ctx): continue
            frame = STACKFRAME64()
            frame.AddrPC.Offset = ctypes.c_uint64.from_address(ctx + 0xF8).value; frame.AddrPC.Mode = 3
            frame.AddrFrame.Offset = ctypes.c_uint64.from_address(ctx + 0xA0).value; frame.AddrFrame.Mode = 3
            frame.AddrStack.Offset = ctypes.c_uint64.from_address(ctx + 0x98).value; frame.AddrStack.Mode = 3
            pcs = []
            while len(pcs) < 96 and dbg.StackWalk64(0x8664, proc, thread, ctypes.byref(frame), ctx, None, table, modbase, None):
                if not frame.AddrPC.Offset: break
                pcs.append(frame.AddrPC.Offset)
            stacks.append(pcs)
        finally:
            k32.ResumeThread(thread)
        time.sleep(interval)

    names = {}
    sym = SYMBOL_INFO(); sym.SizeOfStruct = 88; sym.MaxNameLen = 1000
    disp = ctypes.c_uint64()
    def name(pc):
        if pc not in names:
            names[pc] = sym.Name.decode(errors="replace") if dbg.SymFromAddr(proc, pc, ctypes.byref(disp), ctypes.byref(sym)) else "0x%x" % pc
        return names[pc]
    waits = ("NtWaitFor", "WaitForSingleObject", "WaitForMultipleObjects", "SleepEx", "NtDelayExecution", "Sleep", "SleepConditionVariable",
             "RtlSleepCondition", "NtYieldExecution", "SwitchToThread")
    inclusive, leaf = collections.Counter(), collections.Counter()
    waiting = 0
    for pcs in stacks:
        fs = [name(pc) for pc in pcs]
        if fs and any(fs[0].startswith(w) for w in waits) or (len(fs) > 1 and any(fs[1].startswith(w) for w in waits)):
            waiting += 1
        for f in set(fs): inclusive[f] += 1
        own = next((f for f in fs if "::" in f), fs[0] if fs else "?")   # first named viewer frame
        leaf[own] += 1
    n = len(stacks)
    print("main thread %d: %d samples, %d in a wait/sleep (%.0f%%)" % (tid, n, waiting, 100.0 * waiting / max(n, 1)))
    print("\n== inclusive (share of samples containing the function)")
    for f, c in inclusive.most_common(70): print("%5.1f%%  %s" % (100.0 * c / n, f[:150]))
    print("\n== self (first C++ frame of each sample)")
    for f, c in leaf.most_common(40): print("%5.1f%%  %s" % (100.0 * c / n, f[:150]))

main()
