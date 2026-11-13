# The recomp's options in Wonder's own menus

Some recomp entries belong in the game's own menus, styled like the game (the F1 overlay keeps the rest). This is how
Wonder's menus are built, how the recomp adds to them, and the plan for each option. First written in the
josiahg2502-glitch fork for 1.2.0; the offsets here are 1.0.0's.

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
- **Text** comes from `Mals/<lang>.Product.<v>.sarc.zs`, `LayoutMsgRef/<Layout>Ref.msbt`, label
  `<part pane>-<text pane>` (e.g. `L_BtnExit-T_BtnName` = "Exit Course").
- **The Settings screen** (pause/menu "Settings", layout `Option`, rows `LOptionParts` in a scrolling list) lists the
  option types `DashCtrlType`, `SquatCtrlType`, `SwingCtrlType`, `HDVibrationLevelType`, `BadgeSelectOpenType`,
  `TalkFlowerType`, `TalkFlowerLanguageType`, `TelopDisplayType`; their text is `GameMsg/Option.msbt`
  `<Type>_Subject` / `<Type>_SelectionNN`. The count (8) is a constant in the code (e.g. `main+0x1d89a10`
  `cmp w1, #7`), and the names are a pointer table at `main+0x366cdb8`.

The code side: `main` has no symbols for game code, but **enum types** are registered with three callbacks (from
string, to string, for each value) stored as function pointers (1.0.0: the registry is filled by `main+0x581d38`;
1.2.0: `main+0x585d74`). The recompiled code reaches them only through `nx::call`, which looks the target up in the
module's function table, so replacing a table entry (`module_at(base)->table[offset / 4]`) replaces the function for
the whole game without regenerating code. The same works for any virtual function or other function-pointer target;
direct `bl` calls are compiled to direct C++ calls and cannot be swapped this way.

- **The title screen** (layout `Title`, screen param with no buttons) says "Press L + R to Start" (`T_PressStart`)
  but takes any button, Minus too. The wait is the AINB node `WaitPressKeyInTitle`; its update (1.0.0
  `main+0x1bfcddc`, a virtual function) runs every frame until the title's key state (`*(main+0x3630a98)` + 0x4c)
  reads 3. The title layout stays up behind the user and character screens after it; the game hides
  `T_PressStart` there by its alpha (animation `DecideKey`), not by hiding `N_PressStart`.
- **Button glyphs in text**: `GameMsg/Key.msbt` names the font's private-use glyphs (`Minus` = U+E0F0, `Plus` =
  U+E0EF, `Key_L` = U+E0E4...); a message can use them directly.

## What is built: "Quit Game" in the pause menu and on the title screen

- `tools/wonder_menu_mod.py` writes the RomFS mod (`<data root>/mods/recomp-menu/` is the place for it) from the
  extracted RomFS:
  - **Pause menu:** `L_QuitBtn`, a copy of the `L_BtnExit` part one step under it (the icon row moves down 50), its
    button entry (`PauseResult` = `QuitToDesktop`) and label.
  - **Title screen:** a "(Minus glyph) Quit Game" line in the bottom right corner, a copy of the "Press L + R to
    Start" text pane, as a child of each of the two such panes (`T_QuitGame` under `T_PressStart`, `T_QuitGameUS`
    under `T_PressStart_USEU`; the game shows one by region), labels in `TitleRef.msbt`. The parents get the
    "influenced alpha" flag. As children the lines go when the game hides "Press L + R" (as a sibling the line stayed
    visible behind the character screen; under `T_PressStart` alone it never showed in the US region).
  - The labels in every language the game has, and the size table. Run it once after extracting the RomFS (the
    version in the file names, `Product.100` or `Product.120`, is found by itself). It uses the `zstandard` module
    when it is installed, otherwise the system's libzstd.
- `games/wonder/menu.cpp` (1.0.0 offsets; each target's first instructions are checked, and a version with other code
  there keeps the game's own function and logs it):
  - swaps `PauseResult`'s from-string callback (`main+0x1c0bd9c`). The pause screen converts a button's `EnumValue`
    with it when the button is **chosen** (not when the screen opens or the cursor moves), so the name
    `QuitToDesktop` reaching it is the moment to quit. The other names go to the game's own callback.
  - swaps `WaitPressKeyInTitle`'s update: while Minus is held on its own, the game's update is held back (the title
    keeps waiting) and 0.4 s of it quits; let go sooner and the title starts as it always did. Minus + Plus, the
    overlay's combo, never reaches the game.
  - Quitting is `plat::exit_process(0)`, what closing the window does: the pipeline cache is saved and the log
    flushed. Progress since the last save is not saved (as when the window is closed). `NX_QUIT_GAME=0` leaves the
    menus as the game has them.

Two things the data side has to respect, both found the hard way (the game crashes at boot otherwise):

- **The resource size table** (`System/Resource/ResourceSizeTable.Product.<v>.rsizetable.zs`, RESTBL: CRC32 of the
  path -> size) sizes the buffer each file is loaded into: every archive that grows needs its entry grown as much.
- **A pack member's own entry** sizes what it is parsed into, not just its bytes: the pause screen's bgyml is 4660
  bytes (1.2.0) but has 19136, and one more button overflows it (a failed allocation of 0x60 bytes). The tool grows it
  by 8 KiB.

Ported from the josiahg2502-glitch fork (commit b2e9708, for 1.2.0); the title screen is new here.

**Not done: the world map's menu.** The fork (b17c70a) also adds Quit Game there (`MainMenuResult`, from-string
callback `main+0x1c0beec` at 1.0.0). At 1.0.0 the button draws but the menu's cursor never stops on it, checked with
frames: inserted after Return to Title in the button table, Down no longer left the top row at all; appended last, the
cursor went Save, Return to Title, then Settings, skipping it, wherever it was placed (beside Return to Title, in a
row with Save, or in a column under it). The layout has no button group, so the menu's cursor order comes from its
code. From the world map, Return to Title then Minus on the title quits.

## The plan for the other options

The same two halves, data (tool) and meaning (`menu.cpp`):

| Option | Where | How |
|---|---|---|
| Quit to desktop | Pause menu, title screen | Done. The world map's menu: see above. A confirmation (the game's `CmnWindow`, as Exit Course asks) would be kinder: not done. |
| Render scale, fullscreen | Settings screen, new rows after the game's 8 | Extend the option-type count: the name table (`main+0x366cdb8`) and the per-type selection counts (`main+0x2aa88b0`) are data the runtime can replace; the functions that compare with 7 have to be found and swapped (they are reached through the Settings screen's virtual functions). Row text: `GameMsg/Option.msbt` labels added by the tool. (1.2.0 offsets.) |
| Save anywhere, faster animations, fast-forward | Settings screen rows (On/Off) | As above; each row's value is read by the runtime feature it controls. |

Where the choices would be kept is open: the PC build keeps its settings in `pc-settings.ini` today.

## Mods and updates

The mod replaces whole files: another mod that replaces `Layout/Pause...`, `Layout/Title...`, `Pack/Bootup...` or a
`Mals` archive and sorts before `recomp-menu` hides the entries (and if it replaces only the layout or only the pack,
the button table and the layout disagree). `--romfs` takes another RomFS folder with all of the files, e.g. one with
such a mod merged in. The offsets in `menu.cpp` are for 1.0.0 (a version with other code there runs without the
entries, with a message).
