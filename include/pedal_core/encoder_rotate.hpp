#pragma once

// Which action an encoder rotation performs, given the current UI mode. Pure and
// dependency-free so the dispatch precedence is unit-testable (test_encoder_rotate)
// without standing up the whole encoder_ui collaborator graph.
//
// encoder_ui::update() switches on this so exactly one handler runs per detent — the
// modes are mutually exclusive by construction. Independent `if` handlers would let a
// single rotation fire two of them at once: the dev/VCA-cal pages are entered via SysEx
// from any page (including while the Global overlay is open), so with cal entered there a
// rotation would both nudge the VCA offset AND scroll the overlay pages. Resolving to a
// single action closes that off.
namespace encoder_ui_detail {

// GlobalPage = scroll through the Global overlay's pages (the overlay is knob-edited, so
// the encoder only navigates it). Normal = the main param screen, where rotation acts on
// whichever header widget is highlighted (page / algorithm / preset) — encoder_ui::update()
// resolves that by its focus state. VcaOffset/None name a cal mode that claims the encoder.
enum class RotateAction { None, VcaOffset, GlobalPage, Normal };

// Precedence: VCA-cal claims the encoder; dev-cal swallows rotation (the knobs do the work
// there); an open Global overlay scrolls its pages; otherwise the normal param screen
// dispatches by its highlighted focus. The cal flags are checked before `on_global` because
// dev/VCA-cal can be entered (via SysEx) while the overlay is open.
inline RotateAction rotate_action(bool vca_cal, bool dev_cal, bool on_global)
{
    if (vca_cal)   return RotateAction::VcaOffset;
    if (dev_cal)   return RotateAction::None;
    if (on_global) return RotateAction::GlobalPage;
    return RotateAction::Normal;
}

}  // namespace encoder_ui_detail
