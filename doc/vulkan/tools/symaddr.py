# Resolve module offsets (rva) from VulkanSlowRecord log lines to symbol names using the exe's PDB.
# Usage: python -I symaddr.py <exe> <rva> [<rva> ...]   (rva as 0x... hex, as logged)
import ctypes, ctypes.wintypes as wt, os, sys

dbg = ctypes.WinDLL("dbghelp", use_last_error=True)

class SYMBOL_INFO(ctypes.Structure):
    _fields_ = [("SizeOfStruct", wt.ULONG), ("TypeIndex", wt.ULONG), ("Reserved", ctypes.c_uint64 * 2),
                ("Index", wt.ULONG), ("Size", wt.ULONG), ("ModBase", ctypes.c_uint64), ("Flags", wt.ULONG),
                ("Value", ctypes.c_uint64), ("Address", ctypes.c_uint64), ("Register", wt.ULONG), ("Scope", wt.ULONG),
                ("Tag", wt.ULONG), ("NameLen", wt.ULONG), ("MaxNameLen", wt.ULONG), ("Name", ctypes.c_char * 1024)]

dbg.SymInitialize.argtypes = [wt.HANDLE, ctypes.c_char_p, wt.BOOL]
dbg.SymLoadModuleEx.restype = ctypes.c_uint64
dbg.SymLoadModuleEx.argtypes = [wt.HANDLE, wt.HANDLE, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint64, wt.DWORD,
                                ctypes.c_void_p, wt.DWORD]
dbg.SymFromAddr.argtypes = [wt.HANDLE, ctypes.c_uint64, ctypes.POINTER(ctypes.c_uint64), ctypes.POINTER(SYMBOL_INFO)]

exe = os.path.abspath(sys.argv[1])
proc = wt.HANDLE(0x1234)  # any unique value works for offline symbol loading
BASE = 0x140000000
dbg.SymSetOptions(0x2)  # SYMOPT_UNDNAME; deferred loads made offline SymFromAddr fail
assert dbg.SymInitialize(proc, os.path.dirname(exe).encode(), False), ctypes.get_last_error()
assert dbg.SymLoadModuleEx(proc, None, exe.encode(), None, BASE, 0, None, 0), ctypes.get_last_error()
for arg in sys.argv[2:]:
    sym = SYMBOL_INFO(SizeOfStruct=ctypes.sizeof(SYMBOL_INFO) - 1024, MaxNameLen=1024)
    disp = ctypes.c_uint64()
    ok = dbg.SymFromAddr(proc, BASE + int(arg, 16), ctypes.byref(disp), ctypes.byref(sym))
    print(arg, sym.Name.decode(errors="replace") if ok else "?")
