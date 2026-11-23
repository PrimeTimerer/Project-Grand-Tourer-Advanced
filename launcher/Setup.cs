// The setup of a new copy: everything between "the user has the game" (an ISO or a folder of the European disc,
// BCES01893) and "the game starts", done by the launcher with one button and nothing but a progress bar to see.
//   1  the disc's files                     -> game\dev_bdvd            (read out of the ISO, or copied from a folder)
//   2  RPCS3, as a tool                     -> _setup\rpcs3             (downloaded from its own releases, checked; unpacked by
//                                                                       Windows' tar or, where that cannot read 7z, by SevenZip.cs)
//   3  the PS3 firmware                     -> installed in that RPCS3  (downloaded from Sony's update server)
//   4  the game's 22 update packages        -> game\dev_hdd0            (downloaded from Sony's update server, installed
//                                                                       by RPCS3's own package installer)
//   5  the game's main executable           -> game\EMAIN.ELF           (decrypted by RPCS3; the licence key is the one
//                                                                       the game's own loader hands over when RPCS3
//                                                                       runs it for a moment - it is never stored)
//   6  the firmware's libsre                -> game\firmware            (decrypted by RPCS3, placed at its load address)
//   7  _setup is deleted: RPCS3, the firmware file and the packages are gone again
//   8  the game's own installation          (the game merges its updates on a first start; here it runs hidden)
// The launcher holds no Sony key and no part of the game. Every result is checked against the hash of the file the
// port was built from. RPCS3's installers are GUI programs: they run on a Windows desktop of their own (CreateDesktop)
// that is never shown, started through this same exe in agent mode (--setup-agent), which answers their dialogs
// through UI Automation. [GT6 0089]
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Net;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Windows.Automation;

namespace GT6Launcher
{
    public class UpdatePackage
    {
        public readonly string Version, Url, Sha1;   // Sha1: of everything before the package's last 0x20 bytes (its own footer, and Sony's list)
        public readonly long Size;

        public UpdatePackage(string version, string url, long size, string sha1)
        {
            Version = version;
            Url = url;
            Size = size;
            Sha1 = sha1;
        }

        public string FileName { get { return Url.Substring(Url.LastIndexOf('/') + 1); } }
    }

    // What the setup downloads and what its results must be. The hashes are those of the files the port was built from.
    public static class SetupFacts
    {
        public const string ElfSha256 = "D65B7E9C68B6EDA6CB838D9D36DE268D8CE248BDD4BC41E4C9F58B01B5EE19FD";      // game\EMAIN.ELF (1.22)
        public const string SelfSha256 = "439F412CA4E1E3E76D8CE0B5C466BD9108DFDA712A5EC5FBE7E29AE970185708";     // USRDIR\TPPS-01.22\EMAIN.SELF
        public const string LoaderSha256 = "EEEB8551A86F9777BE3D15183B26CF3FDB6D022BFE29EDEA2AC59B7028598ECF";   // USRDIR\EBOOT.BIN (1.22)
        public const string LibsreSha256 = "9E7EE6EDC99B1D372686CF927B883B18F19ED4D32D1FAF35E214CCA4AE0737B6";   // libsre.prx, decrypted
        public const string LibsreImageSha256 = "088BD13689C4980D063C7188631A5904BE4BFA510620EF539FFB987985E660BA";
        public const uint LibsreBase = 0x30000000;   // stubs.cpp ps3_load_prx_modules
        public const long VolumeSize = 15407310848;  // PS3_GAME\USRDIR\GT.VOL
        public const string VolumeSha256 = "C0B645491F0CE68D2C237F41A3A4F2DCAFE0A59C184674ADB0AF28E5D831355C";
        public const string VolumeHeadSha256 = "AC9CB7D2AB0DF1036839022B191E45ACC1429375067821850052A70CF249F8CB";   // its first MiB
        public const string FinalSelf = "TPPS-01.22\\EMAIN.SELF";

        // RPCS3's own Windows build, the one the steps below were tried with (its dialogs are answered by name).
        public const string ToolUrl = "https://github.com/RPCS3/rpcs3-binaries-win/releases/download/build-eee3f0f7c285f2b8aa18fec278a2146c5cd874eb/rpcs3-v0.0.43-20565-eee3f0f7_win64_msvc.7z";
        public const string ToolSha256 = "FDEC321625C856DCF2D84EDA128F75F24A428230A233F1B2BF12C490882870E7";
        public const long ToolSize = 37210593;

        // Firmware 4.93. Sony's list names the current file; the address below is the one it named on 2026-11-22.
        public const string FirmwareList = "http://fus01.ps3.update.playstation.net/update/ps3/list/us/ps3-updatelist.txt";
        public const string FirmwareUrl = "http://dus01.ps3.update.playstation.net/update/ps3/image/us/2026_0318_a2b60b6ac1d2e49e230144345616927c/PS3UPDAT.PUP";
        public const string FirmwareSha256 = "158471FD834F8EA8036136B6AAB43CD86C7BA73D79CA30E0AF3C0FE0001CF365";
        public const long FirmwareSize = 206197916;

        // The game's updates 1.01 - 1.22 (Sony's list https://a0.ww.np.dl.playstation.net/tpl/np/BCES01893/BCES01893-ver.xml
        // names 1.02 - 1.22 with these sizes and hashes; 1.01 is no longer listed there but is still on the server).
        // They are deltas and are installed in this order.
        public static readonly UpdatePackage[] Updates =
        {
            new UpdatePackage("01.01", "http://b0.ww.np.dl.playstation.net/tppkg/np/BCES01893/BCES01893_T27/7e3350cee3aeb220/EP9001-BCES01893_00-0000000000000000-A0101-V0100-PE.pkg", 1264888528L, "597b2bb218171ea36ef7f5718279e115b6ee7da4"),
            new UpdatePackage("01.02", "http://b0.ww.np.dl.playstation.net/tppkg/np/BCES01893/BCES01893_T359/b80952de9329a9ba/EP9001-BCES01893_00-0000000000000000-A0102-V0101-PE.pkg", 1315387312L, "d68d0584aa30b6d02a7aee95236f91ef43fdc11e"),
            new UpdatePackage("01.03", "http://b0.ww.np.dl.playstation.net/tppkg/np/BCES01893/BCES01893_T359/b80952de9329a9ba/EP9001-BCES01893_00-0000000000000000-A0103-V0100-PE.pkg", 102857792L, "7b1c6b9dd621ed9a5ae130737bfa5a4dd386db66"),
            new UpdatePackage("01.04", "http://b0.ww.np.dl.playstation.net/tppkg/np/BCES01893/BCES01893_T359/b80952de9329a9ba/EP9001-BCES01893_00-0000000000000000-A0104-V0100-PE.pkg", 223699280L, "4cedbc07c384b0f5556d5f0697b03c81376fd89d"),
            new UpdatePackage("01.05", "http://b0.ww.np.dl.playstation.net/tppkg/np/BCES01893/BCES01893_T359/b80952de9329a9ba/EP9001-BCES01893_00-0000000000000000-A0105-V0100-PE.pkg", 104507968L, "25c84d212167ccdbcc57905e67f9deb3c349a364"),
            new UpdatePackage("01.06", "http://b0.ww.np.dl.playstation.net/tppkg/np/BCES01893/BCES01893_T359/b80952de9329a9ba/EP9001-BCES01893_00-0000000000000000-A0106-V0100-PE.pkg", 308954240L, "0d17b6862e2a431e8be9850f7342efd9a0d3b741"),
            new UpdatePackage("01.07", "http://b0.ww.np.dl.playstation.net/tppkg/np/BCES01893/BCES01893_T359/b80952de9329a9ba/EP9001-BCES01893_00-0000000000000000-A0107-V0100-PE.pkg", 1006648464L, "8f11c0eb5ddaa93c06408e0080a1ded738807f7a"),
            new UpdatePackage("01.08", "http://b0.ww.np.dl.playstation.net/tppkg/np/BCES01893/BCES01893_T359/b80952de9329a9ba/EP9001-BCES01893_00-0000000000000000-A0108-V0100-PE.pkg", 1546878976L, "ef515808cf3762ade94d4e231f363fb4ec261b71"),
            new UpdatePackage("01.09", "http://b0.ww.np.dl.playstation.net/tppkg/np/BCES01893/BCES01893_T359/b80952de9329a9ba/EP9001-BCES01893_00-0000000000000000-A0109-V0100-PE.pkg", 626582896L, "73e2adb5b4ebd65e8869907a925f3ad1801fe82e"),
            new UpdatePackage("01.10", "http://b0.ww.np.dl.playstation.net/tppkg/np/BCES01893/BCES01893_T359/b80952de9329a9ba/EP9001-BCES01893_00-0000000000000000-A0110-V0100-PE.pkg", 2217258064L, "6583cfd46ac634a42388c7667e23b17d10cc4477"),
            new UpdatePackage("01.11", "http://b0.ww.np.dl.playstation.net/tppkg/np/BCES01893/BCES01893_T359/b80952de9329a9ba/EP9001-BCES01893_00-0000000000000000-A0111-V0100-PE.pkg", 30865776L, "f1c66083b93ea7a21cd9bb2d5e3124452a15e3b5"),
            new UpdatePackage("01.12", "http://b0.ww.np.dl.playstation.net/tppkg/np/BCES01893/BCES01893_T359/b80952de9329a9ba/EP9001-BCES01893_00-0000000000000000-A0112-V0100-PE.pkg", 506497408L, "74d7e9ab67ef26cac129b4f29008898394c229a5"),
            new UpdatePackage("01.13", "http://b0.ww.np.dl.playstation.net/tppkg/np/BCES01893/BCES01893_T359/b80952de9329a9ba/EP9001-BCES01893_00-0000000000000000-A0113-V0100-PE.pkg", 9830320L, "81b84e26fdf6f8e791de9869de4db9bec4ea6568"),
            new UpdatePackage("01.14", "http://b0.ww.np.dl.playstation.net/tppkg/np/BCES01893/BCES01893_T359/b80952de9329a9ba/EP9001-BCES01893_00-0000000000000000-A0114-V0100-PE.pkg", 574299472L, "3abad3972dc8ddcfbb814d30b56c3eb9215e3b36"),
            new UpdatePackage("01.15", "http://b0.ww.np.dl.playstation.net/tppkg/np/BCES01893/BCES01893_T359/b80952de9329a9ba/EP9001-BCES01893_00-0000000000000000-A0115-V0100-PE.pkg", 617542576L, "2d1a534f6e6c34f3c7a51673823ad4b63deeefc3"),
            new UpdatePackage("01.16", "http://b0.ww.np.dl.playstation.net/tppkg/np/BCES01893/BCES01893_T359/b80952de9329a9ba/EP9001-BCES01893_00-0000000000000000-A0116-V0100-PE.pkg", 561702416L, "f595540d1e342fbe9481d9fa603d043b019010ed"),
            new UpdatePackage("01.17", "http://b0.ww.np.dl.playstation.net/tppkg/np/BCES01893/BCES01893_T359/b80952de9329a9ba/EP9001-BCES01893_00-0000000000000000-A0117-V0100-PE.pkg", 415587168L, "f83790d801d5e6dd98c0466584160e12257590d9"),
            new UpdatePackage("01.18", "http://b0.ww.np.dl.playstation.net/tppkg/np/BCES01893/BCES01893_T359/b80952de9329a9ba/EP9001-BCES01893_00-0000000000000000-A0118-V0100-PE.pkg", 215434256L, "b2e23bf2bc1928bdd2aac4d6de9912e4b13c4ad5"),
            new UpdatePackage("01.19", "http://b0.ww.np.dl.playstation.net/tppkg/np/BCES01893/BCES01893_T359/b80952de9329a9ba/EP9001-BCES01893_00-0000000000000000-A0119-V0100-PE.pkg", 282711744L, "fa59ff9c903a4749d56f6f055764f904f8c07fd4"),
            new UpdatePackage("01.20", "http://b0.ww.np.dl.playstation.net/tppkg/np/BCES01893/BCES01893_T359/b80952de9329a9ba/EP9001-BCES01893_00-0000000000000000-A0120-V0100-PE.pkg", 325353952L, "888b5ff65d4346016ed964014190f7d1d2376f10"),
            new UpdatePackage("01.21", "http://b0.ww.np.dl.playstation.net/tppkg/np/BCES01893/BCES01893_T359/b80952de9329a9ba/EP9001-BCES01893_00-0000000000000000-A0121-V0100-PE.pkg", 193535024L, "aec783dc1fc4faaf40c77c430bf063fd146a2bd9"),
            new UpdatePackage("01.22", "http://b0.ww.np.dl.playstation.net/tppkg/np/BCES01893/BCES01893_T359/b80952de9329a9ba/EP9001-BCES01893_00-0000000000000000-A0122-V0100-PE.pkg", 36344400L, "14893bb9efdfe34944dc1e8cb38373d69edf9549"),
        };

        public static long UpdatesSize { get { return Updates.Sum(u => u.Size); } }

        // The disc's files the port uses (path below the disc's root; the big one last).
        public static readonly string[] DiscFiles =
        {
            "PS3_DISC.SFB", "PS3_GAME/PARAM.SFO", "PS3_GAME/ICON0.PNG", "PS3_GAME/PS3LOGO.DAT", "PS3_GAME/LICDIR/LIC.DAT", "PS3_GAME/USRDIR/GT.VOL",
        };
    }

    public class IsoFile
    {
        public string Path;
        public long Offset, Size;
    }

    // A PS3 disc image as a file: ISO 9660, read directly (nothing is mounted). A file over 4 GB is stored as several
    // directory records (flag 0x80 = more follow); on PS3 discs they follow each other, so a file is one byte range.
    public static class Iso9660
    {
        const int Sector = 2048;

        static void ReadFully(Stream s, byte[] buffer, int count)
        {
            int done = 0;
            while (done < count)
            {
                int n = s.Read(buffer, done, count - done);
                if (n <= 0)
                    throw new EndOfStreamException();
                done += n;
            }
        }

        public static List<IsoFile> List(string iso)
        {
            using (var f = new FileStream(iso, FileMode.Open, FileAccess.Read, FileShare.Read))
            {
                if (f.Length < 17L * Sector)
                    throw new InvalidDataException("too short");
                var pvd = new byte[Sector];
                f.Seek(16L * Sector, SeekOrigin.Begin);
                ReadFully(f, pvd, Sector);
                if (pvd[1] != 'C' || pvd[2] != 'D' || pvd[3] != '0' || pvd[4] != '0' || pvd[5] != '1')
                    throw new InvalidDataException("no ISO 9660 volume descriptor");
                var files = new List<IsoFile>();
                Walk(f, BitConverter.ToUInt32(pvd, 158), BitConverter.ToUInt32(pvd, 166), "", files, 0);
                return files;
            }
        }

        static void Walk(FileStream f, uint lba, uint size, string prefix, List<IsoFile> files, int depth)
        {
            if (depth > 12 || size > (32u << 20) || (long)lba * Sector + size > f.Length)
                throw new InvalidDataException("directory out of range");
            var data = new byte[size];
            f.Seek((long)lba * Sector, SeekOrigin.Begin);
            ReadFully(f, data, (int)size);
            string name = null;
            long first = 0, total = 0, next = 0;
            int p = 0;
            while (p < data.Length)
            {
                int length = data[p];
                if (length == 0)
                {
                    p = (p / Sector + 1) * Sector;   // records do not cross sectors
                    continue;
                }
                if (p + 33 > data.Length)
                    break;
                uint extent = BitConverter.ToUInt32(data, p + 2), extentSize = BitConverter.ToUInt32(data, p + 10);
                int flags = data[p + 25], nameLength = data[p + 32];
                bool self = nameLength == 1 && data[p + 33] <= 1;
                string record = self || p + 33 + nameLength > data.Length ? null : Encoding.ASCII.GetString(data, p + 33, nameLength);
                p += length;
                if (record == null)
                    continue;
                int version = record.IndexOf(';');
                if (version >= 0)
                    record = record.Substring(0, version);
                if ((flags & 2) != 0)
                {
                    Walk(f, extent, extentSize, prefix + record + "/", files, depth + 1);
                    continue;
                }
                long blocks = ((long)extentSize + Sector - 1) / Sector;
                if (name != null && name == record)
                {
                    if (extent != next)
                        throw new InvalidDataException("a file's parts do not follow each other: " + prefix + record);
                    total += extentSize;
                    next = extent + blocks;
                }
                else
                {
                    name = record;
                    first = extent;
                    total = extentSize;
                    next = extent + blocks;
                }
                if ((flags & 0x80) == 0)
                {
                    files.Add(new IsoFile { Path = prefix + name, Offset = first * Sector, Size = total });
                    name = null;
                }
            }
        }

        public static IsoFile Find(List<IsoFile> files, string path)
        {
            return files.FirstOrDefault(e => e.Path.Equals(path, StringComparison.OrdinalIgnoreCase));
        }

        public static byte[] Read(string iso, IsoFile file)
        {
            if (file.Size > 64 << 20)
                throw new InvalidDataException("too large to read whole");
            return ReadAt(iso, file.Offset, (int)file.Size);
        }

        public static byte[] ReadAt(string path, long offset, int count)
        {
            using (var f = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read))
            {
                var data = new byte[count];
                f.Seek(offset, SeekOrigin.Begin);
                ReadFully(f, data, count);
                return data;
            }
        }
    }

    // The user's game as the setup found it: a disc image or a disc folder (the one that holds PS3_GAME).
    public class GameSource
    {
        public string Path, TitleId = "", Title = "";
        public bool IsIso;
        public long VolumeSize, VolumeOffset;
        public string Problem;   // why it cannot be used, or null

        public bool Usable { get { return Problem == null; } }
    }

    public static class GameFinder
    {
        // What a file or folder is, without changing anything.
        public static GameSource Inspect(string path)
        {
            var source = new GameSource { Path = path };
            try
            {
                Dictionary<string, string> sfo;
                if (File.Exists(path))
                {
                    source.IsIso = true;
                    List<IsoFile> files;
                    try
                    {
                        files = Iso9660.List(path);
                    }
                    catch (Exception)
                    {
                        source.Problem = L.T("디스크 이미지로 읽을 수 없습니다. 복호화된(decrypted) PS3 ISO가 필요합니다.");
                        return source;
                    }
                    IsoFile param = Iso9660.Find(files, "PS3_GAME/PARAM.SFO"), volume = Iso9660.Find(files, "PS3_GAME/USRDIR/GT.VOL");
                    if (param == null)
                    {
                        source.Problem = L.T("PS3 게임 디스크의 이미지가 아닙니다 (PS3_GAME 폴더가 없습니다).");
                        return source;
                    }
                    sfo = Sfo.Parse(Iso9660.Read(path, param));
                    source.VolumeSize = volume == null ? 0 : volume.Size;
                    source.VolumeOffset = volume == null ? 0 : volume.Offset;
                }
                else if (Directory.Exists(path))
                {
                    string param = System.IO.Path.Combine(path, "PS3_GAME", "PARAM.SFO"), volume = System.IO.Path.Combine(path, "PS3_GAME", "USRDIR", "GT.VOL");
                    if (!File.Exists(param))
                    {
                        source.Problem = L.T("이 폴더에 PS3_GAME 폴더가 없습니다. 디스크의 맨 위 폴더(PS3_GAME이 들어 있는 폴더)를 골라 주세요.");
                        return source;
                    }
                    sfo = Sfo.Read(param);
                    source.VolumeSize = File.Exists(volume) ? new FileInfo(volume).Length : 0;
                }
                else
                {
                    source.Problem = L.T("파일이나 폴더를 찾을 수 없습니다.");
                    return source;
                }
                source.TitleId = Sfo.Get(sfo, "TITLE_ID");
                source.Title = Sfo.Get(sfo, "TITLE").Replace("\n", " ").Trim();
                if (source.TitleId != GameFiles.TitleId)
                    source.Problem = (source.TitleId.Length > 0 ? source.Title + " (" + source.TitleId + ")" : L.T("알 수 없는 디스크")) +
                                     L.T(" 입니다. 이 포트는 유럽판 Gran Turismo 6 (BCES01893)만 지원합니다. 다른 지역판은 지원하지 않습니다.");
                else if (source.VolumeSize != SetupFacts.VolumeSize)
                    source.Problem = L.T("게임의 데이터 파일(GT.VOL)이 없거나 크기가 다릅니다. 온전한 디스크 이미지가 필요합니다.");
                else
                {
                    // The data file's first MiB as the disc holds it: an image dumped without decryption (Redump) has
                    // other bytes there.
                    byte[] head = source.IsIso ? Iso9660.ReadAt(path, source.VolumeOffset, 1 << 20)
                                               : Iso9660.ReadAt(System.IO.Path.Combine(path, "PS3_GAME", "USRDIR", "GT.VOL"), 0, 1 << 20);
                    if (Hashes.Sha256(head) != SetupFacts.VolumeHeadSha256)
                        source.Problem = L.T("이 디스크의 데이터 파일(GT.VOL)이 기대한 내용이 아닙니다. 암호화된 채로 덤프한 이미지(Redump 원본)라면 복호화된 ISO가 필요합니다.");
                }
            }
            catch (Exception error)
            {
                source.Problem = L.T("읽지 못했습니다: ") + error.Message;
            }
            return source;
        }

        // Looks for the game where people keep such files: beside the launcher, Desktop, Downloads, Documents, and the
        // drives a few levels deep. Only names and sizes are looked at until a candidate is found (an image of this
        // game is 25 GB; nothing under 15 GB is opened).
        public static List<GameSource> Find(Launcher launcher, Func<bool> cancelled)
        {
            var found = new List<GameSource>();
            var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            string home = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
            var roots = new List<KeyValuePair<string, int>>
            {
                new KeyValuePair<string, int>(launcher.Dir, 2),
                new KeyValuePair<string, int>(System.IO.Path.GetDirectoryName(launcher.Dir) ?? launcher.Dir, 2),
                new KeyValuePair<string, int>(Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory), 3),
                new KeyValuePair<string, int>(System.IO.Path.Combine(home, "Downloads"), 3),
                new KeyValuePair<string, int>(Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments), 3),
            };
            try
            {
                foreach (DriveInfo drive in DriveInfo.GetDrives())
                    if ((drive.DriveType == DriveType.Fixed || drive.DriveType == DriveType.Removable) && drive.IsReady)
                        roots.Add(new KeyValuePair<string, int>(drive.RootDirectory.FullName, 3));
            }
            catch (Exception)
            {
            }
            var watch = Stopwatch.StartNew();
            foreach (KeyValuePair<string, int> root in roots)
            {
                if (found.Count(s => s.Usable) > 0 || watch.Elapsed.TotalSeconds > 25 || (cancelled != null && cancelled()))
                    break;
                Search(root.Key, root.Value, found, seen, watch, cancelled);
            }
            return found.OrderByDescending(s => s.Usable).ToList();
        }

        static void Search(string dir, int depth, List<GameSource> found, HashSet<string> seen, Stopwatch watch, Func<bool> cancelled)
        {
            if (string.IsNullOrEmpty(dir) || !seen.Add(dir.TrimEnd('\\')) || watch.Elapsed.TotalSeconds > 25 || (cancelled != null && cancelled()))
                return;
            try
            {
                if (File.Exists(System.IO.Path.Combine(dir, "PS3_GAME", "PARAM.SFO")))
                {
                    GameSource folder = Inspect(dir);
                    if (folder.TitleId == GameFiles.TitleId)
                        found.Add(folder);
                    return;
                }
                foreach (string file in Directory.GetFiles(dir, "*.iso"))
                {
                    if (new FileInfo(file).Length < SetupFacts.VolumeSize)
                        continue;
                    GameSource iso = Inspect(file);
                    if (iso.TitleId == GameFiles.TitleId)
                        found.Add(iso);
                }
                if (depth == 0 || found.Count(s => s.Usable) > 0)
                    return;
                foreach (string sub in Directory.GetDirectories(dir))
                {
                    string name = System.IO.Path.GetFileName(sub);
                    if (name.StartsWith(".") || name.StartsWith("$") || name.StartsWith("dev_", StringComparison.OrdinalIgnoreCase) ||
                        name.Equals("Windows", StringComparison.OrdinalIgnoreCase) || name.Equals("AppData", StringComparison.OrdinalIgnoreCase) ||
                        name.Equals("ProgramData", StringComparison.OrdinalIgnoreCase) || name.Equals("node_modules", StringComparison.OrdinalIgnoreCase) ||
                        name.StartsWith("Program Files", StringComparison.OrdinalIgnoreCase) || name.Equals("_setup", StringComparison.OrdinalIgnoreCase))
                        continue;
                    if ((File.GetAttributes(sub) & FileAttributes.ReparsePoint) != 0)
                        continue;
                    Search(sub, depth - 1, found, seen, watch, cancelled);
                }
            }
            catch (Exception)
            {
            }
        }
    }

    // Downloads to a file; an unfinished one (<target>.part) is continued where it stopped.
    public static class Fetch
    {
        public static void ToFile(string address, string target, long expected, Action<long> progress, Func<bool> cancelled)
        {
            ServicePointManager.SecurityProtocol |= (SecurityProtocolType)3072;   // TLS 1.2
            string part = target + ".part";
            Directory.CreateDirectory(Path.GetDirectoryName(target));
            for (int attempt = 0; ; attempt++)
            {
                long have = File.Exists(part) ? new FileInfo(part).Length : 0;
                if (expected > 0 && have > expected)
                {
                    File.Delete(part);
                    have = 0;
                }
                try
                {
                    if (expected <= 0 || have < expected)
                    {
                        var request = (HttpWebRequest)WebRequest.Create(address);
                        request.UserAgent = "GT6Launcher";
                        request.Timeout = 30000;
                        request.ReadWriteTimeout = 60000;
                        if (have > 0)
                            request.AddRange(have);
                        using (var response = (HttpWebResponse)request.GetResponse())
                        {
                            if (have > 0 && response.StatusCode != HttpStatusCode.PartialContent)
                                have = 0;   // the server sends the whole file again
                            using (Stream from = response.GetResponseStream())
                            using (var to = new FileStream(part, have > 0 ? FileMode.Append : FileMode.Create, FileAccess.Write, FileShare.Read))
                            {
                                var buffer = new byte[1 << 18];
                                int n;
                                long done = have;
                                while ((n = from.Read(buffer, 0, buffer.Length)) > 0)
                                {
                                    to.Write(buffer, 0, n);
                                    done += n;
                                    if (progress != null)
                                        progress(n);
                                    if (cancelled != null && cancelled())
                                        throw new OperationCanceledException();
                                }
                            }
                        }
                    }
                    long size = new FileInfo(part).Length;
                    if (expected > 0 && size != expected)
                        throw new IOException("size " + size + " instead of " + expected);
                    if (File.Exists(target))
                        File.Delete(target);
                    File.Move(part, target);
                    return;
                }
                catch (OperationCanceledException)
                {
                    throw;
                }
                catch (Exception)
                {
                    if (attempt >= 5)
                        throw;
                    Thread.Sleep(1500 * (attempt + 1));
                }
            }
        }

        public static string Text(string address)
        {
            ServicePointManager.SecurityProtocol |= (SecurityProtocolType)3072;
            var request = (HttpWebRequest)WebRequest.Create(address);
            request.UserAgent = "GT6Launcher";
            request.Timeout = 20000;
            using (var response = (HttpWebResponse)request.GetResponse())
            using (var reader = new StreamReader(response.GetResponseStream(), Encoding.UTF8))
                return reader.ReadToEnd();
        }
    }

    // (Windows' own hashing (CNG): several times faster than the managed one on 15 GB.)
    public static class Hashes
    {
        public static string Sha256(string path)
        {
            using (var sha = new SHA256Cng())
            using (var f = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 20))
                return BitConverter.ToString(sha.ComputeHash(f)).Replace("-", "");
        }

        public static string Sha256(byte[] data)
        {
            using (var sha = new SHA256Cng())
                return BitConverter.ToString(sha.ComputeHash(data)).Replace("-", "");
        }

        public static string Hex(byte[] hash)
        {
            return BitConverter.ToString(hash).Replace("-", "");
        }

        // A PS3 package checks itself: its last 0x20 bytes begin with the SHA-1 of everything before them.
        public static bool PackageIntact(string path, string sha1)
        {
            using (var sha = new SHA1Cng())
            using (var f = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 20))
            {
                long body = f.Length - 0x20;
                if (body <= 0)
                    return false;
                var buffer = new byte[1 << 20];
                while (body > 0)
                {
                    int n = f.Read(buffer, 0, (int)Math.Min(buffer.Length, body));
                    if (n <= 0)
                        return false;
                    sha.TransformBlock(buffer, 0, n, null, 0);
                    body -= n;
                }
                sha.TransformFinalBlock(buffer, 0, 0);
                var footer = new byte[20];
                if (f.Read(footer, 0, 20) != 20)
                    return false;
                string computed = BitConverter.ToString(sha.Hash).Replace("-", ""), stored = BitConverter.ToString(footer).Replace("-", "");
                return computed == stored && (string.IsNullOrEmpty(sha1) || computed.Equals(sha1, StringComparison.OrdinalIgnoreCase));
            }
        }
    }
}
