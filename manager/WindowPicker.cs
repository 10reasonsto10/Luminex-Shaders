using System.Diagnostics;
using System.Runtime.InteropServices;

namespace LuminexManager;

// Window selection adapted from the user's PID Detector project.
internal static class WindowPicker
{
    [StructLayout(LayoutKind.Sequential)]
    private struct Point { public int X, Y; }

    internal static bool Down(int key) => (GetAsyncKeyState(key) & 0x8000) != 0;

    internal static nint WindowUnderCursor()
    {
        if (!GetCursorPos(out var point)) return 0;
        nint hit = WindowFromPoint(point);
        nint root = GetAncestor(hit, 2);
        return root != 0 ? root : hit;
    }

    internal static int ReadRobloxPid(nint window)
    {
        if (window == 0 || !IsWindow(window))
            throw new InvalidOperationException("That window has closed. Select another window.");
        if (GetWindowThreadProcessId(window, out uint pid) == 0)
            throw new InvalidOperationException("Could not identify that window's process.");
        using var process = Process.GetProcessById(checked((int)pid));
        if (!process.ProcessName.Equals("RobloxPlayerBeta", StringComparison.OrdinalIgnoreCase))
            throw new InvalidOperationException("Select a Roblox client window.");
        return process.Id;
    }

    [DllImport("user32.dll")] private static extern short GetAsyncKeyState(int key);
    [DllImport("user32.dll")] private static extern bool GetCursorPos(out Point point);
    [DllImport("user32.dll")] private static extern nint WindowFromPoint(Point point);
    [DllImport("user32.dll")] private static extern nint GetAncestor(nint window, uint flags);
    [DllImport("user32.dll")] private static extern bool IsWindow(nint window);
    [DllImport("user32.dll")] internal static extern uint GetWindowThreadProcessId(nint window, out uint pid);
}
