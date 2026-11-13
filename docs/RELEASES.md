# Releases and SuperWonders Update

How a release is packed, what SuperWonders Update checks before it installs one, and how a repository owner signs
releases. Nothing here publishes anything: packing makes files, and putting them on GitHub is the owner's step.

## What a release is

The program ships ready made; the game is set up on the player's PC from their own files (the launcher runs
`tools/setup_game.py` from the release's `game-build/`). `tools/pack_release.py` makes one:

| Platform | File | Made with |
|---|---|---|
| Windows | `SuperWonders-v<version>-Windows-x64.zip` | `tools/pack_release.py --build <build> --out <dir> --toolchain <llvm-mingw> --python <CPython> --translator <published spirv_harness>` |
| Linux | `SuperWonders-v<version>-Linux-x64.AppImage` (and `.tar.gz`) | `TRANSLATOR=<published spirv_harness> tools/linux/appimage.sh` (an Ubuntu 22.04 container) |

The names stay the same from release to release: SuperWonders Update and launchers such as RecompLauncher find a
release's file by them. The version is `VERSION`; a version with `-alpha.N`, `-beta.N` or `-rc.N` is a pre-release,
offered only to players already on one. Every pack runs a privacy check and stops if a file names the machine that
packed it or holds anything of the game (below).

## Where a player's files are, and mods

Saves, settings (`pc-settings.ini`), the launcher's folders (`launcher.ini`), the log and the game set up from the
player's files are in the data folder: `data/` next to the program on Windows (and in a portable Linux folder),
`$XDG_DATA_HOME/superwonders` (`~/.local/share/superwonders`) for the AppImage. The launcher's Game files page opens it.

**Mods** go in its `mods/` folder, one folder each, holding a `romfs` folder (`mods/<name>/romfs/`, or the layout
Switch mods come in, `mods/<name>/contents/<title id>/romfs/`). Their files go over the game's at start: in name order,
the first mod with a file wins, and a folder whose name starts with `-` is off (`runtime/src/hle_fs.cpp`). The launcher's
Mods page lists them, turns each on or off, and opens the folder.

## What SuperWonders Update checks

It looks at the GitHub releases of the repository in the release's `update.ini` (`repo=owner/name`; empty, the
default, means no checks at all). Before it changes anything:

- **Always:** the file must have GitHub's SHA-256 (GitHub records one for every file uploaded to a release), and the
  download must match it. Every file in it must match the release's own list (`superwonders-manifest.txt`).
- **With a release key:** the file must also have a signature, `<file>.sig`, made with the repository owner's key.
  A release without one is not offered; one whose signature doesn't check out is not installed. A malformed key
  in `update.ini` stops updates altogether rather than falling back to the checksum alone.

The key is the owner's: SuperWonders ships none, and none is in this repository.

## Signing releases (for a repository owner)

Signing is optional. Without it, an update is only as trustworthy as the GitHub repository it comes from; with it,
someone who could upload files to the releases (but doesn't have the key) still can't get players to install them.

1. **Make the key, once** (OpenSSL), and keep it somewhere safe outside any repository. Whoever has it can sign
   releases; if it is lost, players need a release with a new public key installed by hand.

       openssl ecparam -name prime256v1 -genkey -noout -out superwonders-release-key.pem

2. **Its public half**, 128 hex digits, is what releases carry:

       python tools/sign_update.py --public superwonders-release-key.pem

3. **Pack with both**: `update.ini` gets the repository and the public key, and the archive gets its signature:

       python tools/pack_release.py ... --repo owner/name --pubkey <the 128 hex digits> --sign superwonders-release-key.pem

   (Instead of `update.ini`, the public key can be built into the program: `-DSUPERWONDERS_UPDATE_PUBKEY=<hex>`.)
   For an AppImage, sign the finished file the same way: `python tools/sign_update.py <file>.AppImage <key.pem>`.

4. **Upload** the archive (or AppImage) and its `.sig` to the GitHub release, side by side, under the same names.

Players on a release whose `update.ini` has no key only check the SHA-256. The first release with a key starts the
signature check for the releases after it.

## The privacy check

`tools/pack_release.py` refuses to pack (and `--check-only <folder>` reports) any file whose name or contents, in
UTF-8 or UTF-16, has:

- the packing PC's home folder (where a path starts) or user folder;
- `SuperWonders-private` or `OneDrive` (Windows' own `FOLDERID_OneDrive` aside);
- an e-mail address, in SuperWonders' own files (the third-party parts, Python's library, the compiler, the .NET
  runtime, are full of their authors' public addresses);
- anything listed in `tools/privacy-extra.txt`, one string per line: put the packer's own names, mail and folders
  there. It is in `.gitignore`, so it is never committed;
- a game file (`main`, `sdk`, NSO, NCA, NSP, XCI, the RomFS's archive types, `romfs/` or `exefs/` folders).

It also removes Python's bytecode caches first: a `.pyc` keeps the path it was compiled at.
