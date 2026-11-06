<#
.SYNOPSIS
  Run a program on an invisible Windows desktop (nothing appears on the user's screen).

.DESCRIPTION
  Creates (or opens) a desktop object "<Desktop>" in the current window station, starts the
  program there with CREATE_NO_WINDOW and stdout/stderr redirected to files, waits for it to
  exit (or kills it - and all its children, via a job object - after -TimeoutSec) and closes
  the desktop again. Error mode is set so that loader / crash dialogs are suppressed; any
  window the program opens anyway (including Qt message boxes) lives on the hidden desktop.

  Exit code of the script = exit code of the program, 124 on timeout.

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\hidden_desktop.ps1 `
     -Exe build\Release\nextpnr-himbaechel.exe -Arguments '--device CCGM1A1 --gui' -Stdout out.txt -Stderr err.txt
#>
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [string]$Arguments = "",
    [string]$Stdout = "",
    [string]$Stderr = "",
    [string]$WorkDir = "",
    [int]$TimeoutSec = 120,
    [string]$Desktop = "nextpnr-test",
    # if > 0: after this many ms list the top-level windows on the hidden desktop and on the
    # user's default desktop that belong to the started process (used to prove invisibility)
    [int]$ListWindowsAfterMs = 0
)

$ErrorActionPreference = "Stop"

$src = @"
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

public static class HiddenDesktop {
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct STARTUPINFO {
        public int cb; public string lpReserved; public string lpDesktop; public string lpTitle;
        public int dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
        public short wShowWindow, cbReserved2; public IntPtr lpReserved2, hStdInput, hStdOutput, hStdError;
    }
    [StructLayout(LayoutKind.Sequential)]
    struct PROCESS_INFORMATION { public IntPtr hProcess, hThread; public int dwProcessId, dwThreadId; }
    [StructLayout(LayoutKind.Sequential)]
    struct SECURITY_ATTRIBUTES { public int nLength; public IntPtr lpSecurityDescriptor; public bool bInheritHandle; }
    [StructLayout(LayoutKind.Sequential)]
    struct JOBOBJECT_BASIC_LIMIT_INFORMATION {
        public long PerProcessUserTimeLimit, PerJobUserTimeLimit; public uint LimitFlags;
        public UIntPtr MinimumWorkingSetSize, MaximumWorkingSetSize; public uint ActiveProcessLimit;
        public UIntPtr Affinity; public uint PriorityClass, SchedulingClass;
    }
    [StructLayout(LayoutKind.Sequential)]
    struct IO_COUNTERS { public ulong a, b, c, d, e, f; }
    [StructLayout(LayoutKind.Sequential)]
    struct JOBOBJECT_EXTENDED_LIMIT_INFORMATION {
        public JOBOBJECT_BASIC_LIMIT_INFORMATION BasicLimitInformation; public IO_COUNTERS IoInfo;
        public UIntPtr ProcessMemoryLimit, JobMemoryLimit, PeakProcessMemoryUsed, PeakJobMemoryUsed;
    }
    delegate bool EnumWindowsProc(IntPtr hwnd, IntPtr lParam);

    [DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern IntPtr CreateDesktop(string name, IntPtr dev, IntPtr devmode, int flags, uint access, IntPtr sa);
    [DllImport("user32.dll", SetLastError = true)] static extern bool CloseDesktop(IntPtr h);
    [DllImport("user32.dll", SetLastError = true)] static extern IntPtr GetProcessWindowStation();
    [DllImport("user32.dll", SetLastError = true)] static extern IntPtr GetThreadDesktop(int tid);
    [DllImport("kernel32.dll")] static extern int GetCurrentThreadId();
    [DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool GetUserObjectInformation(IntPtr h, int index, StringBuilder buf, int len, out int needed);
    [DllImport("user32.dll")] static extern bool EnumDesktopWindows(IntPtr desk, EnumWindowsProc cb, IntPtr l);
    [DllImport("user32.dll")] static extern int GetWindowThreadProcessId(IntPtr hwnd, out int pid);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetWindowText(IntPtr hwnd, StringBuilder s, int n);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassName(IntPtr hwnd, StringBuilder s, int n);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr hwnd);
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool CreateProcess(string app, StringBuilder cmd, IntPtr pa, IntPtr ta, bool inherit, uint flags,
                                     IntPtr env, string cwd, ref STARTUPINFO si, out PROCESS_INFORMATION pi);
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern IntPtr CreateFile(string name, uint access, uint share, ref SECURITY_ATTRIBUTES sa, uint disp, uint flags, IntPtr tmpl);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
    [DllImport("kernel32.dll")] static extern uint WaitForSingleObject(IntPtr h, uint ms);
    [DllImport("kernel32.dll")] static extern bool GetExitCodeProcess(IntPtr h, out uint code);
    [DllImport("kernel32.dll")] static extern uint SetErrorMode(uint mode);
    [DllImport("kernel32.dll")] static extern uint ResumeThread(IntPtr h);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] static extern IntPtr CreateJobObject(IntPtr sa, string name);
    [DllImport("kernel32.dll")] static extern bool SetInformationJobObject(IntPtr job, int cls, ref JOBOBJECT_EXTENDED_LIMIT_INFORMATION info, int len);
    [DllImport("kernel32.dll")] static extern bool AssignProcessToJobObject(IntPtr job, IntPtr proc);
    [DllImport("kernel32.dll")] static extern bool TerminateJobObject(IntPtr job, uint code);

    static string ObjName(IntPtr h) {
        var sb = new StringBuilder(256); int n;
        GetUserObjectInformation(h, 2 /*UOI_NAME*/, sb, 512, out n);
        return sb.ToString();
    }

    static IntPtr OpenOut(string path) {
        var sa = new SECURITY_ATTRIBUTES(); sa.nLength = Marshal.SizeOf(sa); sa.bInheritHandle = true;
        // GENERIC_WRITE, share read|write, CREATE_ALWAYS (or OPEN_EXISTING for NUL)
        uint disp = path == "NUL" ? 3u : 2u;
        IntPtr h = CreateFile(path, 0x40000000, 3, ref sa, disp, 0x80, IntPtr.Zero);
        if (h == new IntPtr(-1)) throw new Exception("CreateFile " + path + " failed: " + Marshal.GetLastWin32Error());
        return h;
    }
    static IntPtr OpenIn() {
        var sa = new SECURITY_ATTRIBUTES(); sa.nLength = Marshal.SizeOf(sa); sa.bInheritHandle = true;
        return CreateFile("NUL", 0x80000000, 3, ref sa, 3, 0x80, IntPtr.Zero);
    }

    public static List<string> ListWindows(IntPtr desk, int pid) {
        var res = new List<string>();
        EnumDesktopWindows(desk, (hwnd, l) => {
            int wpid; GetWindowThreadProcessId(hwnd, out wpid);
            if (wpid == pid) {
                var t = new StringBuilder(256); GetWindowText(hwnd, t, 256);
                var c = new StringBuilder(256); GetClassName(hwnd, c, 256);
                res.Add(String.Format("hwnd=0x{0:x} visible={1} class='{2}' title='{3}'", hwnd.ToInt64(), IsWindowVisible(hwnd), c, t));
            }
            return true;
        }, IntPtr.Zero);
        return res;
    }

    public static int Run(string exe, string args, string stdoutPath, string stderrPath, string cwd, int timeoutSec,
                          string deskName, int listAfterMs) {
        string winsta = ObjName(GetProcessWindowStation());
        IntPtr userDesk = GetThreadDesktop(GetCurrentThreadId());
        // GENERIC_ALL
        IntPtr desk = CreateDesktop(deskName, IntPtr.Zero, IntPtr.Zero, 0, 0x10000000, IntPtr.Zero);
        if (desk == IntPtr.Zero) throw new Exception("CreateDesktop failed: " + Marshal.GetLastWin32Error());
        string fullDesk = winsta + "\\" + deskName;
        Console.Error.WriteLine("[hidden_desktop] desktop " + fullDesk + " (launcher desktop: " + ObjName(userDesk) + ")");

        // no critical-error / GP-fault / open-file dialogs; inherited by the child
        SetErrorMode(0x0001 | 0x0002 | 0x8000);

        IntPtr hOut = OpenOut(String.IsNullOrEmpty(stdoutPath) ? "NUL" : stdoutPath);
        IntPtr hErr = OpenOut(String.IsNullOrEmpty(stderrPath) ? "NUL" : stderrPath);
        IntPtr hIn = OpenIn();

        var si = new STARTUPINFO(); si.cb = Marshal.SizeOf(si); si.lpDesktop = fullDesk;
        si.dwFlags = 0x100; // STARTF_USESTDHANDLES
        si.hStdInput = hIn; si.hStdOutput = hOut; si.hStdError = hErr;

        IntPtr job = CreateJobObject(IntPtr.Zero, null);
        var info = new JOBOBJECT_EXTENDED_LIMIT_INFORMATION();
        info.BasicLimitInformation.LimitFlags = 0x2000 | 0x400; // KILL_ON_JOB_CLOSE | DIE_ON_UNHANDLED_EXCEPTION
        SetInformationJobObject(job, 9, ref info, Marshal.SizeOf(info));

        var cmd = new StringBuilder("\"" + exe + "\" " + args);
        PROCESS_INFORMATION pi;
        // CREATE_SUSPENDED | CREATE_NO_WINDOW
        if (!CreateProcess(null, cmd, IntPtr.Zero, IntPtr.Zero, true, 0x4 | 0x08000000, IntPtr.Zero,
                           String.IsNullOrEmpty(cwd) ? null : cwd, ref si, out pi))
            throw new Exception("CreateProcess failed: " + Marshal.GetLastWin32Error());
        AssignProcessToJobObject(job, pi.hProcess);
        ResumeThread(pi.hThread);
        CloseHandle(hOut); CloseHandle(hErr); CloseHandle(hIn);
        Console.Error.WriteLine("[hidden_desktop] started pid " + pi.dwProcessId);

        uint total = (uint)timeoutSec * 1000;
        uint waited = 0;
        if (listAfterMs > 0) {
            uint r0 = WaitForSingleObject(pi.hProcess, (uint)listAfterMs);
            waited = (uint)listAfterMs;
            foreach (var w in ListWindows(desk, pi.dwProcessId)) Console.Error.WriteLine("[hidden_desktop] on hidden desktop: " + w);
            var uw = ListWindows(userDesk, pi.dwProcessId);
            Console.Error.WriteLine("[hidden_desktop] windows of pid on launcher desktop: " + uw.Count);
            foreach (var w in uw) Console.Error.WriteLine("[hidden_desktop] on launcher desktop: " + w);
        }
        uint r = WaitForSingleObject(pi.hProcess, total > waited ? total - waited : 0);
        uint code;
        if (r != 0) {
            Console.Error.WriteLine("[hidden_desktop] timeout after " + timeoutSec + "s, killing");
            TerminateJobObject(job, 124);
            WaitForSingleObject(pi.hProcess, 10000);
            code = 124;
        } else {
            GetExitCodeProcess(pi.hProcess, out code);
        }
        CloseHandle(job); // kills leftover children
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        if (!CloseDesktop(desk)) Console.Error.WriteLine("[hidden_desktop] CloseDesktop failed: " + Marshal.GetLastWin32Error());
        Console.Error.WriteLine("[hidden_desktop] exit code " + code);
        return (int)code;
    }
}
"@

if (-not ("HiddenDesktop" -as [type])) { Add-Type -TypeDefinition $src -Language CSharp }

function Resolve-Full([string]$p) {
    if ([string]::IsNullOrEmpty($p)) { return "" }
    return $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($p)
}
$exeFull = Resolve-Full $Exe
if (-not (Test-Path $exeFull)) { $exeFull = (Get-Command $Exe).Source }
$code = [HiddenDesktop]::Run($exeFull, $Arguments, (Resolve-Full $Stdout), (Resolve-Full $Stderr), (Resolve-Full $WorkDir),
                             $TimeoutSec, $Desktop, $ListWindowsAfterMs)
exit $code
