# Phase 4: window activation, the Windows half of
# tests/window_activation_smoke_test.md. Four cases, each against a fresh
# instance: a launch from Explorer, a session opened from the app's own File >
# Open, a session opened from Explorer while the window is minimized, and one
# opened from Explorer over an unsaved edit. Windows may refuse a process the
# foreground, so a case passes on either answer the manual accepts: the
# instance holds the foreground, or the shell was told to flash its taskbar
# button.
#
# What this console sees, it sees through a probe that pumps messages on its
# own thread for the whole phase: the shell hook (HSHELL_FLASH 0x8006 names
# the window flashing), every change of foreground window, and every
# DuskStudio.exe start and exit with its parent and exit code, on one clock.
# Prompts and alerts are embedded in the main window, so they are found
# through UI Automation by the accessible title each one sets.
#
# The session has no file association on Windows, so "from outside" is the
# route Explorer's Open with takes: Explorer runs DuskStudio.exe "<session>".
# Here a shortcut with that command line is opened through a File Explorer
# window's own ShellExecute, so the process is a child of explorer.exe, as it
# is from a double-click. A process Explorer starts takes Explorer's
# environment, not this console's, so the launch that starts an instance (case
# 1) puts the private config and music folders in the user environment, and has
# Explorer reread it, for the length of the launch; the handoffs (cases 3 and 4)
# exit before they write anything.
#
# Input reaches the app as a person's would, through SendInput: a click on a
# window to bring it to the front, keys with their scan codes, modifiers held
# across the key they modify.

# A compile error otherwise reaches only the guest's console.
$rgTypeError = ''
if (-not ('RegressActivation.Watch' -as [type])) {
    try {
        Add-Type -ErrorAction Stop -ReferencedAssemblies System.Windows.Forms, UIAutomationClient, UIAutomationTypes, WindowsBase -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Windows.Automation;
using System.Windows.Forms;

namespace RegressActivation
{
public struct RECT { public int Left, Top, Right, Bottom; }
public struct POINT { public int X, Y; }

[StructLayout(LayoutKind.Sequential)]
public struct MOUSEINPUT { public int dx, dy; public uint mouseData, dwFlags, time; public IntPtr dwExtraInfo; }
[StructLayout(LayoutKind.Sequential)]
public struct KEYBDINPUT { public ushort wVk, wScan; public uint dwFlags, time; public IntPtr dwExtraInfo; }
[StructLayout(LayoutKind.Explicit)]
public struct InputUnion { [FieldOffset(0)] public MOUSEINPUT mi; [FieldOffset(0)] public KEYBDINPUT ki; }
[StructLayout(LayoutKind.Sequential)]
public struct INPUT { public uint type; public InputUnion u; }

[StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
public struct PROCESSENTRY32
{
    public uint dwSize, cntUsage, th32ProcessID;
    public IntPtr th32DefaultHeapID;
    public uint th32ModuleID, cntThreads, th32ParentProcessID;
    public int pcPriClassBase;
    public uint dwFlags;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 260)] public string szExeFile;
}

public class Win
{
    public long Hwnd; public int Pid; public string Title; public string Class;
    public bool Visible, Iconic; public long Owner; public long ExStyle; public RECT Rect;
    public override string ToString()
    {
        return string.Format("0x{0:x} pid={1} '{2}' visible={3} iconic={4} rect={5},{6},{7},{8}",
            Hwnd, Pid, Title, Visible, Iconic, Rect.Left, Rect.Top, Rect.Right, Rect.Bottom);
    }
}

public class Ev
{
    public double T; public string Kind; public int Code; public long Hwnd; public int Pid; public string Text;
    public override string ToString()
    {
        return string.Format("{0,9:0} ms {1} code=0x{2:x} hwnd=0x{3:x} pid={4} {5}", T, Kind, Code, Hwnd, Pid, Text);
    }
}

public static class Api
{
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out int pid);
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern IntPtr GetWindow(IntPtr h, uint cmd);
    [DllImport("user32.dll", EntryPoint = "GetWindowLongPtrW")] public static extern IntPtr GetWindowLongPtr(IntPtr h, int i);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern IntPtr WindowFromPoint(POINT p);
    [DllImport("user32.dll")] public static extern IntPtr GetAncestor(IntPtr h, uint flags);
    [DllImport("user32.dll")] public static extern bool RegisterShellHookWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool DeregisterShellHookWindow(IntPtr h);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern uint RegisterWindowMessage(string s);
    [DllImport("user32.dll", SetLastError = true)] public static extern uint SendInput(uint n, INPUT[] inputs, int size);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint mapType);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern short VkKeyScan(char c);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr SendMessageTimeout(IntPtr h, uint m, IntPtr w, string l, uint flags, uint timeout, out IntPtr result);
    [DllImport("kernel32.dll")] public static extern IntPtr CreateToolhelp32Snapshot(uint flags, uint pid);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] public static extern bool Process32FirstW(IntPtr snap, ref PROCESSENTRY32 e);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] public static extern bool Process32NextW(IntPtr snap, ref PROCESSENTRY32 e);
    [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr h);
    [DllImport("kernel32.dll")] public static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
    [DllImport("kernel32.dll")] public static extern uint WaitForSingleObject(IntPtr h, uint ms);
    [DllImport("kernel32.dll")] public static extern bool GetExitCodeProcess(IntPtr h, out uint code);

    delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc cb, IntPtr l);

    public static int OwnerPid(IntPtr h) { int pid; GetWindowThreadProcessId(h, out pid); return pid; }
    public static int ForegroundPid() { return OwnerPid(GetForegroundWindow()); }

    public static Win Describe(IntPtr h)
    {
        var w = new Win();
        w.Hwnd = h.ToInt64();
        w.Pid = OwnerPid(h);
        var sb = new StringBuilder(512);
        GetWindowText(h, sb, 512); w.Title = sb.ToString();
        sb.Clear(); GetClassName(h, sb, 512); w.Class = sb.ToString();
        w.Visible = IsWindowVisible(h); w.Iconic = IsIconic(h);
        w.Owner = GetWindow(h, 4).ToInt64();
        w.ExStyle = GetWindowLongPtr(h, -20).ToInt64();
        RECT r; GetWindowRect(h, out r); w.Rect = r;
        return w;
    }

    public static Win[] TopLevel()
    {
        var all = new List<Win>();
        EnumWindows((h, l) => { all.Add(Describe(h)); return true; }, IntPtr.Zero);
        return all.ToArray();
    }

    // A main window as the taskbar counts one: unowned, titled, not a tool
    // window, shown or minimized. JUCE's drop shadows are untitled, and its
    // hidden message windows are neither shown nor minimized.
    public static Win[] MainWindows(int[] pids)
    {
        var set = new HashSet<int>(pids);
        var found = new List<Win>();
        foreach (var w in TopLevel())
            if (set.Contains(w.Pid) && w.Owner == 0 && w.Title.Length > 0
                && (w.ExStyle & 0x80) == 0 && (w.Visible || w.Iconic))
                found.Add(w);
        return found.ToArray();
    }

    // The top-level window that owns the pixel at x, y.
    public static long RootAt(int x, int y)
    {
        return GetAncestor(WindowFromPoint(new POINT { X = x, Y = y }), 2).ToInt64();
    }

    public static long RootAt(System.Windows.Rect r)
    {
        return RootAt((int)(r.Left + r.Width / 2), (int)(r.Top + r.Height / 2));
    }

    public static Dictionary<int, PROCESSENTRY32> Processes()
    {
        var map = new Dictionary<int, PROCESSENTRY32>();
        var snap = CreateToolhelp32Snapshot(2, 0);
        if (snap == new IntPtr(-1)) return map;
        var e = new PROCESSENTRY32(); e.dwSize = (uint)Marshal.SizeOf(typeof(PROCESSENTRY32));
        if (Process32FirstW(snap, ref e))
            do { map[(int)e.th32ProcessID] = e; } while (Process32NextW(snap, ref e));
        CloseHandle(snap);
        return map;
    }

    // With its scan code, as a keyboard sends it: JUCE reads the key of a
    // WM_CHAR back from the scan code, so Ctrl+O without one arrives as 0x0F.
    static INPUT Key(ushort vk, bool up)
    {
        var i = new INPUT(); i.type = 1; i.u.ki.wVk = vk;
        i.u.ki.wScan = (ushort)MapVirtualKey(vk, 0);
        i.u.ki.dwFlags = up ? 2u : 0u;
        return i;
    }

    static void Send(INPUT[] inputs)
    {
        uint sent = SendInput((uint)inputs.Length, inputs, Marshal.SizeOf(typeof(INPUT)));
        if (sent != inputs.Length)
            throw new InvalidOperationException("SendInput sent " + sent + " of " + inputs.Length + ", error " + Marshal.GetLastWin32Error());
    }

    // The last key is struck while the others are held. JUCE reads modifiers
    // from GetAsyncKeyState when it handles the message, so a chord sent in one
    // batch has already let go of Ctrl by then.
    public static void Chord(ushort[] keys)
    {
        for (int i = 0; i < keys.Length - 1; ++i) { Send(new[] { Key(keys[i], false) }); Thread.Sleep(40); }
        var last = keys[keys.Length - 1];
        Send(new[] { Key(last, false) });
        Thread.Sleep(30);
        Send(new[] { Key(last, true) });
        Thread.Sleep(keys.Length > 1 ? 120 : 10);
        for (int i = keys.Length - 2; i >= 0; --i) { Send(new[] { Key(keys[i], true) }); Thread.Sleep(20); }
    }

    public static void Type(string s)
    {
        foreach (var c in s)
        {
            short scan = VkKeyScan(c);
            if (scan == -1) throw new ArgumentException("no key on this layout types '" + c + "'");
            var vk = (ushort)(scan & 0xff);
            Chord((scan & 0x100) != 0 ? new ushort[] { 0x10, vk } : new ushort[] { vk });
            Thread.Sleep(20);
        }
    }

    public static void Click(int x, int y)
    {
        SetCursorPos(x, y);
        Thread.Sleep(80);
        var down = new INPUT(); down.type = 0; down.u.mi.dwFlags = 2;
        Send(new[] { down });
        Thread.Sleep(60);
        var up = new INPUT(); up.type = 0; up.u.mi.dwFlags = 4;
        Send(new[] { up });
    }

    public static bool BroadcastEnvironment()
    {
        IntPtr r;
        return SendMessageTimeout(new IntPtr(0xffff), 0x1A, IntPtr.Zero, "Environment", 2, 5000, out r) != IntPtr.Zero;
    }

    // Embedded modals are the last children the window lists: JUCE's tree is
    // flat, thousands of elements each a cross-process call, and a modal goes
    // in above everything already there.
    public static AutomationElement FindLast(IntPtr hwnd, string name, int maxSiblings)
    {
        var walker = TreeWalker.RawViewWalker;
        try
        {
            int seen = 0;
            for (var c = walker.GetLastChild(AutomationElement.FromHandle(hwnd)); c != null && seen < maxSiblings;
                 c = walker.GetPreviousSibling(c), ++seen)
                if (c.Current.Name == name) return c;
        }
        catch (ElementNotAvailableException) { }
        return null;
    }

    public static AutomationElement NextSibling(AutomationElement e)
    {
        return TreeWalker.RawViewWalker.GetNextSibling(e);
    }

    public static string Describe(AutomationElement e)
    {
        if (e == null) return "<none>";
        var c = e.Current;
        return c.ControlType.ProgrammaticName + " '" + c.Name + "' pid=" + c.ProcessId
            + " rect=" + c.BoundingRectangle + " offscreen=" + c.IsOffscreen;
    }

    public static AutomationElement Focused() { return AutomationElement.FocusedElement; }
}

// Pumps messages on its own thread for the whole phase, so the shell's
// notifications are seen as they are sent, and samples the foreground window
// and the DuskStudio.exe processes every 15 ms on the same clock.
public sealed class Watch : NativeWindow
{
    readonly List<Ev> events = new List<Ev>();
    readonly Stopwatch clock = Stopwatch.StartNew();
    readonly ManualResetEvent ready = new ManualResetEvent(false);
    readonly Dictionary<int, IntPtr> handles = new Dictionary<int, IntPtr>();
    readonly HashSet<int> exited = new HashSet<int>();
    Thread thread;
    uint shellMsg;
    bool hooked;
    IntPtr lastForeground = new IntPtr(-1);

    public void Start()
    {
        thread = new Thread(Run);
        thread.SetApartmentState(ApartmentState.STA);
        thread.IsBackground = true;
        thread.Start();
        if (!ready.WaitOne(10000)) throw new InvalidOperationException("the shell watch did not start");
        if (!hooked) throw new InvalidOperationException("RegisterShellHookWindow failed");
    }

    void Run()
    {
        CreateHandle(new CreateParams { Caption = "dusk-regress-shell-watch" });
        shellMsg = Api.RegisterWindowMessage("SHELLHOOK");
        hooked = Api.RegisterShellHookWindow(Handle);
        var timer = new System.Windows.Forms.Timer { Interval = 15 };
        timer.Tick += (s, e) => Sample();
        timer.Start();
        Sample();
        ready.Set();
        Application.Run();
        timer.Stop();
        Api.DeregisterShellHookWindow(Handle);
        DestroyHandle();
    }

    public void Stop()
    {
        if (thread == null) return;
        Api.PostMessage(Handle, 0x8001, IntPtr.Zero, IntPtr.Zero);
        thread.Join(10000);
        thread = null;
        lock (events) { foreach (var h in handles.Values) if (h != IntPtr.Zero) Api.CloseHandle(h); handles.Clear(); }
    }

    protected override void WndProc(ref Message m)
    {
        if (m.Msg == 0x8001) { Application.ExitThread(); return; }
        if (shellMsg != 0 && m.Msg == (int)shellMsg)
        {
            int code = (int)m.WParam.ToInt64();
            // Created and destroyed windows, of every process, are noise here;
            // DuskStudio's own windows are counted by enumeration instead.
            if (code != 1 && code != 2 && code != 0x10)
            {
                var w = Api.Describe(m.LParam);
                Add("shell", code, w.Hwnd, w.Pid, "'" + w.Title + "' " + w.Class);
            }
        }
        base.WndProc(ref m);
    }

    void Add(string kind, int code, long hwnd, int pid, string text)
    {
        lock (events) events.Add(new Ev { T = clock.Elapsed.TotalMilliseconds, Kind = kind, Code = code, Hwnd = hwnd, Pid = pid, Text = text });
    }

    public void Mark(string text) { Add("mark", 0, 0, 0, text); }
    public double Now() { return clock.Elapsed.TotalMilliseconds; }
    public Ev[] Events() { lock (events) return events.ToArray(); }

    void Sample()
    {
        var fg = Api.GetForegroundWindow();
        if (fg != lastForeground)
        {
            lastForeground = fg;
            var w = Api.Describe(fg);
            Add("foreground", 0, fg.ToInt64(), w.Pid, "'" + w.Title + "' " + w.Class);
        }
        var procs = Api.Processes();
        foreach (var kv in procs)
        {
            if (!string.Equals(kv.Value.szExeFile, "DuskStudio.exe", StringComparison.OrdinalIgnoreCase)) continue;
            if (handles.ContainsKey(kv.Key)) continue;
            // SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, held so the exit
            // code outlives a handoff that is over in a fifth of a second.
            handles[kv.Key] = Api.OpenProcess(0x00100000 | 0x1000, false, kv.Key);
            PROCESSENTRY32 parent;
            int parentPid = (int)kv.Value.th32ParentProcessID;
            Add("process-start", 0, 0, kv.Key, "parent=" + parentPid + " "
                + (procs.TryGetValue(parentPid, out parent) ? parent.szExeFile : "?"));
        }
        foreach (var kv in handles)
        {
            if (kv.Value == IntPtr.Zero || exited.Contains(kv.Key) || Api.WaitForSingleObject(kv.Value, 0) != 0) continue;
            uint code;
            Api.GetExitCodeProcess(kv.Value, out code);
            exited.Add(kv.Key);
            Add("process-exit", (int)code, 0, kv.Key, "exit=" + code);
        }
    }
}
}
'@
    } catch {
        $rgTypeError = "probe types did not compile: $($_.Exception.Message)`n"
    }
}

# The main window a person sees, by its title: MainWindowHandle can be one of
# the drop-shadow windows around it.
function Get-WaMain($procId) {
    return [RegressActivation.Api]::MainWindows([int[]]@($procId)) |
        Where-Object { $_.Title -eq 'Dusk Studio' } | Select-Object -First 1
}

function Get-WaAppPids {
    return @(Get-Process DuskStudio -ErrorAction SilentlyContinue | ForEach-Object { $_.Id })
}

# Every instance gone, so the next launch is a primary and not a handoff into
# a leftover. Stop-RegressApps does not wait for the exit.
function Stop-WaApps {
    Stop-RegressApps
    $until = (Get-Date).AddSeconds(15)
    while ((Get-WaAppPids).Count -gt 0 -and (Get-Date) -lt $until) { Start-Sleep -Milliseconds 200 }
    if ((Get-WaAppPids).Count -gt 0) { throw "DuskStudio.exe still running: $((Get-WaAppPids) -join ', ')" }
}

# Only where the window meant is the one under the pointer. Two clicks on one
# spot within the double-click time are a double-click, and one on a title bar
# maximizes the window.
function Invoke-WaClick($hwnd, $x, $y) {
    $at = [RegressActivation.Api]::RootAt([int]$x, [int]$y)
    if ($at -ne $hwnd) { throw "a click at $([int]$x),$([int]$y) would land on 0x$('{0:x}' -f $at), not 0x$('{0:x}' -f $hwnd)" }
    [RegressActivation.Api]::Click([int]$x, [int]$y)
    Start-Sleep -Milliseconds 700
}

function Wait-WaForeground($procId, $seconds) {
    $until = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $until) {
        if ([RegressActivation.Api]::ForegroundPid() -eq $procId) { return $true }
        Start-Sleep -Milliseconds 100
    }
    return $false
}

# Brings Notepad to the front with a click in its text, which places the caret
# and changes nothing. Windows keeps another process's window from being moved
# over the active one, so Notepad is always-on-top only for as long as it takes
# to put it where the click can reach it; the click then raises it for good.
function Set-WaNotepadFront($x, $y, $w, $h) {
    $np = [IntPtr]$rgNotepad.Hwnd
    [void][RegressActivation.Api]::SetWindowPos($np, [IntPtr]-1, $x, $y, $w, $h, 0x50)
    Start-Sleep -Milliseconds 300
    try {
        Invoke-WaClick $rgNotepad.Hwnd ($x + $w / 2) ($y + $h / 2)
    } finally {
        [void][RegressActivation.Api]::SetWindowPos($np, [IntPtr]-2, 0, 0, 0, 0, 0x13)
    }
    if (-not (Wait-WaForeground $rgNotepad.Pid 5)) { throw 'Notepad did not come to the front' }
}

function Start-WaPrimary($case, $session) {
    $envs = New-RegressSandbox "activation-$case"
    $envs['DUSKSTUDIO_LOAD_SESSION'] = $session
    $name = Split-Path $session -Leaf
    $p = Start-RegressApp $envs ''
    $main = $null
    $loaded = $false
    $until = (Get-Date).AddSeconds(60)
    while ((Get-Date) -lt $until -and -not $p.HasExited) {
        $err = Read-RegressStderr $p.RgErr 300
        if (-not $loaded) { $loaded = $err.Contains("[Dusk Studio/Load] $name") }
        if (-not $main) { $main = Get-WaMain $p.Id }
        if ($loaded -and $main) { break }
    }
    if (-not $loaded -or -not $main) { throw "the instance did not load $name and show its window (alive=$(-not $p.HasExited))" }
    # The load re-asserts the window from a later message-loop tick.
    Start-Sleep -Seconds 2
    $p | Add-Member -NotePropertyName WaMain -NotePropertyValue (Get-WaMain $p.Id)
    return $p
}

function Save-WaPrimaryLog($case, $p) {
    if ($null -eq $p) { return }
    if (-not $p.HasExited) { try { $p.Kill() } catch { } }
    $p.WaitForExit(20000) | Out-Null
    Stop-RegressChildren
    $err = Read-RegressStderr $p.RgErr 2000
    Set-Content -Path "$rgWa\$case-primary.log" -Value ((Read-RegressTask $p.RgOut 10000) + "`n---stderr---`n" + $err)
}

# Opens a shortcut the way a double-click in File Explorer does: by File
# Explorer's own ShellExecute, in its own process.
function Invoke-WaExplorerOpen($lnk) {
    $rgFolder.Document.Application.ShellExecute($lnk, '', '', 'open', 1)
}

# Only these names, and only while the harness owns them: the marker says so.
$rgWaEnvNames = @('DUSKSTUDIO_CONFIG_DIR', 'DUSKSTUDIO_MUSIC_DIR', 'DUSKSTUDIO_REGRESS_ENV_OWNER')
function Clear-WaUserEnv {
    $key = Get-Item 'HKCU:\Environment'
    if ($null -eq $key.GetValue('DUSKSTUDIO_REGRESS_ENV_OWNER')) { return }
    foreach ($n in $rgWaEnvNames) { Remove-ItemProperty 'HKCU:\Environment' -Name $n -ErrorAction SilentlyContinue }
    [void][RegressActivation.Api]::BroadcastEnvironment()
}

# Taken after the last observation of a case, so the evidence matches the verdict.
function Save-WaShot($label) {
    if (Save-RegressShot "phase4-$label") { return "shot-phase4-$label.png" }
    return 'not taken: the host did not answer'
}

# The verdict inputs every case shares, from the watch's record since $since and
# the windows as they are now.
function Get-WaObservation($primaryPid, $mainHwnd, $since) {
    $events = @($rgWatch.Events() | Where-Object { $_.T -ge $since })
    $flashes = @($events | Where-Object { $_.Kind -eq 'shell' -and $_.Code -eq 0x8006 -and $_.Pid -eq $primaryPid })
    $raised = @($events | Where-Object { $_.Kind -eq 'foreground' -and $_.Pid -eq $primaryPid })
    $main = [RegressActivation.Api]::Describe([IntPtr]$mainHwnd)
    $pids = Get-WaAppPids
    $mains = @([RegressActivation.Api]::MainWindows([int[]]$pids))
    $o = @{
        Foreground = ([RegressActivation.Api]::ForegroundPid() -eq $primaryPid)
        FirstForegroundMs = $(if ($raised.Count -gt 0) { [int]($raised[0].T - $since) } else { $null })
        Flashes = $flashes.Count
        FirstFlashMs = $(if ($flashes.Count -gt 0) { [int]($flashes[0].T - $since) } else { $null })
        Visible = $main.Visible
        Iconic = $main.Iconic
        MainWindows = $mains.Count
        Processes = $pids.Count
        Events = $events
    }
    $o.FocusOrFlash = $o.Foreground -or $o.Flashes -gt 0
    $o.Summary = "foreground=$($o.Foreground) (first after $($o.FirstForegroundMs) ms) flashes=$($o.Flashes) (first after $($o.FirstFlashMs) ms) " +
        "visible=$($o.Visible) iconic=$($o.Iconic) main windows=$($o.MainWindows) ($(($mains | ForEach-Object { $_.ToString() }) -join '; ')) DuskStudio processes=$($o.Processes)"
    return $o
}

# The DuskStudio.exe Explorer started for a case, and how it ended.
function Get-WaLaunched($since, $exclude) {
    $start = $rgWatch.Events() | Where-Object { $_.T -ge $since -and $_.Kind -eq 'process-start' -and $_.Pid -ne $exclude } |
        Select-Object -First 1
    if (-not $start) { return $null }
    $exit = $rgWatch.Events() | Where-Object { $_.Kind -eq 'process-exit' -and $_.Pid -eq $start.Pid } | Select-Object -First 1
    return @{ Pid = $start.Pid; Parent = $start.Text; FromExplorer = ($start.Text -match ' explorer\.exe$')
        Exited = ($null -ne $exit); ExitCode = $(if ($exit) { $exit.Code } else { $null }) }
}

function Wait-WaExit($since, $exclude, $seconds) {
    $until = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $until) {
        $l = Get-WaLaunched $since $exclude
        if ($l -and $l.Exited) { return $l }
        Start-Sleep -Milliseconds 200
    }
    return Get-WaLaunched $since $exclude
}

# An embedded prompt or alert counts as visible when it is in the main window's
# accessibility tree, on screen, and the pixel at its centre belongs to the main
# window rather than to whatever is in front of it.
function Test-WaShown($mainHwnd, $name) {
    $e = [RegressActivation.Api]::FindLast([IntPtr]$mainHwnd, $name, 60)
    if ($null -eq $e) { return @{ Shown = $false; Text = "'$name' not in the window's accessibility tree" } }
    $rect = $e.Current.BoundingRectangle
    $root = [RegressActivation.Api]::RootAt($rect)
    $shown = (-not $e.Current.IsOffscreen) -and $root -eq $mainHwnd
    return @{ Shown = $shown; Text = "'$name' $([RegressActivation.Api]::Describe($e)), centre belongs to 0x$('{0:x}' -f $root)" }
}

# A case passes when it misses none of its checks; the misses are named on an
# error line, which the host puts in the leg's row of the summary.
function Get-WaUnmet($checks) {
    return @($checks.Keys | Where-Object { -not $checks[$_] })
}

function Format-WaCase($case, $unmet) {
    if ($unmet.Count -eq 0) { return "CASE $case PASS`n" }
    return "CASE $case FAIL`nerror: $case missed: $($unmet -join '; ')`n"
}

function Get-WaHow($o) {
    if ($o.Foreground) { return 'foreground' }
    if ($o.Flashes -gt 0) { return 'taskbar flash' }
    return 'neither foreground nor flash'
}

$rgWa = "$rgRoot\activation"
$rgWatch = $null
$rgNotepad = $null
$rgNotepadState = $null
$rgFolder = $null
$rgNotes = @()
$rgVerdicts = @()
$rgCaseLog = ''
$rgP = $null
try {
    if ($rgTypeError) { throw $rgTypeError }
    Stop-WaApps
    $rgExe = Get-RegressExe
    $rgLog += "executable: $rgExe FileVersion=$((Get-Item -LiteralPath $rgExe).VersionInfo.FileVersion)`n"
    Clear-WaUserEnv
    foreach ($n in $rgWaEnvNames) {
        if ($null -ne (Get-Item 'HKCU:\Environment').GetValue($n)) { throw "the user environment already sets $n; it is not the harness's to change" }
    }

    if (Test-Path $rgWa) { Remove-Item -Recurse -Force $rgWa }
    foreach ($d in 'a', 'b', 'c', 'launch') { New-Item -ItemType Directory -Force -Path "$rgWa\$d" | Out-Null }
    $rgA = "$rgWa\a\wa-a.json"
    $rgB = "$rgWa\b\wa-b.json"
    $rgC = "$rgWa\c\wa-c.json"
    Set-Content -Path $rgA -Value '{ "version": 6 }'
    Set-Content -Path $rgB -Value '{ "version": 6 }'
    # A region whose audio is missing, so opening this one raises a load alert.
    Set-Content -Path $rgC -Value '{ "version": 6, "tracks": [ { "name": "T", "regions": [ { "file": "audio/wa-missing.wav", "timeline_start": 0, "length": 48000, "source_offset": 0 } ] } ] }'
    $rgShell = New-Object -ComObject WScript.Shell
    $rgLaunchLnk = "$rgWa\launch\Dusk Studio.lnk"
    $rgOpenLnk = "$rgWa\launch\Open wa-b.lnk"
    foreach ($pair in @(@($rgLaunchLnk, ''), @($rgOpenLnk, "`"$rgB`""))) {
        $l = $rgShell.CreateShortcut($pair[0])
        $l.TargetPath = $rgExe
        $l.Arguments = $pair[1]
        $l.WorkingDirectory = (Split-Path $rgExe)
        $l.Save()
    }

    $rgWatch = New-Object RegressActivation.Watch
    $rgWatch.Start()

    # Notepad reopens its last session's tabs and saves them again when it
    # closes, so its saved state is put back as it was once it has gone.
    $rgNotepadPkg = Get-ChildItem "$env:LOCALAPPDATA\Packages" -Directory -Filter 'Microsoft.WindowsNotepad_*' -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ((Get-Process Notepad -ErrorAction SilentlyContinue) -or -not $rgNotepadPkg) {
        $rgLog += "REGRESS-NOTE Notepad's saved session was not backed up (Notepad running or not the packaged one)`n"
    } elseif (Test-Path "$($rgNotepadPkg.FullName)\LocalState") {
        $rgNotepadState = "$rgWa\notepad-localstate"
        Copy-Item -Recurse -Force "$($rgNotepadPkg.FullName)\LocalState" $rgNotepadState
    }
    $rgBefore = @([RegressActivation.Api]::TopLevel() | Where-Object { $_.Class -eq 'Notepad' } | ForEach-Object { $_.Hwnd })
    Start-Process notepad.exe
    $rgUntil = (Get-Date).AddSeconds(20)
    while (-not $rgNotepad -and (Get-Date) -lt $rgUntil) {
        Start-Sleep -Milliseconds 300
        $rgNotepad = [RegressActivation.Api]::TopLevel() |
            Where-Object { $_.Class -eq 'Notepad' -and $_.Visible -and $rgBefore -notcontains $_.Hwnd } | Select-Object -First 1
    }
    if (-not $rgNotepad) { throw 'Notepad did not open a window' }
    $rgLog += "Notepad: $rgNotepad`n"

    $rgShellApp = New-Object -ComObject Shell.Application
    $rgBefore = @($rgShellApp.Windows() | ForEach-Object { $_.HWND })
    Start-Process explorer.exe -ArgumentList "`"$rgWa\launch`""
    $rgUntil = (Get-Date).AddSeconds(20)
    while (-not $rgFolder -and (Get-Date) -lt $rgUntil) {
        Start-Sleep -Milliseconds 300
        foreach ($w in $rgShellApp.Windows()) {
            if ($rgBefore -notcontains $w.HWND -and "$($w.Document.Folder.Self.Path)" -eq "$rgWa\launch") { $rgFolder = $w }
        }
    }
    if (-not $rgFolder) { throw "no File Explorer window opened on $rgWa\launch" }
    $rgFolderWin = [RegressActivation.Api]::Describe([IntPtr]$rgFolder.HWND)
    $rgLog += "File Explorer: $rgFolderWin`n"
    # Out of the way: the cases open shortcuts through it, never by clicking it.
    [void][RegressActivation.Api]::PostMessage([IntPtr]$rgFolder.HWND, 0x112, [IntPtr]0xF020, [IntPtr]::Zero)
    Start-Sleep -Seconds 1

    # ---- 1. Initial launch: Notepad in front, Dusk Studio opened from Explorer.
    $rgCase = 'initial-launch'
    try {
        Stop-WaApps
        Set-WaNotepadFront 360 160 1200 700
        $rgEnv = New-RegressSandbox "activation-$rgCase"
        $rgCfg = $rgEnv['DUSKSTUDIO_CONFIG_DIR']
        Set-ItemProperty 'HKCU:\Environment' -Name DUSKSTUDIO_REGRESS_ENV_OWNER -Value $rgRoot
        Set-ItemProperty 'HKCU:\Environment' -Name DUSKSTUDIO_CONFIG_DIR -Value $rgCfg
        Set-ItemProperty 'HKCU:\Environment' -Name DUSKSTUDIO_MUSIC_DIR -Value $rgEnv['DUSKSTUDIO_MUSIC_DIR']
        [void][RegressActivation.Api]::BroadcastEnvironment()
        Start-Sleep -Seconds 1
        if (-not (Wait-WaForeground $rgNotepad.Pid 2)) { Set-WaNotepadFront 360 160 1200 700 }
        $rgSince = $rgWatch.Now()
        $rgWatch.Mark("$rgCase open shortcut")
        Invoke-WaExplorerOpen $rgLaunchLnk
        $rgLaunched = $null
        $rgMain = $null
        $rgUntil = (Get-Date).AddSeconds(45)
        while ((Get-Date) -lt $rgUntil) {
            if (-not $rgLaunched) { $rgLaunched = Get-WaLaunched $rgSince 0 }
            if ($rgLaunched) {
                Clear-WaUserEnv
                $rgMain = Get-WaMain $rgLaunched.Pid
                if ($rgMain) { break }
            }
            Start-Sleep -Milliseconds 200
        }
        Clear-WaUserEnv
        if (-not $rgLaunched) { throw 'Explorer started no DuskStudio.exe' }
        if (-not $rgMain) { throw "pid $($rgLaunched.Pid) showed no main window" }
        # The window is shown, and asks for the foreground, from a later tick.
        Start-Sleep -Seconds 4
        $rgO = Get-WaObservation $rgLaunched.Pid $rgMain.Hwnd $rgSince
        $rgCfgFiles = @(Get-ChildItem -LiteralPath $rgCfg -Recurse -File -ErrorAction SilentlyContinue).Count
        $rgShot = Save-WaShot $rgCase
        $rgUnmet = Get-WaUnmet ([ordered]@{
            'foreground or taskbar flash' = $rgO.FocusOrFlash
            'window shown, not minimized' = $rgO.Visible -and -not $rgO.Iconic
            'exactly one main window' = $rgO.MainWindows -eq 1
            'started by explorer.exe' = $rgLaunched.FromExplorer
            'private config folder used' = $rgCfgFiles -gt 0
        })
        $rgVerdicts += "$rgCase $(if ($rgUnmet.Count) { 'FAIL' } else { 'PASS' })"
        $rgCaseLog += (Format-WaCase $rgCase $rgUnmet) + ("  launched pid=$($rgLaunched.Pid) $($rgLaunched.Parent); main $rgMain`n" +
            "  $($rgO.Summary)`n  private config folder holds $rgCfgFiles files (the Explorer launch carried it)`n  screenshot $rgShot`n")
        $rgNotes += "${rgCase}: $(Get-WaHow $rgO)"
    } catch {
        $rgVerdicts += "$rgCase FAIL"
        $rgCaseLog += "CASE $rgCase FAIL`nerror: ${rgCase}: $_`n"
    } finally {
        Clear-WaUserEnv
        Stop-WaApps
    }

    # ---- 2. Session open from inside: File > Open with Notepad partly over the
    # active window, opening a session whose missing audio raises a load alert.
    $rgCase = 'session-open'
    $rgP = $null
    try {
        Stop-WaApps
        $rgP = Start-WaPrimary $rgCase $rgA
        $rgMain = $rgP.WaMain
        $r = $rgMain.Rect
        Invoke-WaClick $rgMain.Hwnd (($r.Left + $r.Right) / 2) ($r.Top + 18)
        if (-not (Wait-WaForeground $rgP.Id 5)) { throw 'a click on the title bar did not bring Dusk Studio to the front' }
        # Over the strips, clear of the middle where the browser and alerts sit.
        # Only an always-on-top window can lie over the active one on Windows.
        [void][RegressActivation.Api]::SetWindowPos([IntPtr]$rgNotepad.Hwnd, [IntPtr]-1, $r.Left + 20, $r.Bottom - 470, 280, 420, 0x50)
        Start-Sleep -Milliseconds 500
        $rgCovered = [RegressActivation.Api]::RootAt($r.Left + 160, $r.Bottom - 260) -eq $rgNotepad.Hwnd
        if (-not $rgCovered -or -not (Wait-WaForeground $rgP.Id 1)) { throw "Notepad is not over the active window (covering=$rgCovered)" }
        $rgSince = $rgWatch.Now()
        $rgWatch.Mark("$rgCase ctrl+o")
        [RegressActivation.Api]::Chord([uint16[]]@(0x11, 0x4F))
        $rgUntil = (Get-Date).AddSeconds(10)
        $rgBox = $null
        while (-not $rgBox -and (Get-Date) -lt $rgUntil) {
            $l = [RegressActivation.Api]::FindLast([IntPtr]$rgMain.Hwnd, 'file:', 30)
            if ($l) { $rgBox = [RegressActivation.Api]::NextSibling($l) }
            if (-not $rgBox) { Start-Sleep -Milliseconds 300 }
        }
        if (-not $rgBox) { throw 'Ctrl+O opened no file browser' }
        $br = $rgBox.Current.BoundingRectangle
        Invoke-WaClick $rgMain.Hwnd ($br.Left + $br.Width / 2) ($br.Top + $br.Height / 2)
        $rgFocus = [RegressActivation.Api]::Focused()
        if ($rgFocus.Current.ControlType.ProgrammaticName -ne 'ControlType.Edit' -or $rgFocus.Current.ProcessId -ne $rgP.Id) {
            throw "the browser's name box did not take the keyboard: $([RegressActivation.Api]::Describe($rgFocus))"
        }
        [RegressActivation.Api]::Type($rgC)
        Start-Sleep -Milliseconds 300
        # A typed path moves the browser to its folder and leaves the name; a
        # second Return opens it.
        [RegressActivation.Api]::Chord([uint16[]]@(0x0D))
        Start-Sleep -Milliseconds 1500
        [RegressActivation.Api]::Chord([uint16[]]@(0x0D))
        $rgLoaded = $false
        $rgUntil = (Get-Date).AddSeconds(20)
        while (-not $rgLoaded -and (Get-Date) -lt $rgUntil) {
            $rgLoaded = (Read-RegressStderr $rgP.RgErr 300).Contains('[Dusk Studio/Load] wa-c.json')
        }
        if (-not $rgLoaded) { throw 'the second session did not load' }
        $rgUntil = (Get-Date).AddSeconds(10)
        do {
            Start-Sleep -Milliseconds 500
            $rgAlert = Test-WaShown $rgMain.Hwnd 'Missing audio files'
        } while (-not $rgAlert.Shown -and (Get-Date) -lt $rgUntil)
        Start-Sleep -Seconds 2
        $rgAlert = Test-WaShown $rgMain.Hwnd 'Missing audio files'
        $rgO = Get-WaObservation $rgP.Id $rgMain.Hwnd $rgSince
        $rgStillCovered = [RegressActivation.Api]::RootAt($r.Left + 160, $r.Bottom - 260) -eq $rgNotepad.Hwnd
        $rgShot = Save-WaShot $rgCase
        $rgUnmet = Get-WaUnmet ([ordered]@{
            'foreground or taskbar flash' = $rgO.FocusOrFlash
            'window shown, not minimized' = $rgO.Visible -and -not $rgO.Iconic
            'exactly one main window' = $rgO.MainWindows -eq 1
            'load alert visible' = $rgAlert.Shown
        })
        $rgVerdicts += "$rgCase $(if ($rgUnmet.Count) { 'FAIL' } else { 'PASS' })"
        $rgCaseLog += (Format-WaCase $rgCase $rgUnmet) + ("  wa-c.json loaded through File > Open; main $rgMain`n  $($rgO.Summary)`n" +
            "  load alert shown=$($rgAlert.Shown): $($rgAlert.Text)`n  Notepad (always on top) still over part of the window=$rgStillCovered`n  screenshot $rgShot`n")
        $rgNotes += "${rgCase}: $(Get-WaHow $rgO)"
    } catch {
        $rgVerdicts += "$rgCase FAIL"
        $rgCaseLog += "CASE $rgCase FAIL`nerror: ${rgCase}: $_`n"
    } finally {
        if ($rgNotepad) { [void][RegressActivation.Api]::SetWindowPos([IntPtr]$rgNotepad.Hwnd, [IntPtr]-2, 0, 0, 0, 0, 0x13) }
        Save-WaPrimaryLog $rgCase $rgP
        Stop-WaApps
    }

    # ---- 3. Minimized: the session opened from Explorer while the window is
    # minimized and Notepad is in front.
    $rgCase = 'minimized'
    $rgP = $null
    try {
        Stop-WaApps
        $rgP = Start-WaPrimary $rgCase $rgA
        $rgMain = $rgP.WaMain
        $r = $rgMain.Rect
        [void][RegressActivation.Api]::PostMessage([IntPtr]$rgMain.Hwnd, 0x112, [IntPtr]0xF020, [IntPtr]::Zero)
        $rgUntil = (Get-Date).AddSeconds(5)
        while (-not [RegressActivation.Api]::IsIconic([IntPtr]$rgMain.Hwnd) -and (Get-Date) -lt $rgUntil) { Start-Sleep -Milliseconds 100 }
        if (-not [RegressActivation.Api]::IsIconic([IntPtr]$rgMain.Hwnd)) { throw 'the window did not minimize' }
        Set-WaNotepadFront ([Math]::Max(0, $r.Left - 150)) 200 450 600
        $rgSince = $rgWatch.Now()
        $rgWatch.Mark("$rgCase open shortcut")
        Invoke-WaExplorerOpen $rgOpenLnk
        $rgSecond = Wait-WaExit $rgSince $rgP.Id 20
        $rgLoaded = $false
        $rgUntil = (Get-Date).AddSeconds(20)
        while (-not $rgLoaded -and (Get-Date) -lt $rgUntil) {
            $rgLoaded = (Read-RegressStderr $rgP.RgErr 300).Contains('[Dusk Studio/Load] wa-b.json')
        }
        Start-Sleep -Seconds 3
        $rgO = Get-WaObservation $rgP.Id $rgMain.Hwnd $rgSince
        $rgShot = Save-WaShot $rgCase
        $rgHanded = $rgSecond -and $rgSecond.FromExplorer -and $rgSecond.Exited -and $rgSecond.ExitCode -eq 0
        $rgUnmet = Get-WaUnmet ([ordered]@{
            'foreground or taskbar flash' = $rgO.FocusOrFlash
            'window restored' = $rgO.Visible -and -not $rgO.Iconic
            'exactly one main window' = $rgO.MainWindows -eq 1
            'one DuskStudio process' = $rgO.Processes -eq 1
            'second process from explorer.exe exited 0' = $rgHanded
            'handed-off session loaded' = $rgLoaded
        })
        $rgVerdicts += "$rgCase $(if ($rgUnmet.Count) { 'FAIL' } else { 'PASS' })"
        $rgCaseLog += (Format-WaCase $rgCase $rgUnmet) + ("  minimized before the open; second process " +
            $(if ($rgSecond) { "pid=$($rgSecond.Pid) $($rgSecond.Parent) exited=$($rgSecond.Exited) code=$($rgSecond.ExitCode)" } else { 'never seen' }) +
            "; wa-b.json loaded=$rgLoaded`n  $($rgO.Summary)`n  screenshot $rgShot`n")
        $rgNotes += "${rgCase}: $(Get-WaHow $rgO)"
    } catch {
        $rgVerdicts += "$rgCase FAIL"
        $rgCaseLog += "CASE $rgCase FAIL`nerror: ${rgCase}: $_`n"
    } finally {
        Save-WaPrimaryLog $rgCase $rgP
        Stop-WaApps
    }

    # ---- 4. Second-process handoff over an unsaved edit: a marker dropped with
    # M, then the session opened from Explorer with Notepad in front.
    $rgCase = 'handoff'
    $rgP = $null
    try {
        Stop-WaApps
        $rgP = Start-WaPrimary $rgCase $rgA
        $rgMain = $rgP.WaMain
        $r = $rgMain.Rect
        Invoke-WaClick $rgMain.Hwnd (($r.Left + $r.Right) / 2) ($r.Top + 18)
        if (-not (Wait-WaForeground $rgP.Id 5)) { throw 'a click on the title bar did not bring Dusk Studio to the front' }
        [RegressActivation.Api]::Chord([uint16[]]@(0x4D))
        $rgUntil = (Get-Date).AddSeconds(5)
        do { Start-Sleep -Milliseconds 300 } while (-not ([RegressActivation.Api]::FindLast([IntPtr]$rgMain.Hwnd, 'Rename', 20)) -and (Get-Date) -lt $rgUntil)
        if (-not ([RegressActivation.Api]::FindLast([IntPtr]$rgMain.Hwnd, 'Rename', 20))) { throw 'M dropped no marker (no naming prompt)' }
        # Escape keeps the marker's default name.
        [RegressActivation.Api]::Chord([uint16[]]@(0x1B))
        Start-Sleep -Seconds 1
        if ([RegressActivation.Api]::FindLast([IntPtr]$rgMain.Hwnd, 'Rename', 20)) { throw 'the marker naming prompt did not close' }
        # In front and over part of the window, clear of the middle where the
        # prompt opens: refused the foreground, the window stays under Notepad,
        # and the prompt is visible only where Notepad leaves it uncovered.
        Set-WaNotepadFront ([Math]::Max(0, $r.Left - 150)) 200 450 600
        $rgSince = $rgWatch.Now()
        $rgWatch.Mark("$rgCase open shortcut")
        Invoke-WaExplorerOpen $rgOpenLnk
        $rgSecond = Wait-WaExit $rgSince $rgP.Id 20
        $rgTitle = 'Save changes before opening another session?'
        $rgUntil = (Get-Date).AddSeconds(15)
        do {
            [void](Read-RegressStderr $rgP.RgErr 300)
            $rgPrompt = Test-WaShown $rgMain.Hwnd $rgTitle
        } while (-not $rgPrompt.Shown -and (Get-Date) -lt $rgUntil)
        Start-Sleep -Seconds 2
        $rgPrompt = Test-WaShown $rgMain.Hwnd $rgTitle
        $rgButtons = @("Save", "Don't Save", 'Cancel' | Where-Object { [RegressActivation.Api]::FindLast([IntPtr]$rgMain.Hwnd, $_, 60) })
        $rgO = Get-WaObservation $rgP.Id $rgMain.Hwnd $rgSince
        $rgLoadedB = (Read-RegressStderr $rgP.RgErr 300).Contains('[Dusk Studio/Load] wa-b.json')
        $rgShot = Save-WaShot $rgCase
        $rgHanded = $rgSecond -and $rgSecond.FromExplorer -and $rgSecond.Exited -and $rgSecond.ExitCode -eq 0
        $rgUnmet = Get-WaUnmet ([ordered]@{
            'foreground or taskbar flash' = $rgO.FocusOrFlash
            'window shown, not minimized' = $rgO.Visible -and -not $rgO.Iconic
            'exactly one main window' = $rgO.MainWindows -eq 1
            'one DuskStudio process' = $rgO.Processes -eq 1
            'second process from explorer.exe exited 0' = $rgHanded
            'save prompt visible' = $rgPrompt.Shown
            'Save, Don''t Save and Cancel offered' = $rgButtons.Count -eq 3
            'session held behind the prompt' = -not $rgLoadedB
        })
        $rgVerdicts += "$rgCase $(if ($rgUnmet.Count) { 'FAIL' } else { 'PASS' })"
        $rgCaseLog += (Format-WaCase $rgCase $rgUnmet) + ("  marker dropped (naming prompt seen and closed); second process " +
            $(if ($rgSecond) { "pid=$($rgSecond.Pid) $($rgSecond.Parent) exited=$($rgSecond.Exited) code=$($rgSecond.ExitCode)" } else { 'never seen' }) + "`n" +
            "  prompt shown=$($rgPrompt.Shown): $($rgPrompt.Text)`n  buttons: $($rgButtons -join ', '); wa-b.json loaded behind the prompt=$rgLoadedB`n" +
            "  $($rgO.Summary)`n  screenshot $rgShot`n")
        $rgNotes += "${rgCase}: $(Get-WaHow $rgO)"
    } catch {
        $rgVerdicts += "$rgCase FAIL"
        $rgCaseLog += "CASE $rgCase FAIL`nerror: ${rgCase}: $_`n"
    } finally {
        Save-WaPrimaryLog $rgCase $rgP
        Stop-WaApps
    }

    $rgLog += $rgCaseLog
    if ($rgVerdicts.Count -eq 4 -and @($rgVerdicts | Where-Object { $_ -like '* FAIL' }).Count -eq 0) { $rgResult = 'PASS' }
} catch {
    $rgLog += $rgCaseLog
    $rgLog += "phase4 failed: $_`n"
} finally {
    Clear-WaUserEnv
    try { Stop-WaApps } catch { $rgLog += "error: $_`n" }
    if ($rgNotepad) {
        $np = $rgNotepad.Pid
        [void][RegressActivation.Api]::PostMessage([IntPtr]$rgNotepad.Hwnd, 0x10, [IntPtr]::Zero, [IntPtr]::Zero)
        $rgUntil = (Get-Date).AddSeconds(15)
        while ((Get-Process -Id $np -ErrorAction SilentlyContinue) -and (Get-Date) -lt $rgUntil) { Start-Sleep -Milliseconds 200 }
        # A process left with no window of its own has saved its session already.
        if ((Get-Process -Id $np -ErrorAction SilentlyContinue) -and
                -not ([RegressActivation.Api]::TopLevel() | Where-Object { $_.Pid -eq $np -and $_.Class -eq 'Notepad' -and $_.Visible })) {
            Stop-Process -Id $np -Force -ErrorAction SilentlyContinue
            Start-Sleep -Seconds 1
        }
        if (Get-Process -Id $np -ErrorAction SilentlyContinue) {
            $rgLog += "error: Notepad (pid $np) did not close`n"
        } elseif ($rgNotepadState -and (Test-Path $rgNotepadState)) {
            $live = "$($rgNotepadPkg.FullName)\LocalState"
            Remove-Item -Recurse -Force $live -ErrorAction SilentlyContinue
            Copy-Item -Recurse -Force $rgNotepadState $live
            $rgLog += "Notepad's saved session put back`n"
        } elseif ($rgNotepadState) {
            $rgLog += "error: Notepad's backup at $rgNotepadState is gone; its saved session was left as Notepad wrote it`n"
        }
    }
    if ($rgFolder) { try { $rgFolder.Quit() } catch { } }
    # run-cleanup removes the run folder, Notepad's backup with it, only after this.
    if (Test-Path $rgWa) { Set-Content -Path "$rgWa\phase4.done" -Value '' }
    if ($null -ne $rgWatch) {
        $rgWatch.Stop()
        Invoke-RegressPost (($rgWatch.Events() | ForEach-Object { $_.ToString() }) -join "`n") 'phase4-window-activation-events.log'
    }
}

foreach ($n in $rgNotes) { $rgLog += "REGRESS-NOTE $n`n" }
$rgLog += "REGRESS-PHASE phase4-window-activation RESULT $rgResult`nREGRESS-PHASE phase4-window-activation END`n"
Invoke-RegressPost $rgLog

# iex runs the script in a child scope, so plain "exit" leaves the console
# open and repeat runs stack up windows in the guest.
[Environment]::Exit(0)
