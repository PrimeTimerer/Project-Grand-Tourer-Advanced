// Taken from the Sakatsuku PC launcher by tools\take_frame.py; change it there (or here, then keep the two alike).
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;
using Microsoft.Win32;

namespace GT6Launcher
{
    // A settings file edited in place: comments, order and every other setting stay as they are.
    public class IniFile
    {
        public static readonly Encoding FileEncoding = new UTF8Encoding(false);
        readonly string path;
        public readonly List<string> Lines = new List<string>();
        string newline = "\r\n";

        public IniFile(string path)
        {
            this.path = path;
            if (!File.Exists(path))
                return;
            string text = File.ReadAllText(path, FileEncoding);
            newline = text.Contains("\r\n") || !text.Contains("\n") ? "\r\n" : "\n";
            Lines.AddRange(text.Split('\n').Select(line => line.TrimEnd('\r')));
            if (Lines.Count > 0 && Lines[Lines.Count - 1].Length == 0)
                Lines.RemoveAt(Lines.Count - 1); // the final newline
        }

        public static string SectionName(string line)
        {
            string t = line.Trim();
            return t.StartsWith("[") && t.EndsWith("]") ? t.Substring(1, t.Length - 2).Trim() : null;
        }

        // [start, end) of a section's lines after its header; start = -1 when there is no such section.
        public void SectionRange(string section, out int start, out int end)
        {
            start = end = -1;
            for (int i = 0; i < Lines.Count; i++)
            {
                string name = SectionName(Lines[i]);
                if (name == null)
                    continue;
                if (start >= 0)
                {
                    end = i;
                    return;
                }
                if (string.Equals(name, section, StringComparison.OrdinalIgnoreCase))
                    start = i + 1;
            }
            if (start >= 0)
                end = Lines.Count;
        }

        public static bool KeyValue(string line, out string key, out string value)
        {
            key = value = null;
            string t = line.Trim();
            int equals = t.IndexOf('=');
            if (t.Length == 0 || t.StartsWith("#") || t.StartsWith(";") || equals <= 0)
                return false;
            key = t.Substring(0, equals).Trim();
            value = t.Substring(equals + 1).Trim();
            return true;
        }

        int Find(string section, string key, out int insertAt)
        {
            int start, end;
            SectionRange(section, out start, out end);
            insertAt = start < 0 ? -1 : start;
            for (int i = start; i >= 0 && i < end; i++)
            {
                string k, v;
                if (!KeyValue(Lines[i], out k, out v))
                    continue;
                insertAt = i + 1;
                if (string.Equals(k, key, StringComparison.OrdinalIgnoreCase))
                    return i;
            }
            return -1;
        }

        public string Get(string section, string key, string fallback)
        {
            int insertAt;
            int i = Find(section, key, out insertAt);
            string k, v;
            return i >= 0 && KeyValue(Lines[i], out k, out v) ? v : fallback;
        }

        public void Set(string section, string key, string value)
        {
            int insertAt;
            int i = Find(section, key, out insertAt);
            if (i >= 0)
            {
                int equals = Lines[i].IndexOf('=');
                if (Lines[i].Substring(equals + 1).Trim() != value)
                    Lines[i] = Lines[i].Substring(0, equals + 1) + (Lines[i].Substring(equals - 1, 1) == " " ? " " : "") + value;
                return;
            }
            if (insertAt < 0)
            {
                if (Lines.Count > 0 && Lines[Lines.Count - 1].Trim().Length > 0)
                    Lines.Add("");
                Lines.Add("[" + section + "]");
                insertAt = Lines.Count;
            }
            Lines.Insert(insertAt, key + "=" + value);
        }

        public void Save()
        {
            File.WriteAllText(path, string.Join(newline, Lines) + newline, FileEncoding);
        }
    }

    // PARAM.SFO: the PS3's key/value table (titles, versions) of the disc, the patch and every save.
    public static class Sfo
    {
        public static Dictionary<string, string> Read(string path)
        {
            try
            {
                return Parse(File.ReadAllBytes(path));
            }
            catch (Exception)
            {
                return new Dictionary<string, string>();
            }
        }

        // The same from the file's bytes (a PARAM.SFO read out of a disc image).
        public static Dictionary<string, string> Parse(byte[] b)
        {
            var map = new Dictionary<string, string>();
            try
            {
                if (b == null || b.Length < 20 || b[0] != 0 || b[1] != 'P' || b[2] != 'S' || b[3] != 'F')
                    return map;
                int keys = BitConverter.ToInt32(b, 8), data = BitConverter.ToInt32(b, 12), count = BitConverter.ToInt32(b, 16);
                for (int i = 0; i < count && 20 + 16 * i + 16 <= b.Length; i++)
                {
                    int entry = 20 + 16 * i;
                    int keyAt = keys + BitConverter.ToUInt16(b, entry), format = BitConverter.ToUInt16(b, entry + 2);
                    int length = BitConverter.ToInt32(b, entry + 4), at = data + BitConverter.ToInt32(b, entry + 12);
                    int end = keyAt;
                    while (end < b.Length && b[end] != 0)
                        end++;
                    string key = Encoding.ASCII.GetString(b, keyAt, end - keyAt);
                    if (at < 0 || at + length > b.Length)
                        continue;
                    map[key] = format == 0x0404 ? BitConverter.ToUInt32(b, at).ToString() : Encoding.UTF8.GetString(b, at, length).TrimEnd('\0');
                }
            }
            catch (Exception)
            {
            }
            return map;
        }

        public static string Get(Dictionary<string, string> map, string key)
        {
            string value;
            return map.TryGetValue(key, out value) ? value : "";
        }
    }

    // What the PC has (registry and Win32 only: quick, nothing installed).
    public class PcInfo
    {
        public string Cpu = L.T("알 수 없음");
        public int Threads = Environment.ProcessorCount;
        public long Ram;
        public string Gpu = L.T("알 수 없음");
        public long Vram;
        public int ScreenWidth, ScreenHeight;

        [StructLayout(LayoutKind.Sequential)]
        class MemoryStatus
        {
            public uint Length = 64;
            public uint Load;
            public ulong TotalPhys, AvailPhys, TotalPageFile, AvailPageFile, TotalVirtual, AvailVirtual, AvailExtendedVirtual;
        }

        [DllImport("kernel32.dll")]
        static extern bool GlobalMemoryStatusEx([In, Out] MemoryStatus status);

        [DllImport("user32.dll")]
        static extern int GetSystemMetrics(int index);

        public static PcInfo Read()
        {
            var pc = new PcInfo();
            try
            {
                using (RegistryKey cpu = Registry.LocalMachine.OpenSubKey(@"HARDWARE\DESCRIPTION\System\CentralProcessor\0"))
                    if (cpu != null && cpu.GetValue("ProcessorNameString") != null)
                        pc.Cpu = Regex.Replace(cpu.GetValue("ProcessorNameString").ToString(), @"\s+", " ").Trim();
            }
            catch (Exception)
            {
            }
            var memory = new MemoryStatus();
            if (GlobalMemoryStatusEx(memory))
                pc.Ram = (long)memory.TotalPhys;
            pc.ScreenWidth = GetSystemMetrics(0);
            pc.ScreenHeight = GetSystemMetrics(1);
            // Display adapters: the one with the most video memory (the dedicated card on laptops with two).
            try
            {
                using (RegistryKey adapters = Registry.LocalMachine.OpenSubKey(@"SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}"))
                {
                    foreach (string name in adapters.GetSubKeyNames())
                    {
                        try
                        {
                            using (RegistryKey adapter = adapters.OpenSubKey(name))
                            {
                                object desc = adapter == null ? null : adapter.GetValue("DriverDesc");
                                if (desc == null || desc.ToString().Contains("Basic") || desc.ToString().Contains("Remote"))
                                    continue;
                                long vram = 0;
                                object q = adapter.GetValue("HardwareInformation.qwMemorySize");
                                object d = adapter.GetValue("HardwareInformation.MemorySize");
                                if (q is long)
                                    vram = (long)q;
                                else if (d is int)
                                    vram = (uint)(int)d;
                                else if (d is byte[] && ((byte[])d).Length >= 4)
                                    vram = BitConverter.ToUInt32((byte[])d, 0);
                                if (vram > pc.Vram || pc.Gpu == L.T("알 수 없음"))
                                {
                                    pc.Gpu = desc.ToString();
                                    pc.Vram = vram;
                                }
                            }
                        }
                        catch (Exception)
                        {
                        }
                    }
                }
            }
            catch (Exception)
            {
            }
            return pc;
        }

        public static string Size(long bytes)
        {
            if (bytes <= 0)
                return "?";
            if (bytes < 1L << 20)
                return Math.Max(1, bytes >> 10) + " KB";
            return bytes >= 1L << 30 ? (bytes / (double)(1L << 30)).ToString("0.#") + " GB" : (bytes >> 20) + " MB";
        }
    }
}
