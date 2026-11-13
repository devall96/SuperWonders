// The launcher's UPDATES page and the update check (launcher/updates.cpp; SuperWonders Update does the installing).
#pragma once

namespace nx::launcher {

void updates_init(bool config_mode);  // once, when the launcher shows: starts the background check if it's on
void page_updates(float k);           // the page, inside the launcher's page child

}  // namespace nx::launcher
