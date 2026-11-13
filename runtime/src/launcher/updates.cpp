// The launcher's UPDATES page (updates.h). For now: what version this is.
#include "launcher/updates.h"

#include "overlay/option_rows.h"

#include "imgui.h"

namespace nx::launcher {

void updates_init(bool) {}

void page_updates(float) {
    namespace rows = overlay::rows;
    rows::begin("##updates", ImGui::GetContentRegionAvail().y);
    rows::section("Updates");
    rows::action("This version", NX_VERSION_STRING, "The version of SuperWonders you have.", true);
    rows::end();
}

}  // namespace nx::launcher
