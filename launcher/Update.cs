// New versions of the package (the launcher and the native game builds together) from GitHub Releases: the newest
// release's zip (GT6_PC_<yyyyMMdd-HHmm>.zip, made by tools\make_release.ps1) against this folder's version
// (version.txt, else the newest game exe's time). The repository address is not built in: it is launcher.ini
// [Launcher] Repository=owner/name (Updates · Tools page), empty until the project is on GitHub, and nothing is asked of the
// network while it is empty.
// Installing replaces only the app files of the zip. The game data, the saves, the mods, the logs, launcher.ini and
// the menu's settings are never written, whatever the zip holds (Protected). Every file about to be replaced is
// copied to update\backup first; "Go back to the previous version" puts those back. A failed install puts them back by itself.
using System;
using System.Collections;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Net;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Web.Script.Serialization;
using System.Windows;

namespace GT6Launcher
{
    public class ReleaseInfo
    {
        public string Tag, Name, Notes, PageUrl, ZipName, ZipUrl, AssetUrl, Stamp;   // AssetUrl: the asset through the API (a private repository)
        public long ZipSize;
        public bool Newer;
        public bool NoRelease;   // the repository is there (and readable) but has no release yet
        public string Error;
    }

    // The token for a private repository. Two sources, in this order: a token the user pasted into the launcher
    // (kept in launcher.ini encrypted for this Windows account - DPAPI - so the file alone is of no use to anyone
    // else), or, when the user switched that on, the login of the GitHub CLI on this PC (gh auth token). A real token
    // is only ever sent to https://api.github.com: an update address that points elsewhere gets none.
    public static class Auth
    {
        static readonly byte[] Entropy = Encoding.UTF8.GetBytes("GT6Launcher.token.v1");
        static string ghToken;
        static bool ghAsked;

        public static bool HasSaved(Launcher launcher)
        {
            return launcher.State("TokenProtected", "").Length > 0;
        }

        public static void Save(Launcher launcher, string token)
        {
            byte[] sealedBytes = ProtectedData.Protect(Encoding.UTF8.GetBytes(token.Trim()), Entropy, DataProtectionScope.CurrentUser);
            launcher.SetState("TokenProtected", Convert.ToBase64String(sealedBytes));
        }

        public static void Clear(Launcher launcher)
        {
            launcher.SetState("TokenProtected", "");
        }

        static string Saved(Launcher launcher)
        {
            try
            {
                string text = launcher.State("TokenProtected", "");
                if (text.Length == 0)
                    return null;
                string token = Encoding.UTF8.GetString(ProtectedData.Unprotect(Convert.FromBase64String(text), Entropy, DataProtectionScope.CurrentUser));
                return token.Length > 0 ? token : null;
            }
            catch (Exception)
            {
                return null;   // another Windows account or a damaged value: as if there were none
            }
        }

        // gh.exe on the PATH, or null.
        public static string GhPath()
        {
            try
            {
                foreach (string dir in (Environment.GetEnvironmentVariable("PATH") ?? "").Split(';'))
                {
                    if (dir.Trim().Length == 0)
                        continue;
                    string path = Path.Combine(dir.Trim().Trim('"'), "gh.exe");
                    if (File.Exists(path))
                        return path;
                }
            }
            catch (Exception)
            {
            }
            return null;
        }

        static string FromGh()
        {
            if (ghAsked)
                return ghToken;
            ghAsked = true;
            try
            {
                string gh = GhPath();
                if (gh == null)
                    return null;
                var info = new ProcessStartInfo(gh, "auth token --hostname github.com") { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true };
                using (Process p = Process.Start(info))
                {
                    string text = p.StandardOutput.ReadToEnd().Trim();
                    if (!p.WaitForExit(8000))
                        return null;
                    if (p.ExitCode == 0 && text.Length > 0 && !text.Contains(" "))
                        ghToken = text;
                }
            }
            catch (Exception)
            {
            }
            return ghToken;
        }

        // The token a request to this address may carry, or null.
        public static string For(Launcher launcher, string address)
        {
            Uri uri;
            if (!Uri.TryCreate(address, UriKind.Absolute, out uri) || uri.IsFile)
                return null;
            if (uri.IsLoopback)
            {
                // (trying the updater out against a stand-in server on this PC: its own made-up token, nothing real)
                string test = Environment.GetEnvironmentVariable("GT6_LAUNCHER_TEST_TOKEN");
                return string.IsNullOrEmpty(test) ? null : test;
            }
            if (uri.Scheme != Uri.UriSchemeHttps || !uri.Host.Equals("api.github.com", StringComparison.OrdinalIgnoreCase))
                return null;
            string saved = Saved(launcher);
            if (saved != null)
                return saved;
            return launcher.Setting("UseGhCli") == "1" ? FromGh() : null;
        }

        // Which source is in force, in words (never the token itself).
        public static string Describe(Launcher launcher)
        {
            if (Saved(launcher) != null)
                return L.T("저장된 토큰 사용");
            if (launcher.Setting("UseGhCli") == "1")
                return FromGh() != null ? L.T("이 PC의 GitHub CLI 로그인 사용") : L.T("GitHub CLI 로그인을 쓰도록 했지만 gh가 토큰을 주지 않음");
            return L.T("토큰 없음");
        }
    }

    public static class PortUpdate
    {
        const string OldName = "GT6 Launcher.old.exe";
        static readonly Regex ZipPattern = new Regex(@"^GT6_PC_(\d{8}-\d{4})\.zip$", RegexOptions.IgnoreCase);
        public static bool Disabled;   // --shot: no network

        // The port's public repository: every copy of the launcher looks there for a new release (the launcher itself and
        // the game exe), unless another address was typed in on the tools page. So a release published there reaches
        // everyone who downloaded the port: found by the daily check, downloaded in the background, applied at the next
        // start of the launcher (ApplyAtStart).
        public const string DefaultRepository = "PrimeTimerer/Project-Grand-Tourer-Advanced";

        // "owner/name", from what the user typed (a GitHub address is cut down to it); the public repository when there is none.
        public static string Repository(Launcher launcher)
        {
            string own = Normalize(launcher.State("Repository", ""));
            return own.Length > 0 ? own : DefaultRepository;
        }

        public static string Normalize(string typed)
        {
            string text = (typed ?? "").Trim();
            Match m = Regex.Match(text, @"^(?:https?://)?(?:www\.)?github\.com/([^/\s]+)/([^/\s#?]+)", RegexOptions.IgnoreCase);
            if (m.Success)
                text = m.Groups[1].Value + "/" + m.Groups[2].Value;
            if (text.EndsWith(".git", StringComparison.OrdinalIgnoreCase))
                text = text.Substring(0, text.Length - 4);
            return Regex.IsMatch(text, @"^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$") ? text : "";
        }

        // Where the newest release is described (GitHub's "latest release" JSON). [Launcher] UpdateFeed overrides it:
        // a mirror, or a file on this PC in the same form (how the updater is tried out before anything is online).
        public static string Feed(Launcher launcher)
        {
            string feed = launcher.State("UpdateFeed", "");
            if (feed.Length > 0)
                return feed;
            string repository = Repository(launcher);
            return repository.Length > 0 ? "https://api.github.com/repos/" + repository + "/releases/latest" : "";
        }

        public static bool Configured(Launcher launcher)
        {
            return Feed(launcher).Length > 0;
        }

        public static string ReleasesPage(Launcher launcher)
        {
            string repository = Repository(launcher);
            return repository.Length > 0 ? "https://github.com/" + repository + "/releases" : null;
        }

        // This folder's version: version.txt (a release), else the time of the newest of the game exes and the launcher
        // (as make_release.ps1 names the zip).
        public static string LocalStamp(Launcher launcher)
        {
            try
            {
                string file = Path.Combine(launcher.Dir, "version.txt");
                if (File.Exists(file))
                {
                    Match m = Regex.Match(File.ReadAllText(file), @"\d{8}-\d{4}");
                    if (m.Success)
                        return m.Value;
                }
                DateTime newest = DateTime.MinValue;
                var apps = new List<string>(Directory.GetFiles(launcher.Dir, "GT6Recomp*.exe"));
                apps.Add(Path.Combine(launcher.Dir, Launcher.ExeName));   // a release may bring a new launcher only
                foreach (string exe in apps)
                    if (File.Exists(exe) && !exe.EndsWith("_prev.exe", StringComparison.OrdinalIgnoreCase) && File.GetLastWriteTime(exe) > newest)
                        newest = File.GetLastWriteTime(exe);
                if (newest > DateTime.MinValue)
                    return newest.ToString("yyyyMMdd-HHmm");
            }
            catch (Exception)
            {
            }
            return "";
        }

        public static bool IsGitCheckout(Launcher launcher)
        {
            return Directory.Exists(Path.Combine(launcher.Dir, ".git"));
        }

        static bool IsLocal(string address)
        {
            Uri uri;
            return Uri.TryCreate(address, UriKind.Absolute, out uri) ? uri.IsFile : Path.IsPathRooted(address);
        }

        static string LocalPath(string address)
        {
            Uri uri;
            return Uri.TryCreate(address, UriKind.Absolute, out uri) && uri.IsFile ? uri.LocalPath : address;
        }

        static WebClient Web()
        {
            ServicePointManager.SecurityProtocol |= (SecurityProtocolType)3072;   // TLS 1.2
            var web = new WebClient { Encoding = Encoding.UTF8 };
            web.Headers[HttpRequestHeader.UserAgent] = "GT6Launcher";
            return web;
        }

        static string Get(string address, string token)
        {
            if (IsLocal(address))
                return File.ReadAllText(LocalPath(address), Encoding.UTF8);
            using (WebClient web = Web())
            {
                web.Headers[HttpRequestHeader.Accept] = "application/vnd.github+json";
                if (token != null)
                    web.Headers[HttpRequestHeader.Authorization] = "Bearer " + token;
                return web.DownloadString(address);
            }
        }

        static int Status(WebException error)
        {
            var response = error.Response as HttpWebResponse;
            return response == null ? 0 : (int)response.StatusCode;
        }

        // An asset of a private repository: asked of the API with the token (Accept: application/octet-stream), which
        // answers with a redirect to a signed address of the file store. That second request must go WITHOUT the
        // token (the store refuses a request that carries two kinds of authorization), so the redirect is followed
        // by hand.
        static void DownloadAsset(string address, string token, string target)
        {
            ServicePointManager.SecurityProtocol |= (SecurityProtocolType)3072;
            var request = (HttpWebRequest)WebRequest.Create(address);
            request.UserAgent = "GT6Launcher";
            request.Accept = "application/octet-stream";
            request.AllowAutoRedirect = false;
            request.Headers[HttpRequestHeader.Authorization] = "Bearer " + token;
            using (var response = (HttpWebResponse)request.GetResponse())
            {
                int code = (int)response.StatusCode;
                if (code >= 300 && code < 400)
                {
                    string location = response.Headers[HttpResponseHeader.Location];
                    if (string.IsNullOrEmpty(location))
                        throw new InvalidDataException(L.T("GitHub이 파일의 주소를 주지 않았습니다."));
                    using (WebClient web = Web())
                        web.DownloadFile(new Uri(new Uri(address), location), target);
                    return;
                }
                if (code != 200)
                    throw new InvalidDataException(L.T("파일을 받지 못했습니다 (HTTP ") + code + ").");
                using (Stream body = response.GetResponseStream())
                using (FileStream file = File.Create(target))
                    body.CopyTo(file);
            }
        }

        public static ReleaseInfo Check(Launcher launcher)
        {
            var info = new ReleaseInfo();
            try
            {
                string feed = Feed(launcher);
                if (feed.Length == 0)
                    throw new InvalidOperationException(L.T("저장소 주소가 없습니다."));
                var json = new JavaScriptSerializer { MaxJsonLength = 8 * 1024 * 1024 };
                string token = Auth.For(launcher, feed), text;
                try
                {
                    text = Get(feed, token);
                }
                catch (WebException error)
                {
                    // GitHub answers 404 both for "no release yet" and for "you may not see this repository": the
                    // repository itself is asked to tell the two apart.
                    int status = Status(error);
                    if (status == 401)
                        throw new InvalidOperationException(L.T("토큰이 거부되었습니다 (만료되었거나 잘못된 토큰입니다)."));
                    if (status == 404 && feed.EndsWith("/releases/latest", StringComparison.OrdinalIgnoreCase))
                    {
                        bool visible = false;
                        try
                        {
                            Get(feed.Substring(0, feed.Length - "/releases/latest".Length), token);
                            visible = true;
                        }
                        catch (WebException)
                        {
                        }
                        if (visible)
                        {
                            info.NoRelease = true;
                            launcher.SetState("LastUpdateCheck", DateTime.Now.ToString("yyyy-MM-dd"));
                            return info;
                        }
                        throw new InvalidOperationException(token == null
                            ? L.T("저장소를 찾지 못했습니다. 비공개 저장소라면 토큰이 필요합니다 (아래 '비공개 저장소용 토큰').")
                            : L.T("이 토큰으로는 저장소를 볼 수 없습니다 (토큰에 이 저장소의 읽기 권한이 없습니다)."));
                    }
                    if (status == 403)
                        throw new InvalidOperationException(L.T("GitHub이 요청을 거절했습니다 (요청 횟수 한도이거나 권한 부족)."));
                    throw;
                }
                var release = json.Deserialize<Dictionary<string, object>>(text);
                info.Tag = release["tag_name"] as string;
                info.Name = release.ContainsKey("name") ? release["name"] as string : info.Tag;
                info.Notes = release.ContainsKey("body") ? release["body"] as string : "";
                info.PageUrl = release.ContainsKey("html_url") ? release["html_url"] as string : null;
                foreach (object a in (ArrayList)release["assets"])
                {
                    var asset = (Dictionary<string, object>)a;
                    string name = asset["name"] as string;
                    Match m = ZipPattern.Match(name ?? "");
                    if (!m.Success)
                        continue;
                    info.ZipName = name;
                    info.ZipUrl = asset["browser_download_url"] as string;
                    info.AssetUrl = asset.ContainsKey("url") ? asset["url"] as string : null;
                    info.ZipSize = Convert.ToInt64(asset["size"]);
                    info.Stamp = m.Groups[1].Value;
                }
                string local = LocalStamp(launcher);
                info.Newer = info.Stamp != null && string.CompareOrdinal(info.Stamp, local) > 0;
                launcher.SetState("LastUpdateCheck", DateTime.Now.ToString("yyyy-MM-dd"));
            }
            catch (Exception error)
            {
                info.Error = error.Message;
            }
            return info;
        }

        // Once a day when the launcher opens (nothing happens without a repository address).
        public static void CheckDaily(Launcher launcher, Action<ReleaseInfo> done)
        {
            if (Disabled || !Configured(launcher) || launcher.State("LastUpdateCheck", "") == DateTime.Now.ToString("yyyy-MM-dd") || launcher.Setting("UpdateCheck") == "0")
                return;
            ThreadPool.QueueUserWorkItem(delegate
            {
                ReleaseInfo info = Check(launcher);
                if (Application.Current != null)
                    Application.Current.Dispatcher.BeginInvoke((Action)delegate { done(info); });
            });
        }

        // update\download: a download in progress (removed at the next start); update\ready: a complete new version
        // waiting to be applied (ready.txt = "<stamp>\t<tag>", the files beside it); update\backup: the files the last
        // install replaced.
        static string DownloadDir(Launcher launcher) { return Path.Combine(launcher.Dir, "update", "download"); }
        static string ReadyDir(Launcher launcher) { return Path.Combine(launcher.Dir, "update", "ready"); }
        static string BackupDir(Launcher launcher) { return Path.Combine(launcher.Dir, "update", "backup"); }

        // Leftovers of the last update (the renamed launcher, a broken download), removed at the next start.
        public static void CleanUp(Launcher launcher)
        {
            string old = Path.Combine(launcher.Dir, OldName);
            string download = DownloadDir(launcher);
            for (int i = 0; i < 10 && (File.Exists(old) || Directory.Exists(download)); i++)   // the old launcher may still be closing
            {
                try
                {
                    if (File.Exists(old))
                        File.Delete(old);
                }
                catch (Exception)
                {
                }
                try
                {
                    if (Directory.Exists(download))
                        Directory.Delete(download, true);
                }
                catch (Exception)
                {
                }
                if (File.Exists(old) || Directory.Exists(download))
                    Thread.Sleep(200);
            }
        }

        // The version downloaded and waiting, or null: { stamp, tag }.
        public static string[] Ready(Launcher launcher)
        {
            try
            {
                string marker = Path.Combine(ReadyDir(launcher), "ready.txt");
                if (!File.Exists(marker))
                    return null;
                string[] parts = File.ReadAllText(marker).Trim().Split('\t');
                return parts.Length == 2 ? parts : null;
            }
            catch (Exception)
            {
                return null;
            }
        }

        // The version the last install replaced and that can be put back, or null.
        public static string Previous(Launcher launcher)
        {
            try
            {
                string marker = Path.Combine(BackupDir(launcher), "from.txt");
                return File.Exists(marker) ? File.ReadAllText(marker).Split('\t')[0] : null;
            }
            catch (Exception)
            {
                return null;
            }
        }

        // Downloads the release, checks it and keeps it in update\ready until it is applied.
        public static void Download(Launcher launcher, ReleaseInfo info, Action<string> progress)
        {
            if (info.ZipUrl == null)
                throw new InvalidOperationException(L.T("최신 릴리스에 GT6_PC_<날짜>.zip이 없습니다."));
            string work = DownloadDir(launcher);
            if (Directory.Exists(work))
                Directory.Delete(work, true);
            Directory.CreateDirectory(work);
            string zip = Path.Combine(work, info.ZipName);
            progress(L.T("받는 중: ") + info.ZipName + " (" + PcInfo.Size(info.ZipSize) + ")");
            string token = info.AssetUrl == null ? null : Auth.For(launcher, info.AssetUrl);
            if (IsLocal(info.ZipUrl))
                File.Copy(LocalPath(info.ZipUrl), zip, true);
            else if (token != null)
                DownloadAsset(info.AssetUrl, token, zip);   // a private repository: through the API, with the token
            else
                using (WebClient web = Web())
                    web.DownloadFile(info.ZipUrl, zip);
            if (new FileInfo(zip).Length != info.ZipSize)
                throw new InvalidDataException(L.T("받은 파일 크기가 다릅니다. 다시 시도하세요."));
            progress(L.T("압축을 푸는 중..."));
            string unpacked = Path.Combine(work, "new");
            Zip.Extract(zip, unpacked);
            string root = unpacked;
            string[] tops = Directory.GetDirectories(unpacked);
            if (!File.Exists(Path.Combine(root, Launcher.ExeName)) && tops.Length == 1)
                root = tops[0];
            if (!File.Exists(Path.Combine(root, Launcher.ExeName)))
                throw new InvalidDataException(L.T("zip 안에 런처(") + Launcher.ExeName + L.T(")가 없습니다. 이 포트의 릴리스 zip이 아닙니다."));
            string ready = ReadyDir(launcher);
            if (Directory.Exists(ready))
                Directory.Delete(ready, true);
            Directory.Move(root, ready);
            File.WriteAllText(Path.Combine(ready, "ready.txt"), info.Stamp + "\t" + info.Tag);
            Directory.Delete(work, true);
            progress(L.T("받아 둠: ") + info.Tag);
        }

        // Downloads and installs at once (the tools page button, --update); returns the new launcher's path.
        public static string Install(Launcher launcher, ReleaseInfo info, Action<string> progress)
        {
            if (IsGitCheckout(launcher))
                throw new InvalidOperationException(L.T("이 폴더는 git 저장소(개발 폴더)라 자동 설치를 하지 않습니다. 릴리스 zip을 다른 폴더에 받아 쓰세요."));
            if (Game.IsRunning)
                throw new InvalidOperationException(L.T("게임이 실행 중입니다. 게임을 닫은 뒤 다시 하세요."));
            string[] ready = Ready(launcher);
            if (ready == null || ready[0] != info.Stamp)
                Download(launcher, info, progress);
            return Apply(launcher, progress);
        }

        // Automatic updates: a new version found by the daily check is downloaded in the background (no question);
        // the next start of the launcher applies it before any window opens.
        public static bool AutoEnabled(Launcher launcher)
        {
            return launcher.Setting("AutoUpdate") != "0" && !IsGitCheckout(launcher);
        }

        public static void DownloadInBackground(Launcher launcher, ReleaseInfo info, Action<string> done)
        {
            string[] ready = Ready(launcher);
            if (ready != null && ready[0] == info.Stamp)
            {
                done(null);
                return;
            }
            ThreadPool.QueueUserWorkItem(delegate
            {
                string error = null;
                try
                {
                    Download(launcher, info, delegate(string line) { });
                }
                catch (Exception e)
                {
                    error = e.Message;
                }
                if (Application.Current != null)
                    Application.Current.Dispatcher.BeginInvoke((Action)delegate { done(error); });
            });
        }

        static void Log(Launcher launcher, string line)
        {
            try
            {
                Directory.CreateDirectory(launcher.LauncherLogsDir);
                File.AppendAllText(Path.Combine(launcher.LauncherLogsDir, "update.log"), DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss") + " " + line + "\r\n", new UTF8Encoding(false));
            }
            catch (Exception)
            {
            }
        }

        // At start (Program.Main): a waiting version newer than this one is applied, and the new launcher is started
        // with the same arguments. Returns true when this process should end. Any failure leaves the old version
        // running and is written to logs\launcher\update.log.
        public static bool ApplyAtStart(Launcher launcher, string[] args)
        {
            string[] ready = Ready(launcher);
            if (ready == null)
                return false;
            if (string.CompareOrdinal(ready[0], LocalStamp(launcher)) <= 0 || IsGitCheckout(launcher))
            {
                try
                {
                    Directory.Delete(ReadyDir(launcher), true);   // not newer (installed by hand), or a developer folder
                }
                catch (Exception)
                {
                }
                return false;
            }
            if (!AutoEnabled(launcher))
                return false;   // automatic updates off: it waits for the button on the tools page
            if (Game.IsRunning)
                return false;   // next time
            try
            {
                string next = Apply(launcher, delegate(string line) { });
                Log(launcher, ready[1] + L.T(" 적용"));
                Process.Start(new ProcessStartInfo(next, string.Join(" ", args.Select(a => "\"" + a + "\""))) { WorkingDirectory = launcher.Dir });
                return true;
            }
            catch (Exception error)
            {
                Log(launcher, ready[1] + L.T(" 적용 실패: ") + error.Message);
                return false;
            }
        }

        // --update: check, download and install with no window. 0 = installed or nothing new, 1 = failed, 2 = no address.
        public static int RunHeadless(Launcher launcher)
        {
            if (!Configured(launcher))
            {
                Log(launcher, L.T("저장소 주소가 없어 확인하지 않았습니다."));
                return 2;
            }
            ReleaseInfo info = Check(launcher);
            if (info.Error != null)
            {
                Log(launcher, L.T("확인 실패: ") + info.Error);
                return 1;
            }
            if (info.NoRelease)
            {
                Log(launcher, L.T("저장소는 보이지만 릴리스가 아직 없습니다 (") + Auth.Describe(launcher) + ").");
                return 0;
            }
            if (!info.Newer)
            {
                Log(launcher, L.T("새 버전 없음 (이 폴더 ") + LocalStamp(launcher) + L.T(", 최신 ") + (info.Stamp ?? "?") + ")");
                return 0;
            }
            try
            {
                Install(launcher, info, delegate(string line) { Log(launcher, line); });
                return 0;
            }
            catch (Exception error)
            {
                Log(launcher, L.T("설치 실패: ") + error.Message);
                return 1;
            }
        }

        // --rollback: the previous version put back with no window. 0 = done, 1 = failed or nothing to put back.
        public static int RollbackHeadless(Launcher launcher)
        {
            try
            {
                Rollback(launcher, delegate(string line) { });
                return 0;
            }
            catch (Exception error)
            {
                Log(launcher, L.T("되돌리기 실패: ") + error.Message);
                return 1;
            }
        }

        // Files an update never writes, whatever a zip holds: the game data, the saves and their backups, the mods, the
        // logs, the launcher's own state and the settings the user made in the game's menu and in the trainer.
        public static bool Protected(string relative)
        {
            string r = relative.Replace('/', '\\').TrimStart('\\').ToLowerInvariant();
            foreach (string dir in new[] { "game\\", "gamedata\\", "mods\\", "logs\\", "scratch\\", "update\\", "save_backups\\", "save_backup_before_play\\", "backup_before_" })
                if (r.StartsWith(dir))
                    return true;
            if (r == "launcher.ini" || r == "ready.txt" || r == OldName.ToLowerInvariant())
                return true;
            if (r.StartsWith("gt6menu.") && r.Contains(".settings.json"))
                return true;
            return false;
        }

        static List<string> Relative(string root)
        {
            if (!Directory.Exists(root))
                return new List<string>();
            string prefix = root.TrimEnd('\\') + "\\";
            return Directory.GetFiles(root, "*", SearchOption.AllDirectories).Select(f => f.Substring(prefix.Length)).Where(r => !Protected(r)).ToList();
        }

        // Copies the listed files of a folder over this one. The running launcher is renamed first (a running exe may be
        // renamed, not overwritten); CleanUp removes the renamed one at the next start.
        static void Replace(Launcher launcher, string from, List<string> files)
        {
            string me = Path.Combine(launcher.Dir, Launcher.ExeName), old = Path.Combine(launcher.Dir, OldName);
            bool moved = false;
            if (files.Any(f => f.Equals(Launcher.ExeName, StringComparison.OrdinalIgnoreCase)) && File.Exists(me))
            {
                if (File.Exists(old))
                    File.Delete(old);
                File.Move(me, old);
                moved = true;
            }
            try
            {
                foreach (string relative in files)
                {
                    string target = Path.Combine(launcher.Dir, relative);
                    Directory.CreateDirectory(Path.GetDirectoryName(target));
                    File.Copy(Path.Combine(from, relative), target, true);
                }
            }
            catch (Exception)
            {
                if (moved && !File.Exists(me))
                    File.Move(old, me);
                throw;
            }
        }

        // Puts the waiting version in place: every file it replaces is copied to update\backup first, then the app
        // files are copied over and version.txt is written. If the copy fails half way, the backup is put back.
        // Returns the new launcher's path.
        static string Apply(Launcher launcher, Action<string> progress)
        {
            string root = ReadyDir(launcher);
            string[] ready = Ready(launcher);
            if (ready == null)
                throw new InvalidOperationException(L.T("받아 둔 새 버전이 없습니다."));
            if (Game.IsRunning)
                throw new InvalidOperationException(L.T("게임이 실행 중입니다. 게임을 닫은 뒤 다시 하세요."));
            List<string> files = Relative(root);
            string backup = BackupDir(launcher), kept = Path.Combine(backup, "files"), versionFile = Path.Combine(launcher.Dir, "version.txt");
            progress(L.T("지금 버전을 백업하는 중..."));
            string oldStamp = LocalStamp(launcher);
            if (Directory.Exists(backup))
                Directory.Delete(backup, true);
            Directory.CreateDirectory(kept);
            var added = new List<string>();
            foreach (string relative in files)
            {
                string target = Path.Combine(launcher.Dir, relative);
                if (!File.Exists(target))
                {
                    added.Add(relative);
                    continue;
                }
                string keep = Path.Combine(kept, relative);
                Directory.CreateDirectory(Path.GetDirectoryName(keep));
                File.Copy(target, keep, true);
            }
            File.WriteAllLines(Path.Combine(backup, "added.txt"), added.ToArray());
            File.WriteAllText(Path.Combine(backup, "from.txt"), oldStamp + "\t" + (File.Exists(versionFile) ? "file" : "none"));
            progress(L.T("새 파일로 바꾸는 중..."));
            try
            {
                Replace(launcher, root, files);
            }
            catch (Exception)
            {
                try
                {
                    PutBack(launcher);   // the folder is as it was
                }
                catch (Exception)
                {
                }
                throw;
            }
            File.WriteAllText(versionFile, ready[0]);
            try
            {
                Directory.Delete(root, true);
            }
            catch (Exception)
            {
            }
            progress(L.T("설치 완료: ") + ready[1] + L.T(" (이전 버전 ") + oldStamp + L.T("은 update\\backup에 있습니다)"));
            return Path.Combine(launcher.Dir, Launcher.ExeName);
        }

        static void PutBack(Launcher launcher)
        {
            string backup = BackupDir(launcher), kept = Path.Combine(backup, "files"), versionFile = Path.Combine(launcher.Dir, "version.txt");
            string[] from = File.ReadAllText(Path.Combine(backup, "from.txt")).Split('\t');
            Replace(launcher, kept, Relative(kept));
            foreach (string added in File.ReadAllLines(Path.Combine(backup, "added.txt")))
            {
                try
                {
                    string path = Path.Combine(launcher.Dir, added);
                    if (added.Length > 0 && !Protected(added) && File.Exists(path))
                        File.Delete(path);
                }
                catch (Exception)
                {
                }
            }
            if (from.Length > 1 && from[1] == "file")
                File.WriteAllText(versionFile, from[0]);
            else if (File.Exists(versionFile))
                File.Delete(versionFile);
        }

        // "Go back to the previous version": the files the last install replaced are put back and the ones it added are removed.
        // Automatic updates are switched off, or the next start would install the same new version again.
        public static string Rollback(Launcher launcher, Action<string> progress)
        {
            if (Previous(launcher) == null)
                throw new InvalidOperationException(L.T("되돌릴 이전 버전이 없습니다."));
            if (Game.IsRunning)
                throw new InvalidOperationException(L.T("게임이 실행 중입니다. 게임을 닫은 뒤 다시 하세요."));
            string previous = Previous(launcher);
            progress(L.T("이전 버전 ") + previous + L.T("으로 되돌리는 중..."));
            PutBack(launcher);
            launcher.SetSetting("AutoUpdate", "0");
            try
            {
                Directory.Delete(BackupDir(launcher), true);
                if (Directory.Exists(ReadyDir(launcher)))
                    Directory.Delete(ReadyDir(launcher), true);
            }
            catch (Exception)
            {
            }
            Log(launcher, L.T("이전 버전 ") + previous + L.T("으로 되돌림 (자동 업데이트 끔)"));
            progress(L.T("되돌렸습니다: ") + previous);
            return Path.Combine(launcher.Dir, Launcher.ExeName);
        }
    }
}
