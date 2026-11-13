// Settings pages in two panes, after Savage Wind's option rows (github.com/Revivedx/SavageWindRecomp,
// runtime/ui/option_rows.h, MIT): option rows on the left - an uppercase label, the value on the right (brighter when
// it isn't the default) - and the selected (controller / keyboard) or hovered (mouse) row's description on the right,
// instead of tooltips. A / Enter / a click changes a row (toggle, next value), left / right step it.
//
//   rows::begin("##video", height);
//   rows::section("Display");
//   if (rows::toggle("Fullscreen", &s.fullscreen, false, "Fill the whole screen.")) ...
//   rows::end();
//
// Here the rows also ease their highlight in and out, and the description fades when it changes.
#pragma once

namespace nx::overlay::rows {

void begin(const char* id, float height);
void section(const char* title);
bool toggle(const char* label, bool* v, bool def, const char* desc);
// wrap: past the last value back to the first (and the other way); false: stops at the ends
bool choice(const char* label, int* v, const char* const* names, int n, int def, const char* desc, bool wrap = true);
// whole numbers lo..hi in steps of `step` (holding left / right speeds up); fmt formats the value (e.g. "%d%%")
bool integer(const char* label, int* v, int lo, int hi, int step, int def, const char* fmt, const char* desc);
// an action row with a value on the right; true when activated. upper: the label in capitals.
bool action(const char* label, const char* value, const char* desc, bool upper = true, bool highlight = false);
// the next row is greyed out and can't be changed (still selectable, so its description shows)
void disable_next();
// rows after this sit one step in (+1) or back out (-1): options that belong to the row above
void indent(int step);
// a note under the description of every row of this page (e.g. what needs a restart)
void page_note(const char* text);
void end();
// the next page's first row takes the controller's cursor
void focus_first();

}  // namespace nx::overlay::rows
