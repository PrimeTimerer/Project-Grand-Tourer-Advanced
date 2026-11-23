// The launcher's language. Every Korean text of the launcher stands in L.T("..."): the Korean as written, or - while the
// language is English - its English out of L10nTable.cs (made by tools\l10n; a text that is not in the table stays
// Korean). launcher.ini [Settings] Language: "" = by Windows' display language (Korean on a Korean Windows, English
// everywhere else), "ko", "en". It is read once when the launcher starts; a change holds from the next start.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Text;
using System.Text.RegularExpressions;

namespace GT6Launcher
{
    public static partial class L
    {
        public static bool English;

        public static void Init(Launcher launcher)
        {
            string chosen = "";
            try
            {
                chosen = launcher.Setting("Language");
            }
            catch (Exception)
            {
            }
            English = chosen == "en" || (chosen != "ko" && CultureInfo.CurrentUICulture.TwoLetterISOLanguageName != "ko");
        }

        public static string T(string ko)
        {
            string en;
            return English && ko != null && Table.TryGetValue(ko, out en) ? en : ko;
        }

        // The game's own menu (F2) keeps its language in its settings file, key "MenuLang": 0 = by Windows, 1 = Korean,
        // 2 = English, read by the game when it starts. Written in place like "MenuFullHd" (MenuSettings): every other
        // key stays as the menu wrote it. Never while the game runs.
        static readonly Regex MenuLang = new Regex("\"MenuLang\"\\s*:\\s*(-?\\d+)");

        public static void SetMenuLanguage(string path, string chosen)
        {
            string value = chosen == "ko" ? "1" : chosen == "en" ? "2" : "0";
            string text = File.Exists(path) ? File.ReadAllText(path) : "", next;
            Match m = MenuLang.Match(text);
            int brace = text.IndexOf('{');
            if (m.Success)
                next = text.Substring(0, m.Groups[1].Index) + value + text.Substring(m.Groups[1].Index + m.Groups[1].Length);
            else if (brace < 0)
                next = "{\n  \"MenuLang\": " + value + "\n}\n";   // no file yet: the menu adds the rest with its defaults
            else
            {
                bool empty = text.Substring(brace + 1).TrimStart().StartsWith("}");
                next = text.Substring(0, brace + 1) + "\n  \"MenuLang\": " + value + (empty ? "\n" : ",") + text.Substring(brace + 1);
            }
            if (next == text)
                return;
            if (File.Exists(path))
                File.Copy(path, path + ".launcher.bak", true);
            string temp = path + ".launcher.tmp";
            File.WriteAllText(temp, next, new UTF8Encoding(false));
            if (File.Exists(path))
                File.Delete(path);
            File.Move(temp, path);
        }
    }
}
