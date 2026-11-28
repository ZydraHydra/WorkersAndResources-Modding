"workshopitem"
{
    "appid"           "784150"
    "publishedfileid" "{PUBLISHED_FILE_ID}"
    "contentfolder"   "{CONTENT_DIR}"
    "previewfile"     "{PREVIEW_FILE}"
    "visibility"      "{VISIBILITY}"
    "title"           "{WORKSHOP_TITLE}"
    "description"     "[h1]{INTRO_TITLE}[/h1]{INTRO_BODY}

{EDITION_HEADER}
Source code & releases: [url=https://github.com/ZydraHydra/WorkersAndResources-Modding]ZydraHydra GitHub[/url]

[b]Alpha Field Dispatch:[/b] Treat this mod as an early [b]Alpha[/b] release. It has been verified on WRSR `1.2.0.8` and `1.2.0.9` and needs broader field testing on large late-game republics. Back up your saves first and report any issues on GitHub!

[h1]Wait - What is a DLL Mod? (Step-by-Step Setup)[/h1]Ordinary Steam Workshop mods only add buildings or vehicles. Because [b]{MOD_NAME}[/b] adds an in-game [b]{HOTKEY}[/b] overlay and hooks into game logic, it needs a [b]Mod Loader[/b] alongside the game. Simply clicking Play in Steam will [b]not[/b] load DLL mods on its own.

Both [b]Glorious Republic Mod Loader (GRML)[/b] and [b]TesmioLoader (v0.4.6+)[/b] automatically find subscribed Steam Workshop DLL mods - [b]no manual file copying required[/b]:

[b]Option A - Glorious Republic Mod Loader (GRML - Recommended):[/b]
[list=1]
[*] Subscribe to this mod ([b]{MOD_NAME}[/b]) - Workshop ID [i]{PUBLISHED_FILE_ID}[/i].
[*] Subscribe to [url=https://steamcommunity.com/sharedfiles/filedetails/?id=3846999477]Glorious Republic Mod Loader (GRML)[/url] ([url=https://github.com/ZydraHydra/GloriousRepublicModLoader]GitHub[/url]).
[*] Open [b]GloriousRepublicModLoader.exe[/b] from:
[code]<Your Steam Library>\steamapps\workshop\content\784150\3846999477\GloriousRepublicModLoader.exe[/code]
(Tip: create a Desktop shortcut or add it to Steam as a Non-Steam Game.)
[*] Tick [b]{MOD_NAME}[/b], click [b]Launch[/b], and press [b]{HOTKEY}[/b] in-game! (GRML provides the unified F9 Mod Settings hub, OTA offset updates, and automatic savegame backups.)
[/list]
[b]Option B - TesmioLoader (v0.4.6+ - Legacy Alternative):[/b]
[list=1]
[*] Subscribe to this mod ([b]{MOD_NAME}[/b]) - Workshop ID [i]{PUBLISHED_FILE_ID}[/i].
[*] Download & extract [url=https://github.com/MaxLegend/TesmioLoader/releases]TesmioLoader (v0.4.6+)[/url] into your WRSR game directory ([url=https://maxlegend.github.io/TesmioLoaderDocs/#/getting-started]Docs[/url]).
[*] Run [b]tesmiolauncher.exe[/b]. Since v0.4, TesmioLoader automatically scans your Steam Workshop folder and lists subscribed DLL plugins (if your Workshop is in a separate Steam library, set it in the launcher / [i]workshop_path[/i]).
[*] Tick [b]{MOD_NAME}[/b], launch WRSR, and press [b]{HOTKEY}[/b] in-game!
[/list]
(Manual/Non-Steam only: this Workshop item's DLL lives at [code]<Your Steam Library>\steamapps\workshop\content\784150\{PUBLISHED_FILE_ID}\plugins\{MOD_NAME}.dll[/code] -> copy into [code]<Game Dir>\plugins\[/code] if you aren't using Steam Workshop scanning.)

{OPERATING_SECTION}

{EXAMPLES_SECTION}

{GUARANTEES_SECTION}

{COMPANION_MODS_SECTION}{CHANGELOG_SECTION}"
    "language"        "english"
    "changenote"      "{CHANGENOTE}"
}
