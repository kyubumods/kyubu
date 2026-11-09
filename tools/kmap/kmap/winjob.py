"""A Windows Job Object through ctypes (add-native-replay design D8).

Every host of a native replay is put in one job with JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE, so a process that a host starts
itself (game_restart) belongs to the job too, and closing the job ends them all. No dependency beyond the standard library.
"""

import ctypes
import os
import time
from ctypes import wintypes

KILL_ON_JOB_CLOSE = 0x2000
EXTENDED_LIMIT_INFORMATION = 9
BASIC_ACCOUNTING_INFORMATION = 1
PROCESS_SET_QUOTA = 0x0100
PROCESS_TERMINATE = 0x0001


class _BasicLimits(ctypes.Structure):
    _fields_ = [('PerProcessUserTimeLimit', ctypes.c_int64), ('PerJobUserTimeLimit', ctypes.c_int64),
                ('LimitFlags', wintypes.DWORD), ('MinimumWorkingSetSize', ctypes.c_size_t),
                ('MaximumWorkingSetSize', ctypes.c_size_t), ('ActiveProcessLimit', wintypes.DWORD),
                ('Affinity', ctypes.c_size_t), ('PriorityClass', wintypes.DWORD), ('SchedulingClass', wintypes.DWORD)]


class _IoCounters(ctypes.Structure):
    _fields_ = [(n, ctypes.c_uint64) for n in ('ReadOps', 'WriteOps', 'OtherOps', 'ReadBytes', 'WriteBytes', 'OtherBytes')]


class _ExtendedLimits(ctypes.Structure):
    _fields_ = [('Basic', _BasicLimits), ('Io', _IoCounters), ('ProcessMemoryLimit', ctypes.c_size_t),
                ('JobMemoryLimit', ctypes.c_size_t), ('PeakProcessMemoryUsed', ctypes.c_size_t),
                ('PeakJobMemoryUsed', ctypes.c_size_t)]


class _Accounting(ctypes.Structure):
    _fields_ = [('TotalUserTime', ctypes.c_int64), ('TotalKernelTime', ctypes.c_int64),
                ('ThisPeriodTotalUserTime', ctypes.c_int64), ('ThisPeriodTotalKernelTime', ctypes.c_int64),
                ('TotalPageFaultCount', wintypes.DWORD), ('TotalProcesses', wintypes.DWORD),
                ('ActiveProcesses', wintypes.DWORD), ('TotalTerminatedProcesses', wintypes.DWORD)]


def _api():
    k = ctypes.WinDLL('kernel32', use_last_error=True)
    k.CreateJobObjectW.restype = wintypes.HANDLE
    k.CreateJobObjectW.argtypes = [ctypes.c_void_p, wintypes.LPCWSTR]
    k.SetInformationJobObject.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD]
    k.QueryInformationJobObject.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD,
                                            ctypes.c_void_p]
    k.OpenProcess.restype = wintypes.HANDLE
    k.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    k.AssignProcessToJobObject.argtypes = [wintypes.HANDLE, wintypes.HANDLE]
    k.TerminateJobObject.argtypes = [wintypes.HANDLE, wintypes.UINT]
    k.CloseHandle.argtypes = [wintypes.HANDLE]
    return k


class Job:
    """One job: `add(proc)` for every host, `active()` for the processes still alive in it, `close()` to end them all."""

    def __init__(self):
        if os.name != 'nt':
            raise OSError('a Job Object exists on Windows only')
        self._k = _api()
        self.handle = self._k.CreateJobObjectW(None, None)
        if not self.handle:
            raise ctypes.WinError(ctypes.get_last_error())
        info = _ExtendedLimits()
        info.Basic.LimitFlags = KILL_ON_JOB_CLOSE
        if not self._k.SetInformationJobObject(self.handle, EXTENDED_LIMIT_INFORMATION, ctypes.byref(info),
                                               ctypes.sizeof(info)):
            err = ctypes.get_last_error()
            self.close()
            raise ctypes.WinError(err)

    def add(self, proc):
        """Puts a subprocess.Popen (or a pid) in the job; its children are in it from then on."""
        pid = getattr(proc, 'pid', proc)
        h = self._k.OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, False, pid)
        if not h:
            raise ctypes.WinError(ctypes.get_last_error())
        try:
            if not self._k.AssignProcessToJobObject(self.handle, h):
                raise ctypes.WinError(ctypes.get_last_error())
        finally:
            self._k.CloseHandle(h)

    def active(self):
        """The number of processes alive in the job."""
        acc = _Accounting()
        if not self._k.QueryInformationJobObject(self.handle, BASIC_ACCOUNTING_INFORMATION, ctypes.byref(acc),
                                                 ctypes.sizeof(acc), None):
            raise ctypes.WinError(ctypes.get_last_error())
        return acc.ActiveProcesses

    def wait_empty(self, timeout):
        """Waits up to `timeout` seconds until no process is left in the job; returns whether none is."""
        end = time.time() + timeout
        while self.active() and time.time() < end:
            time.sleep(0.2)
        return not self.active()

    def close(self):
        """Ends every process in the job (a second call does nothing)."""
        if self.handle:
            self._k.TerminateJobObject(self.handle, 1)
            self._k.CloseHandle(self.handle)
            self.handle = None

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
