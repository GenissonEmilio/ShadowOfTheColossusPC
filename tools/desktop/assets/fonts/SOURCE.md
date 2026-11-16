# Japanese launcher font

`SotCLauncherJapanese-Regular.otf` is a static, renamed subset of
[Noto Sans CJK JP Regular 2.004](https://github.com/notofonts/noto-cjk/blob/Sans2.004/Sans/OTF/Japanese/NotoSansCJKjp-Regular.otf).
It includes the Japanese launcher catalog and kana. The included OFL notice
applies to the font.

To regenerate, subset the original OTF with fontTools using the Unicode
characters in the Japanese column of `../launcher-languages.tsv`,
the language name 日本語, and U+3041–U+30FF. Keep the original copyright
and license name records.
Set the family to `SotC Launcher Japanese` and the PostScript name to
`SotCLauncherJapanese-Regular` in both the OpenType name table and CFF.
