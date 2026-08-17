# -*- coding: utf-8 -*-
"""What JLCPCB costs, refuses, and rotates.

Every figure here is the fab house's, not a board's: it is the same on any
product this family orders, and it changes when JLCPCB changes, never when a
layout does. The per-board data that meets it -- which side is machine-placed,
which refs are hand-fitted, how big the outline may get -- lives in each
repository's kicad/board_config.json.

Python rather than a data file, because the data is code-shaped: LCSC_ROTATION
only means anything beside rotation_correction()'s fallback rule, and a table
split from the rule that reads it is a table that drifts from it. Nothing here
is edited by a product, so a second read-a-file mechanism would cost something
and serve nobody.

kicad/JLCPCB_DFM.md in each repository is the prose: what each rule costs, and
why the board is drawn the way it is. This is the machine-readable half.
"""

# --- Gate thresholds. Each maps to an option that costs money, or to an
#     Economic-PCBA eligibility requirement. -------------------------------------
MIN_VIA_DRILL = 0.30          # below this, the "min via hole" surcharge applies
MIN_VIA_DIA = 0.45            # hole >=0.3 needs dia >=0.4 to stay free; 0.45 adds margin
VIA_TO_MASK_OPENING = 0.35    # barrels closer than this to a mask opening cannot be tented
MIN_NPTH = 0.50               # smallest non-plated hole JLCPCB drills
FIDUCIAL_EDGE = 3.85          # JLCPCB's own fiducial-to-edge figure
EDGE_WARN = 1.0               # courtyard closer than this to the edge: warning only
BANNED_PACKAGES = ("_0402_", "_0201_", "_01005_")  # HASL floor is 0603

# --- Fab-output side: what the drill files may contain. ------------------------
PTH_MIN = 0.3       # below this the order moves to a paid drilling tier
NPTH_MIN = 0.5      # below this JLCPCB cannot drill at all

# --- Placement angles ---------------------------------------------------------
# JLC's part models and KiCad's footprints disagree about pin 1 for whole families,
# and per part for a few. The per-package default is +270 for the 2-row SO-family
# ICs and LQFPs (KiCad's pin-1 convention against JLC's); everything below is a
# part that does not follow its package.
LCSC_ROTATION = {
    "C5120959": 270,  # SGM2210-ADJ  SOT-23-5
    "C3445866": 180,  # OPA1677      SOT-23-5
    "C124463":  180,  # FDN360P      SOT-23
    "C20526":   180,  # MMBT3904     SOT-23
    "C7420333": 180,  # BAT54S       SOT-23
    "C107626":  180,  # TLP2361      SO-6
    "C2145":    180,  # MMBT5551     SOT-23
    "C7519":    270,  # USBLC6-2SC6  SOT-23-6  (JLC preview: 90 CW)
    "C84817":   270,  # MT3608       SOT-23-6  (JLC preview: 90 CW -- same package as USBLC6)
    "C21583":   180,  # HT7333       SOT-89-3  (JLC preview: 180)
    # SOT-23 corrections stay per-part rather than becoming a package default: an
    # unverified SOT-23 would otherwise be dragged along by a blanket rule.
}

PACKAGE_270_PREFIXES = ("SOIC-", "SSOP-", "LQFP-")

# CPL Mid X/Mid Y is normally the footprint origin, which for a well-drawn part is
# its centre. A few footprints put the origin off the pin pattern (a USB-C body
# outline is drawn forward of its solder tabs), so JLC -- which centres its model on
# the pins -- drops the part several mm off. For these, emit the geometric centre of
# the PAD FIELD instead; the weighted centroid over-corrects, dragged back by the big
# shield tabs. Keyed by LCSC. Do NOT use this for SOT-23/SOT-23-5, whose 0.2-0.3mm
# pad asymmetry is normal and whose origin is the correct placement point.
CENTROID_LCSC = {"C165948"}  # USB_C_Receptacle_HRO_TYPE-C-31-M-12


def rotation_correction(fpid, lcsc, extra_270=()):
    """Degrees to add to a footprint's angle for JLC's model.

    `extra_270` carries the product's own footprints that follow the 2-row rule
    without matching its name prefixes -- a relay drawn in a project library, say.
    It comes from board_config's `rotate_270`, because which library a footprint
    lives in is the product's business, not the fab house's.
    """
    if lcsc in LCSC_ROTATION:
        return LCSC_ROTATION[lcsc]
    name = fpid.split(":", 1)[-1]
    if name.startswith(PACKAGE_270_PREFIXES) or fpid in extra_270:
        return 270
    return 0
