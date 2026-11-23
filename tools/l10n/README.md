# English for the F2 menu and the launcher

The texts are written in Korean in the source. Each one stands in a call of the translation function:
`T_("...")` in `gt6_menu_ui.cpp` / `gt6_menu_engine.cpp`, `L.T("...")` in `launcher\*.cs`. While the language is English
the function looks the Korean text up in a table made by these tools; a text without an entry stays Korean.

| File | What it is |
| --- | --- |
| `koextract.py` | lists every string literal with Hangul in the given sources (`ko_all.tsv`) |
| `tr_prep.py` | numbers the different texts (`data\tr_keys.tsv`) and splits them into work files for translation |
| `tr_check.py` | checks a translation file: printf specifiers and their order, `##id` tails, `\n`, spaces, quotes |
| `trconsist.py` | checks that a menu item named in quotes carries that item's own English |
| `l10n_wrap.py` | wraps the literals in the sources (a run of adjacent C++ literals is one unit; `data\groups_*.tsv`) |
| `l10n_tables.py` | writes `gt6_menu_l10n.inc` and `launcher\L10nTable.cs` from `data\` |

`data\tr_out_A..D.tsv` are the translations (id, English), `data\tr_fix.tsv` holds corrections and the texts added later
(Korean, English) and wins over them.

## A new or changed text

1. Write it in Korean in the source, inside `T_("...")` or `L.T("...")`.
2. Add a line `Korean<TAB>English` to `data\tr_fix.tsv` and a line `file<TAB>0<TAB>Korean` to `data\groups_cpp.tsv`
   (menu) or `data\groups_cs.tsv` (launcher). Use the text exactly as it stands in the source, escapes included.
3. `python -X utf8 tools\l10n\l10n_tables.py tools\l10n\data tools\l10n\data\groups_cpp.tsv gt6_menu_l10n.inc tools\l10n\data\groups_cs.tsv launcher\L10nTable.cs tools\l10n\data\groups_trainer.tsv trainer\src\GT6Trainer\L10nTable.cs`
   (the last two: the external trainer, `L.T("...")` in `trainer\src\**\*.cs`, its language like the launcher's: `trainer\src\GT6Trainer\Lang.cs`)
4. Build.

The language: `MenuLang` in the menu's settings file (0 = by Windows, 1 = Korean, 2 = English), `Language` in
`launcher.ini` (`ko` / `en` / empty). Both are read at start; the launcher's choice writes the menu's key too.
For tests: `GT6_MENU_LANG=en`.
