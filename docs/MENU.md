# The recomp's options in Wonder's own menus

The quality bar puts every recomp option in the game's own pause and main menus, styled like the game: no launcher,
no overlay. This is how Wonder's menus are built, how the recomp adds to them, and the plan for each option.

## How Wonder builds its menus

Nearly all of it is data:

- **Layouts** are NintendoWare `bflyt` in `Layout/<Screen>.Nin_NX_NVN.blarc.zs` (a zstd SARC). A button is a part
  pane (`prt1`) using a shared button layout (`LCmnBtn`, `LCmnBtnIcon`...), e.g. the pause screen's `L_BtnContinue`,
  `L_RetryBtn`, `L_BtnExit` and the icon row `L_PlayerChangeBtn`, `L_OptionBtn`, `L_HelpBtn`, `L_AmiiboBtn`. The
  screen's In/Out animations move the containers (`N_Left`, `N_Right`), so a new pane under the same container comes
  and goes with the others.
- **Which panes are buttons, and what they do** is `Gyml/UIExtraScreenParam/<Screen>.game__UIExtraScreenParam.bgyml`
  in `Pack/Bootup.Nin_NX_NVN.pack.zs` (BYML v7). Each `ButtonInfoGenerateTable` entry names a pane and a
  `DecideInfoGenerateTable`: an enum type, a value, and the GameData variable the value is written to when the button
  is chosen. Pause: `game::sequence::PauseResult` into `PauseResult`. World-map menu: `game::sequence::MainMenuResult`
  into `MainMenuResult`.
- **What happens next** is an AINB graph (the pause one is
  `Sequence/_LocalModule_d9f012ee2b26.module.ainb` in `Pack/PostResident.pack.zs`): node `GetDecideButtonResult`
  (screen `Pause`, enum `game::sequence::PauseResult`) then one branch per value (`IsSelectExit` -> `AskIfExitCourse`
  window, `Option` -> `UISeq_Option`...).
- **Text** comes from `Mals/<lang>.Product.120.sarc.zs`, `LayoutMsgRef/<Layout>Ref.msbt`, label
  `<part pane>-<text pane>` (e.g. `L_BtnExit-T_BtnName` = "Exit Course").
- **The Settings screen** (pause/menu "Settings", layout `Option`, rows `LOptionParts` in a scrolling list) lists the
  option types `DashCtrlType`, `SquatCtrlType`, `SwingCtrlType`, `HDVibrationLevelType`, `BadgeSelectOpenType`,
  `TalkFlowerType`, `TalkFlowerLanguageType`, `TelopDisplayType`; their text is `GameMsg/Option.msbt`
  `<Type>_Subject` / `<Type>_SelectionNN`. The count (8) is a constant in the code (e.g. `main+0x1d89a10`
  `cmp w1, #7`), and the names are a pointer table at `main+0x366cdb8`.

The code side: `main` has no symbols for game code, but **enum types** are registered with three callbacks (from
string, to string, for each value) stored as function pointers (the registry is filled by `main+0x585d74`). The
recompiled code reaches them only through `nx::call`, which looks the target up in the module's function table, so
replacing a table entry (`module_at(base)->table[offset / 4]`) replaces the function for the whole game without
regenerating code. The same works for any virtual function or other function-pointer target; direct `bl` calls are
compiled to direct C++ calls and cannot be swapped this way.

## What is built: "Quit Game" in the pause menu

- `tools/wonder_menu_mod.py` writes the RomFS mod `private/wonder/mods/recomp-menu/` from the extracted RomFS:
  `L_QuitBtn` (a copy of the `L_BtnExit` part, one step under it; the icon row moves down 50), its button entry
  (`PauseResult` = `QuitToDesktop`), its label in every language, and the size table. Run it once after extracting
  the RomFS.
- `games/wonder/menu.cpp` swaps `PauseResult`'s from-string callback (`main+0x1d3415c`). The pause screen converts a
  button's `EnumValue` with it when the button is **chosen** (not when the screen opens or the cursor moves: checked
  in game), so the name `QuitToDesktop` reaching it is the moment to quit; the runtime exits the way closing the window
  does. The other names go to the game's own callback.

Two things the data side has to respect, both found the hard way (the game crashes at boot otherwise):

- **The resource size table** (`System/Resource/ResourceSizeTable.Product.120.rsizetable.zs`, RESTBL: CRC32 of the
  path -> size) sizes the buffer each file is loaded into: every archive that grows needs its entry grown as much.
- **A pack member's own entry** sizes what it is parsed into, not just its bytes: the pause screen's bgyml is 4660
  bytes but has 19136, and one more button overflows it (a failed allocation of 0x60 bytes at `main+0x939b6c`). The
  tool grows it by 8 KiB.

Tested headless (scripted input, a copy of a save in course 1-1): the button shows with the game's look and its
animation (see the pause menu with `NX_DUMP_FRAMES`), the cursor reaches it, Continue still resumes, and choosing it
exits with code 0. A new game from an empty save crashed at boot before this change too (unrelated).

## The plan for the other options

The same two halves, data (tool) and meaning (`menu.cpp`):

| Option | Where | How |
|---|---|---|
| Quit to desktop | Pause menu, under Exit Course; world-map menu next to Return to Title | Done (pause). The world-map menu: the same with `MainMenuResult` (`L_TitleBtn` as the model; its callbacks are registered by `main+0x585d74` like `PauseResult`'s). A confirmation (the game's `CmnWindow`, as Exit Course asks) would be kinder: not done. |
| Render scale, fullscreen | Settings screen, new rows after the game's 8 | Extend the option-type count: the name table (`main+0x366cdb8`) and the per-type selection counts (`main+0x2aa88b0`) are data the runtime can replace; the functions that compare with 7 have to be found and swapped (they are reached through the Settings screen's virtual functions). Row text: `GameMsg/Option.msbt` labels added by the tool. |
| Save anywhere, faster animations, fast-forward | Settings screen rows (On/Off) | As above; each row's value is read by the runtime feature it controls. |

Choices are kept in `<data root>/user/settings.txt` as `key=value` lines (`render_scale=1.5`), the file
`plat::setting` reads on PC after the `NX_<KEY>` environment variables (Android's `config.txt` is the same format).
A Settings row writes its key there when the player leaves the screen.

## Mods and updates

The mod replaces whole files: another mod that replaces `Layout/Pause...`, `Pack/Bootup...` or a `Mals` archive and
sorts before `recomp-menu` hides the button (and if it replaces only the layout or only the pack, the button table and
the layout disagree). `--romfs` takes another RomFS folder with all of the files, e.g. one with such a mod merged in.
The offset in `menu.cpp` is for 1.2.0 (a version with no function starting there stops at start with a message).
