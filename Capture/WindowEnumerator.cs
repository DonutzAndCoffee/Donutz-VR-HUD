using System.Runtime.InteropServices;
using System.Text;

namespace Donutz_VR_HUD.Capture
{
    /// <summary>
    /// Represents a top-level, visible, capturable window on the desktop.
    /// </summary>
    public sealed record CapturableWindow(IntPtr Handle, string Title);

    /// <summary>
    /// Enumerates top-level windows on the desktop that can be used as a
    /// source for <see cref="WindowCapture"/> (e.g. a SimHub dashboard window).
    /// </summary>
    public static class WindowEnumerator
    {
        private delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);

        [DllImport("user32.dll")]
        private static extern bool EnumWindows(EnumWindowsProc lpEnumFunc, IntPtr lParam);

        [DllImport("user32.dll")]
        private static extern bool IsWindowVisible(IntPtr hWnd);

        [DllImport("user32.dll")]
        private static extern int GetWindowTextLength(IntPtr hWnd);

        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        private static extern int GetWindowText(IntPtr hWnd, StringBuilder lpString, int nMaxCount);

        [DllImport("user32.dll")]
        private static extern IntPtr GetShellWindow();

        /// <summary>
        /// Returns the list of currently visible, titled top-level windows.
        /// </summary>
        public static IReadOnlyList<CapturableWindow> GetCapturableWindows()
        {
            var result = new List<CapturableWindow>();
            var shellWindow = GetShellWindow();

            EnumWindows((hWnd, _) =>
            {
                if (hWnd == shellWindow || !IsWindowVisible(hWnd))
                {
                    return true;
                }

                var length = GetWindowTextLength(hWnd);
                if (length == 0)
                {
                    return true;
                }

                var builder = new StringBuilder(length + 1);
                GetWindowText(hWnd, builder, builder.Capacity);
                var title = builder.ToString();

                if (!string.IsNullOrWhiteSpace(title))
                {
                    result.Add(new CapturableWindow(hWnd, title));
                }

                return true;
            }, IntPtr.Zero);

            return result;
        }
    }
}
