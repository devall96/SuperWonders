// Updating a Linux AppImage (launcher/update_appimage.c), from Savage Wind (github.com/Revivedx/SavageWindRecomp, MIT:
// runtime/update_appimage.*): the new file replaces the old one in place, under the same name, so shortcuts and
// Steam entries keep working. Linux allows that while the program runs, so no separate updater is needed. Downloads
// with the system's curl, checks the file against GitHub's SHA-256 (and its signature when a key is built in), starts
// it once to see that it runs, and keeps the previous one as <file>.old until the next start.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "update_core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char code[32];    // stable, for tests
    char title[200];  // what happened
    char what[900];   // what it means and what to do
} UpdateProblem;

#define UPDATE_HEALTH_EXIT 73  // wonder --update-health: it starts, so its libraries are all there

// The running AppImage's file, NULL when not running from one (or $APPIMAGE is another program's).
const char *appimage_path(void);
// Downloads r (r->digest required), checks it, starts it once (--update-health), and replaces the AppImage, keeping
// the old one as <file>.old. pubkey_hex: the release key (128 hex digits) or NULL (no signature needed).
// *progress goes 0..1 as it works.
bool appimage_install(const char *work, const UcRelease *r, const char *pubkey_hex, volatile float *progress,
                      UpdateProblem *p);
bool appimage_has_old(void);
void appimage_remove_old(void);
// Replaces this process with the AppImage on disk, opening the launcher (--config). Returns only if it couldn't.
void appimage_relaunch(UpdateProblem *p);

#ifdef __cplusplus
}
#endif
