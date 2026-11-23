// Mods: folders under mods\ whose files stand in for the game's own files under USRDIR (a PDIPFS mod is
// mods\<name>\PDIPFS\...). Nothing is copied into the game data: the engine reads PS3_USRDIR_OVERLAY=<folder> before
// the original files, and at the start of a run the launcher makes that folder (mods\.active) out of the mods that
// are switched on, in list order, with hard links (no extra disk space; a copy where links cannot be made). Switching
// a mod off or removing it is therefore complete at once: the game data was never changed.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.IO.Compression;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;
using System.Web.Script.Serialization;

namespace GT6Launcher
{
    public class ModInfo
    {
        public string Name, Dir, Root;   // Root: the folder that stands for USRDIR; null when none was found
        public int Files;
        public long Size;
        public DateTime Changed;
        public bool Enabled;
    }

    public static class ModStore
    {
        const string Active = ".active";

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        static extern bool CreateHardLink(string newFile, string existingFile, IntPtr security);

        static string[] Names(Launcher launcher, string key)
        {
            return launcher.Value("Mods", key, "").Split(new[] { '|' }, StringSplitOptions.RemoveEmptyEntries);
        }

        public static List<ModInfo> List(Launcher launcher)
        {
            var mods = new List<ModInfo>();
            if (!Directory.Exists(launcher.ModsDir))
                return mods;
            string[] order = Names(launcher, "Order"), enabled = Names(launcher, "Enabled");
            foreach (string dir in Directory.GetDirectories(launcher.ModsDir))
            {
                string name = Path.GetFileName(dir);
                if (name.StartsWith("."))
                    continue;
                var mod = new ModInfo { Name = name, Dir = dir, Root = ContentRoot(dir), Enabled = enabled.Contains(name, StringComparer.OrdinalIgnoreCase) };
                if (mod.Root != null)
                {
                    foreach (string file in Files(mod.Root))
                    {
                        var info = new FileInfo(file);
                        mod.Files++;
                        mod.Size += info.Length;
                        if (info.LastWriteTimeUtc > mod.Changed)
                            mod.Changed = info.LastWriteTimeUtc;
                    }
                }
                mods.Add(mod);
            }
            Func<ModInfo, int> rank = m =>
            {
                int i = Array.FindIndex(order, o => o.Equals(m.Name, StringComparison.OrdinalIgnoreCase));
                return i < 0 ? int.MaxValue : i;
            };
            return mods.OrderBy(rank).ThenBy(m => m.Name, StringComparer.OrdinalIgnoreCase).ToList();
        }

        // The folder of a mod whose content stands for USRDIR: <mod>\USRDIR, or the folder that holds PDIPFS; a zip
        // often has one more folder around it, so one lone subfolder is followed (twice at most).
        internal static string ContentRoot(string dir)
        {
            for (int depth = 0; depth < 3; depth++)
            {
                if (Directory.Exists(Path.Combine(dir, "USRDIR")))
                    return Path.Combine(dir, "USRDIR");
                if (Directory.Exists(Path.Combine(dir, "PDIPFS")))
                    return dir;
                string[] subs = Directory.GetDirectories(dir);
                if (subs.Length != 1)
                    break;
                dir = subs[0];
            }
            return null;
        }

        // The mod's files. Where the mod's own folder stands for USRDIR, notes at its top (.source.json, readme.txt) are
        // not game files: only what sits in a folder counts there.
        internal static IEnumerable<string> Files(string root)
        {
            if (Path.GetFileName(root).Equals("USRDIR", StringComparison.OrdinalIgnoreCase))
                return Directory.EnumerateFiles(root, "*", SearchOption.AllDirectories);
            return Directory.GetDirectories(root).SelectMany(d => Directory.EnumerateFiles(d, "*", SearchOption.AllDirectories));
        }

        static void Save(Launcher launcher, IEnumerable<ModInfo> mods)
        {
            var ini = new IniFile(launcher.StatePath);
            ini.Set("Mods", "Order", string.Join("|", mods.Select(m => m.Name)));
            ini.Set("Mods", "Enabled", string.Join("|", mods.Where(m => m.Enabled).Select(m => m.Name)));
            ini.Save();
        }

        public static void SetEnabled(Launcher launcher, string name, bool on)
        {
            List<ModInfo> mods = List(launcher);
            foreach (ModInfo mod in mods.Where(m => m.Name == name))
                mod.Enabled = on;
            Save(launcher, mods);
        }

        public static void Move(Launcher launcher, string name, int by)
        {
            List<ModInfo> mods = List(launcher);
            int at = mods.FindIndex(m => m.Name == name), to = at + by;
            if (at < 0 || to < 0 || to >= mods.Count)
                return;
            ModInfo mod = mods[at];
            mods.RemoveAt(at);
            mods.Insert(to, mod);
            Save(launcher, mods);
        }

        internal static string CleanName(string name)
        {
            foreach (char c in Path.GetInvalidFileNameChars())
                name = name.Replace(c, '_');
            name = name.Trim().TrimStart('.');
            return name.Length > 0 ? name : "mod";
        }

        // Adds a mod from a folder or a zip: copied (unpacked) into mods\<its name>, put last in the order and switched on.
        public static ModInfo Add(Launcher launcher, string source)
        {
            bool zip = File.Exists(source) && source.EndsWith(".zip", StringComparison.OrdinalIgnoreCase);
            if (!zip && !Directory.Exists(source))
                throw new InvalidOperationException(L.T("폴더나 zip 파일만 추가할 수 있습니다."));
            string name = CleanName(zip ? Path.GetFileNameWithoutExtension(source) : Path.GetFileName(source.TrimEnd('\\', '/')));
            string target = Path.Combine(launcher.ModsDir, name);
            if (Directory.Exists(target))
                throw new InvalidOperationException(L.T("같은 이름의 모드가 이미 있습니다: ") + name + L.T(" (먼저 제거하거나 이름을 바꿔 주세요.)"));
            if (!zip && Path.GetFullPath(source).TrimEnd('\\').StartsWith(Path.GetFullPath(launcher.ModsDir).TrimEnd('\\') + "\\", StringComparison.OrdinalIgnoreCase))
                throw new InvalidOperationException(L.T("이미 mods 폴더 안에 있는 폴더입니다."));
            Directory.CreateDirectory(launcher.ModsDir);
            try
            {
                if (zip)
                    Zip.Extract(source, target);
                else
                    CopyTree(source, target);
                if (ContentRoot(target) == null)
                    throw new InvalidDataException(L.T("이 안에서 PDIPFS(또는 USRDIR) 폴더를 찾지 못했습니다. GT6의 파일을 바꾸는 모드가 아닙니다."));
            }
            catch (Exception)
            {
                DeleteTree(target);
                throw;
            }
            List<ModInfo> mods = List(launcher);
            ModInfo added = mods.First(m => m.Name.Equals(name, StringComparison.OrdinalIgnoreCase));
            mods.Remove(added);
            mods.Add(added);
            added.Enabled = true;
            Save(launcher, mods);
            return added;
        }

        public static void Remove(Launcher launcher, string name)
        {
            if (name != Path.GetFileName(name) || name.StartsWith(".") || name.Length == 0)
                throw new InvalidOperationException(L.T("모드 이름이 올바르지 않습니다."));
            string dir = Path.Combine(launcher.ModsDir, name);
            if (Directory.Exists(dir))
                DeleteTree(dir);
            Save(launcher, List(launcher));
        }

        static void CopyTree(string from, string to)
        {
            Directory.CreateDirectory(to);
            foreach (string dir in Directory.GetDirectories(from, "*", SearchOption.AllDirectories))
                Directory.CreateDirectory(Path.Combine(to, dir.Substring(from.TrimEnd('\\').Length + 1)));
            foreach (string file in Directory.GetFiles(from, "*", SearchOption.AllDirectories))
                File.Copy(file, Path.Combine(to, file.Substring(from.TrimEnd('\\').Length + 1)), false);
        }

        // Removes a folder inside mods\ and nothing else (a read-only file is made writable first).
        internal static void DeleteTree(string dir)
        {
            if (!Directory.Exists(dir))
                return;
            string parent = Path.GetDirectoryName(Path.GetFullPath(dir).TrimEnd('\\'));
            if (!string.Equals(Path.GetFileName(parent), "mods", StringComparison.OrdinalIgnoreCase))
                throw new InvalidOperationException(L.T("mods 폴더 밖은 지우지 않습니다: ") + dir);
            foreach (string file in Directory.GetFiles(dir, "*", SearchOption.AllDirectories))
            {
                FileAttributes attributes = File.GetAttributes(file);
                if ((attributes & FileAttributes.ReadOnly) != 0)
                    File.SetAttributes(file, attributes & ~FileAttributes.ReadOnly);
            }
            Directory.Delete(dir, true);
        }

        // The start of a run: null when no mod is on, else the folder the game reads first (mods\.active), made again
        // only when the mods that are on, their order or their files changed.
        public static string Prepare(Launcher launcher)
        {
            List<ModInfo> mods = List(launcher).Where(m => m.Enabled && m.Root != null && m.Files > 0).ToList();
            if (mods.Count == 0)
                return null;
            string active = Path.Combine(launcher.ModsDir, Active), marker = Path.Combine(active, ".signature");
            string signature = string.Join("\n", mods.Select(m => m.Name + "|" + m.Files + "|" + m.Size + "|" + m.Changed.Ticks));
            if (File.Exists(marker) && File.ReadAllText(marker) == signature)
                return active;
            DeleteTree(active);
            Directory.CreateDirectory(active);
            foreach (ModInfo mod in mods)   // a later mod's file replaces an earlier mod's
            {
                foreach (string file in Files(mod.Root))
                {
                    string target = Path.Combine(active, file.Substring(mod.Root.Length + 1));
                    Directory.CreateDirectory(Path.GetDirectoryName(target));
                    if (File.Exists(target))
                        File.Delete(target);
                    if (!CreateHardLink(target, file, IntPtr.Zero))
                        File.Copy(file, target);
                }
            }
            File.WriteAllText(marker, signature);
            return active;
        }

        // How many files the mods that are on replace in all (a file two mods hold counts once).
        public static int ActiveFiles(IEnumerable<ModInfo> mods)
        {
            var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            foreach (ModInfo mod in mods.Where(m => m.Enabled && m.Root != null))
                foreach (string file in Files(mod.Root))
                    seen.Add(file.Substring(mod.Root.Length + 1));
            return seen.Count;
        }
    }

    // The GT6 Career Mod (by Lunard) with the Spec II mod it is built on, as its archive comes from GTPlanet: one zip with
    // a folder that holds "Spec 2 Mod PDIPFS (INSTALL THIS FIRST).zip", "GT6 Career mod PDIPFS.zip" and the original
    // files for undoing it by hand. Both become mods of this launcher (mods\, read before the game's own files; the game
    // data is never changed, so switching them off is the undoing), Spec II first and the Career mod over it. The mod's
    // own instructions: only after the game has run once with 1.22 to its main screen and made its save. [GT6 0089]
    public static class CareerMod
    {
        public const string PageUrl = "https://www.gtplanet.net/forum/downloads/gran-turismo-6-career-mod.7049/";
        public const string SpecName = "Spec II mod", CareerName = "GT6 Career mod";

        // The game has run to its main screen once (its save is there) and its installation is complete.
        public static bool FirstRunDone(Launcher launcher)
        {
            return Directory.Exists(Path.Combine(launcher.SavesDir, "BCJS37016-GAME6")) && !SetupRunner.Needed(launcher);
        }

        public static string Version(Launcher launcher)
        {
            return launcher.Value("CareerMod", "Version", "");
        }

        public static bool Installed(Launcher launcher)
        {
            return Directory.Exists(Path.Combine(launcher.ModsDir, SpecName)) && Directory.Exists(Path.Combine(launcher.ModsDir, CareerName));
        }

        public static bool On(Launcher launcher)
        {
            List<ModInfo> mods = ModStore.List(launcher);
            return mods.Any(m => m.Name == SpecName && m.Enabled) && mods.Any(m => m.Name == CareerName && m.Enabled);
        }

        public static void Switch(Launcher launcher, bool on)
        {
            ModStore.SetEnabled(launcher, SpecName, on);
            ModStore.SetEnabled(launcher, CareerName, on);
        }

        // --import-career <archive>: the same without a window; the result goes to logs\launcher\runs.log.
        // Exit code 0 = imported, 1 = the game has not run to its main screen yet, 2 = failed.
        public static int ImportHeadless(Launcher launcher, string archive)
        {
            string stamp = DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss") + " career mod import: ";
            if (!FirstRunDone(launcher))
            {
                Logs.Record(launcher, stamp + "refused, the game has not run to its main screen yet (no save)\r\n");
                return 1;
            }
            try
            {
                string version = Import(launcher, archive);
                List<ModInfo> mods = ModStore.List(launcher);
                Logs.Record(launcher, stamp + "ver. " + version + "; mods in order: " + string.Join(", ", mods.Select(m => m.Name + (m.Enabled ? " (on, " : " (off, ") + m.Files + " files)")) + "\r\n");
                return 0;
            }
            catch (Exception error)
            {
                Logs.Record(launcher, stamp + "failed - " + error.Message + "\r\n");
                return 2;
            }
        }

        // The archive (or the folder it was unpacked to): its two mod zips become mods\Spec II mod and mods\GT6 Career mod
        // (an earlier import of them is replaced), switched on, in that order. Returns the version in the archive's name.
        public static string Import(Launcher launcher, string archive)
        {
            string temp = Path.Combine(launcher.ModsDir, ".import");
            ModStore.DeleteTree(temp);
            Directory.CreateDirectory(temp);
            try
            {
                string spec = null, career = null;
                Func<string, int> kind = delegate(string name)   // 1 = Spec II, 2 = Career, 0 = neither (the originals too)
                {
                    if (!name.EndsWith(".zip", StringComparison.OrdinalIgnoreCase) || name.IndexOf("original", StringComparison.OrdinalIgnoreCase) >= 0)
                        return 0;
                    if (Regex.IsMatch(name, @"spec\s*(2|ii)\b", RegexOptions.IgnoreCase))
                        return 1;
                    return name.IndexOf("career", StringComparison.OrdinalIgnoreCase) >= 0 ? 2 : 0;
                };
                if (Directory.Exists(archive))
                {
                    foreach (string file in Directory.GetFiles(archive, "*.zip", SearchOption.AllDirectories))
                    {
                        int k = kind(Path.GetFileName(file));
                        if (k == 0)
                            continue;
                        string to = Path.Combine(temp, (k == 1 ? SpecName : CareerName) + ".zip");
                        File.Copy(file, to, true);
                        if (k == 1)
                            spec = to;
                        else
                            career = to;
                    }
                }
                else
                {
                    using (ZipArchive zip = ZipFile.OpenRead(archive))
                        foreach (ZipArchiveEntry entry in zip.Entries)
                        {
                            int k = kind(entry.Name);
                            if (k == 0)
                                continue;
                            string to = Path.Combine(temp, (k == 1 ? SpecName : CareerName) + ".zip");
                            entry.ExtractToFile(to, true);
                            if (k == 1)
                                spec = to;
                            else
                                career = to;
                        }
                }
                if (spec == null || career == null)
                    throw new InvalidDataException(L.T("이 안에서 Spec II 모드와 커리어 모드의 zip을 찾지 못했습니다. GTPlanet에서 받은 \"GT6 Career mod ver. ….zip\"을 그대로 골라 주세요."));
                foreach (string name in new[] { SpecName, CareerName })
                    if (Directory.Exists(Path.Combine(launcher.ModsDir, name)))
                        ModStore.Remove(launcher, name);
                ModStore.Add(launcher, spec);     // last in the list, switched on
                ModStore.Add(launcher, career);   // after it: where both have a file, the Career mod's is read
                Match version = Regex.Match(Path.GetFileName(archive.TrimEnd('\\')), @"ver\.?\s*([0-9][0-9.]*[0-9]|[0-9])", RegexOptions.IgnoreCase);
                string text = version.Success ? version.Groups[1].Value : "";
                launcher.Set("CareerMod", "Version", text);
                launcher.Set("CareerMod", "Archive", Path.GetFileName(archive.TrimEnd('\\')));
                return text;
            }
            finally
            {
                ModStore.DeleteTree(temp);
            }
        }
    }

    // A mod that is inside the game data itself (another launcher, or a copy by hand, put it there): the game reads it
    // on every run, and this launcher neither switches it off nor removes it. The launcher keeps a record of each one,
    // mods\.game\<id>.txt: a header and the mod's files that are in force (size, path under USRDIR). A record is made
    // only by comparing the mod's own files with the game data byte for byte (GameMods.Check); each time the list is
    // shown, the recorded files are looked up again in the game data (there, same size).
    public class GameMod
    {
        public string Id, Name, Source, From, Installed, Checked, RecordPath;
        public int Order, Added = -1, Replaced = -1, Superseded, Files, Present = -1;   // Present: -1 = not looked up
        public long Size;
        public bool Complete { get { return Files > 0 && Present == Files; } }
    }

    public class GameModCheck
    {
        public readonly List<string> Recorded = new List<string>(), Skipped = new List<string>();
    }

    public static class GameMods
    {
        const string Folder = ".game";

        // What the RPCS3 GT6-Launcher calls the mods it installs (its record holds only these short names).
        static readonly Dictionary<string, string> otherNames = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase)
        {
            { "spec2", "Spec II Mod (Beta 7)" }, { "career", "GT6 Career Mod" },
        };

        class Candidate
        {
            public string Id, Name, Root, Source, From, Installed;
            public int Added = -1, Replaced = -1;
            public List<string> Files;   // paths under Root (Root stands for USRDIR)
        }

        static readonly object gate = new object();
        static string cacheKey;
        static DateTime cacheAt;
        static List<GameMod> cache;

        static string Dir(Launcher launcher)
        {
            return Path.Combine(launcher.ModsDir, Folder);
        }

        static string UsrDir(Launcher launcher)
        {
            return Path.Combine(launcher.TitleDir, "USRDIR");
        }

        // files: null = the header alone
        static GameMod Read(string path, List<KeyValuePair<string, long>> files)
        {
            var mod = new GameMod { RecordPath = path, Id = Path.GetFileNameWithoutExtension(path) };
            mod.Name = mod.Id;
            bool list = false;
            int count = 0;
            long size = 0;
            foreach (string line in File.ReadLines(path, Encoding.UTF8))
            {
                if (list)
                {
                    int tab = line.IndexOf('\t');
                    long length;
                    if (tab <= 0 || !long.TryParse(line.Substring(0, tab), out length))
                        continue;
                    string relative = line.Substring(tab + 1);
                    if (relative.Length == 0 || relative.Contains("..") || relative.Contains(":") || relative.StartsWith("\\"))
                        continue;
                    count++;
                    size += length;
                    files.Add(new KeyValuePair<string, long>(relative, length));
                    continue;
                }
                if (line == "[files]")
                {
                    if (files == null)
                        break;
                    list = true;
                    continue;
                }
                int eq = line.IndexOf('=');
                if (eq <= 0 || line.StartsWith("#"))
                    continue;
                string key = line.Substring(0, eq), value = line.Substring(eq + 1);
                int number;
                int.TryParse(value, out number);
                switch (key)
                {
                    case "name": mod.Name = value; break;
                    case "source": mod.Source = value; break;
                    case "from": mod.From = value; break;
                    case "installed": mod.Installed = value; break;
                    case "checked": mod.Checked = value; break;
                    case "order": mod.Order = number; break;
                    case "added": mod.Added = number; break;
                    case "replaced": mod.Replaced = number; break;
                    case "superseded": mod.Superseded = number; break;
                    case "files": mod.Files = number; break;
                    case "bytes": long.TryParse(value, out mod.Size); break;
                }
            }
            if (list)
            {
                mod.Files = count;
                mod.Size = size;
            }
            return mod;
        }

        static void Write(string path, GameMod mod, List<KeyValuePair<string, long>> files)
        {
            var text = new StringBuilder();
            text.Append(L.T("# GT6 Launcher: 게임 데이터 안에 들어 있는 모드의 기록입니다. 모드의 파일을 게임 데이터와 바이트 단위로 비교해 만들었고,\n"));
            text.Append(L.T("# 아래 목록(크기, USRDIR 아래 경로)은 런처가 모드 화면을 보일 때마다 다시 확인합니다. 이 파일을 지워도 게임 데이터는 그대로입니다.\n"));
            text.Append("name=" + mod.Name + "\n");
            if (!string.IsNullOrEmpty(mod.Source))
                text.Append("source=" + mod.Source + "\n");
            if (!string.IsNullOrEmpty(mod.From))
                text.Append("from=" + mod.From + "\n");
            if (!string.IsNullOrEmpty(mod.Installed))
                text.Append("installed=" + mod.Installed + "\n");
            text.Append("checked=" + DateTime.Now.ToString("yyyy-MM-dd HH:mm") + "\n");
            text.Append("order=" + mod.Order + "\n");
            if (mod.Added >= 0)
                text.Append("added=" + mod.Added + "\n");
            if (mod.Replaced >= 0)
                text.Append("replaced=" + mod.Replaced + "\n");
            text.Append("superseded=" + mod.Superseded + "\n");
            text.Append("files=" + files.Count + "\n");
            text.Append("bytes=" + files.Sum(f => f.Value) + "\n");
            text.Append("[files]\n");
            foreach (KeyValuePair<string, long> file in files)
                text.Append(file.Value).Append('\t').Append(file.Key).Append('\n');
            Directory.CreateDirectory(Path.GetDirectoryName(path));
            File.WriteAllText(path, text.ToString(), new UTF8Encoding(false));
        }

        // The mods the records name, in the order they were put into the game data. verify: every recorded file is also
        // looked up in the game data (some thousand lookups; the answer is kept for a few seconds).
        public static List<GameMod> List(Launcher launcher, bool verify)
        {
            var mods = new List<GameMod>();
            string dir = Dir(launcher);
            if (!Directory.Exists(dir))
                return mods;
            string[] records = Directory.GetFiles(dir, "*.txt");
            string key = null;
            if (verify)
            {
                key = dir + "|" + string.Join("|", records.Select(r => r + ":" + File.GetLastWriteTimeUtc(r).Ticks));
                lock (gate)
                    if (cache != null && cacheKey == key && (DateTime.UtcNow - cacheAt).TotalSeconds < 20)
                        return cache;
            }
            string usrdir = UsrDir(launcher);
            foreach (string record in records)
            {
                try
                {
                    List<KeyValuePair<string, long>> files = verify ? new List<KeyValuePair<string, long>>() : null;
                    GameMod mod = Read(record, files);
                    if (verify)
                    {
                        mod.Present = 0;
                        foreach (KeyValuePair<string, long> file in files)
                        {
                            var info = new FileInfo(Path.Combine(usrdir, file.Key));
                            if (info.Exists && info.Length == file.Value)
                                mod.Present++;
                        }
                    }
                    mods.Add(mod);
                }
                catch (Exception)
                {
                    // a record that cannot be read is not shown
                }
            }
            mods = mods.OrderBy(m => m.Order).ThenBy(m => m.Name, StringComparer.OrdinalIgnoreCase).ToList();
            if (verify)
                lock (gate)
                {
                    cache = mods;
                    cacheKey = key;
                    cacheAt = DateTime.UtcNow;
                }
            return mods;
        }

        // Removes a record (the game data is not touched).
        public static void Forget(Launcher launcher, GameMod mod)
        {
            string dir = Path.GetFullPath(Dir(launcher)).TrimEnd('\\') + "\\", path = Path.GetFullPath(mod.RecordPath);
            if (!path.StartsWith(dir, StringComparison.OrdinalIgnoreCase) || !path.EndsWith(".txt", StringComparison.OrdinalIgnoreCase))
                throw new InvalidOperationException(L.T("모드 기록 파일이 아닙니다: ") + path);
            File.Delete(path);
            lock (gate)
                cache = null;
        }

        static int Fill(Stream stream, byte[] buffer)
        {
            int filled = 0;
            while (filled < buffer.Length)
            {
                int read = stream.Read(buffer, filled, buffer.Length - filled);
                if (read <= 0)
                    break;
                filled += read;
            }
            return filled;
        }

        static bool SameBytes(string a, string b, byte[] bufferA, byte[] bufferB)
        {
            var infoA = new FileInfo(a);
            var infoB = new FileInfo(b);
            if (!infoB.Exists || infoA.Length != infoB.Length)
                return false;
            using (var fileA = new FileStream(a, FileMode.Open, FileAccess.Read, FileShare.Read, 4096, FileOptions.SequentialScan))
            using (var fileB = new FileStream(b, FileMode.Open, FileAccess.Read, FileShare.ReadWrite, 4096, FileOptions.SequentialScan))
            {
                for (;;)
                {
                    int readA = Fill(fileA, bufferA), readB = Fill(fileB, bufferB);
                    if (readA != readB)
                        return false;
                    if (readA == 0)
                        return true;
                    for (int i = 0; i < readA; i++)
                        if (bufferA[i] != bufferB[i])
                            return false;
                }
            }
        }

        static int Count(Dictionary<string, object> entry, string key)
        {
            object value;
            var list = entry.TryGetValue(key, out value) ? value as System.Collections.ICollection : null;
            return list == null ? -1 : list.Count;
        }

        // The RPCS3 GT6-Launcher's own record of what it installed into its game data, in order: data\backups\<title>\
        // stack.json ([{id, added[], replaced[]}]); the mods' files are kept in data\mods\<id>.
        static void Other(string source, string stack, List<Candidate> candidates, GameModCheck result)
        {
            var json = new JavaScriptSerializer { MaxJsonLength = 64 * 1024 * 1024 };
            List<Dictionary<string, object>> entries = json.Deserialize<List<Dictionary<string, object>>>(File.ReadAllText(stack, Encoding.UTF8));
            string when = File.GetLastWriteTime(stack).ToString("yyyy-MM-dd");
            foreach (Dictionary<string, object> entry in entries ?? new List<Dictionary<string, object>>())
            {
                object value;
                string id = entry.TryGetValue("id", out value) ? value as string : null;
                if (string.IsNullOrEmpty(id) || id != Path.GetFileName(id) || id.StartsWith("."))
                    continue;
                string name = otherNames.ContainsKey(id) ? otherNames[id] : id;
                string dir = Path.Combine(source, "data", "mods", id), root = Directory.Exists(dir) ? ModStore.ContentRoot(dir) : null;
                if (root == null)
                {
                    result.Skipped.Add(name + L.T(": 그 런처의 data\\mods\\") + id + L.T("에 모드 파일이 남아 있지 않아 비교하지 못했습니다"));
                    continue;
                }
                var candidate = new Candidate
                {
                    Id = ModStore.CleanName(id), Name = name, Root = root, From = L.T("GT6-Launcher(RPCS3)의 설치 기록"), Installed = when,
                    Added = Count(entry, "added"), Replaced = Count(entry, "replaced"),
                };
                try
                {
                    Dictionary<string, object> note = json.Deserialize<Dictionary<string, object>>(File.ReadAllText(Path.Combine(dir, ".source.json"), Encoding.UTF8));
                    if (note != null && note.TryGetValue("source", out value) && value is string)
                        candidate.Source = Path.GetFileName(((string)value).Replace('/', '\\'));
                }
                catch (Exception)
                {
                    // no note about where the mod came from
                }
                candidates.Add(candidate);
            }
        }

        // Looks for mods inside the game data and records the ones that are wholly there. source: the folder of the RPCS3
        // GT6-Launcher that installed them (its record names them in order), or one mod's own folder (PDIPFS inside).
        // Nothing but mods\.game is written; the game data and the source are only read.
        public static GameModCheck Check(Launcher launcher, string source, Action<string> progress)
        {
            if (string.IsNullOrEmpty(source) || !Directory.Exists(source))
                throw new InvalidOperationException(L.T("폴더를 찾지 못했습니다: ") + source);
            string usrdir = UsrDir(launcher);
            if (!Directory.Exists(usrdir))
                throw new InvalidOperationException(L.T("게임 데이터를 찾지 못했습니다: ") + usrdir);
            var result = new GameModCheck();
            var candidates = new List<Candidate>();
            string stack = Path.Combine(source, "data", "backups", GameFiles.TitleId, "stack.json");
            if (File.Exists(stack))
                Other(source, stack, candidates, result);
            else
            {
                string root = ModStore.ContentRoot(source);
                if (root == null)
                    throw new InvalidOperationException(L.T("이 폴더에는 모드 설치 기록(data\\backups\\") + GameFiles.TitleId + L.T("\\stack.json)도, PDIPFS(또는 USRDIR) 폴더도 없습니다."));
                string name = Path.GetFileName(source.TrimEnd('\\', '/'));
                candidates.Add(new Candidate { Id = ModStore.CleanName(name), Name = name, Root = root, From = L.T("모드 폴더와 직접 비교") });
            }

            // a file that two of them hold belongs to the later one: it was installed over the earlier one's
            var owner = new Dictionary<string, Candidate>(StringComparer.OrdinalIgnoreCase);
            foreach (Candidate candidate in candidates)
            {
                candidate.Files = ModStore.Files(candidate.Root).Select(f => f.Substring(candidate.Root.Length + 1)).ToList();
                foreach (string file in candidate.Files)
                    owner[file] = candidate;
            }
            int order = List(launcher, false).Where(m => !candidates.Any(c => c.Id.Equals(m.Id, StringComparison.OrdinalIgnoreCase))).Select(m => m.Order).DefaultIfEmpty(0).Max();
            byte[] bufferA = new byte[1 << 20], bufferB = new byte[1 << 20];
            foreach (Candidate candidate in candidates)
            {
                var inForce = new List<KeyValuePair<string, long>>();
                int mine = 0, same = 0;
                foreach (string file in candidate.Files)
                {
                    if (owner[file] != candidate)
                        continue;
                    mine++;
                    string own = Path.Combine(candidate.Root, file);
                    if (SameBytes(own, Path.Combine(usrdir, file), bufferA, bufferB))
                    {
                        same++;
                        inForce.Add(new KeyValuePair<string, long>(file, new FileInfo(own).Length));
                    }
                    if (progress != null && mine % 500 == 0)
                        progress(candidate.Name + " (" + mine.ToString("N0") + L.T("개째)"));
                }
                if (mine == 0 || same != mine)
                {
                    result.Skipped.Add(candidate.Name + L.T(": 게임 데이터와 같은 파일이 ") + mine.ToString("N0") + L.T("개 중 ") + same.ToString("N0") + L.T("개뿐이라 기록하지 않았습니다"));
                    continue;
                }
                var mod = new GameMod
                {
                    Id = candidate.Id, Name = candidate.Name, Source = candidate.Source, From = candidate.From, Installed = candidate.Installed,
                    Order = ++order, Added = candidate.Added, Replaced = candidate.Replaced, Superseded = candidate.Files.Count - mine,
                };
                Write(Path.Combine(Dir(launcher), candidate.Id + ".txt"), mod, inForce);
                result.Recorded.Add(candidate.Name + L.T(" (파일 ") + same.ToString("N0") + L.T("개)"));
            }
            lock (gate)
                cache = null;
            return result;
        }

        // --check-game-mods <folder>: Check without a window; what it found goes to logs\launcher\runs.log.
        public static int CheckHeadless(Launcher launcher, string source)
        {
            string stamp = DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss") + L.T(" 게임 데이터의 모드 확인: ");
            try
            {
                GameModCheck check = Check(launcher, source, null);
                Logs.Record(launcher, stamp + (check.Recorded.Count == 0 ? L.T("기록한 모드 없음") : L.T("기록 ") + string.Join(", ", check.Recorded))
                                      + (check.Skipped.Count == 0 ? "" : " | " + string.Join(" | ", check.Skipped)) + "\r\n");
                return check.Recorded.Count > 0 ? 0 : 1;
            }
            catch (Exception error)
            {
                Logs.Record(launcher, stamp + L.T("실패 - ") + error.Message + "\r\n");
                return 2;
            }
        }
    }
}
