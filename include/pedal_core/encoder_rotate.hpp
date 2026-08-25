#pragma once

// Which action an encoder rotation performs, given the current UI mode. Pure and
// dependency-free so the dispatch precedence is unit-testable (test_encoder_rotate)
// without standing up the whole encoder_ui collaborator graph.
//
// encoder_ui::update() switches on this so exactly one handler runs per detent — the
// modes are mutually exclusive by construction. Independent `if` handlers would let a
// single rotation fire two of them at once: the dev/VCA-cal pages are entered via SysEx
// from any page, so with cal entered there a rotation would both nudge the VCA offset AND
// navigate the page underneath. Resolving to a single action closes that off.
namespace encoder_ui_detail {

// Navigate = the ordinary UI, where encoder_ui::update() dispatches on its own mode:
// Play walks the preset list, Edit walks the parameter and settings pages.
// VcaOffset/None name a cal mode that claims the encoder for itself.
enum class RotateAction { None, VcaOffset, Navigate };

// Precedence: VCA-cal claims the encoder; dev-cal swallows rotation (the knobs do the work
// there); otherwise the UI navigates. The cal flags come first because dev/VCA-cal can be
// entered via SysEx from any page, so their claim has to outrank whatever was on screen.
inline RotateAction rotate_action(bool vca_cal, bool dev_cal)
{
    if (vca_cal) return RotateAction::VcaOffset;
    if (dev_cal) return RotateAction::None;
    return RotateAction::Navigate;
}

}  // namespace encoder_ui_detail
