using System.Diagnostics;

namespace Donutz_VR_HUD.Services
{
    /// <summary>
    /// Polls the OS process list to determine which (if any) of a set of
    /// "known" simulation process names (as configured on the user's
    /// profiles) is currently running, so profiles can be auto-loaded per
    /// simulation without hardcoding a fixed list of supported titles.
    /// </summary>
    public static class ActiveGameDetector
    {
        /// <summary>
        /// Returns the process name (without ".exe", case-insensitive
        /// match) of the first currently running process whose name is
        /// contained in <paramref name="candidateProcessNames"/>, or null
        /// if none of them are currently running.
        /// </summary>
        public static string? GetRunningProcessName(IEnumerable<string> candidateProcessNames)
        {
            var candidates = new HashSet<string>(candidateProcessNames, StringComparer.OrdinalIgnoreCase);
            if (candidates.Count == 0)
            {
                return null;
            }

            Process[] processes;
            try
            {
                processes = Process.GetProcesses();
            }
            catch
            {
                return null;
            }

            try
            {
                foreach (var process in processes)
                {
                    try
                    {
                        if (candidates.Contains(process.ProcessName))
                        {
                            return process.ProcessName;
                        }
                    }
                    catch
                    {
                        // Process may have exited or access may be denied; skip it.
                    }
                }
            }
            finally
            {
                foreach (var process in processes)
                {
                    process.Dispose();
                }
            }

            return null;
        }
    }
}
