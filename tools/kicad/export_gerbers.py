# -*- coding: utf-8 -*-
"""Build a JLCPCB-ready gerber + drill zip, from the board that is actually on disk.

    "C:/Program Files/KiCad/10.0/bin/python.exe" export_gerbers.py <board.kicad_pcb> <out.zip>

This exists because the gerber step was the ONE manual step in the fab package, and it drifted: the
zip that was uploaded came from the Fabrication Toolkit plugin while `fab/gerbers/` held a different,
older export, so the copper being fabricated was three weeks behind the CPL being populated. A
script that reads the same .kicad_pcb everything else reads cannot drift.

Layer set is what JLCPCB wants for a 4-layer board and no more: the four coppers, both masks, both
silks, both pastes, and the outline. Fab and courtyard layers are deliberately absent -- they are
documentation, and shipping them invites the board house to interpret them.

  --subtract-soldermask   silk is clipped where it would land on an opening, so no silk on a pad
  --no-protel-ext         plain .gbr names; JLCPCB reads them and they are unambiguous
  --excellon-separate-th  PTH and NPTH in their own files, which is how JLCPCB expects them
  --check-zones           refill before plotting, so the copper in the zip matches the copper the
                          stranded-pour gate signed off. A stale fill is exactly how a pour that
                          passed a check can still ship open.
"""
import os
import subprocess
import sys
import tempfile
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import board_config
import kicad_env

# Everything that is not copper. Fab and courtyard layers are deliberately absent:
# they are documentation, and shipping them invites the board house to read them.
NON_COPPER = "F.Mask,B.Mask,F.SilkS,B.SilkS,F.Paste,B.Paste,Edge.Cuts"


def layer_string(layers):
    """The --layers argument for a board of `layers` copper layers.

    Inner layers are named In1..In(n-2) between the two outer ones, which is the
    order kicad-cli plots them in and the order the fab expects the stackup.
    """
    if layers < 2 or layers % 2:
        sys.exit(f"export_gerbers: {layers} copper layers is not a stackup KiCad plots")
    inner = ",".join(f"In{i}.Cu" for i in range(1, layers - 1))
    cu = "F.Cu," + (inner + "," if inner else "") + "B.Cu"
    return cu + "," + NON_COPPER


def run(args):
    r = subprocess.run(args, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"FAILED: {' '.join(args)}\n{r.stdout}\n{r.stderr}")
    return r.stdout


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--layers")]
    override = next((a for a in sys.argv[1:] if a.startswith("--layers=")), None)
    board, out_zip = args[0], args[1]
    if not os.path.exists(board):
        sys.exit(f"no such board: {board}")

    # The layer count comes from the config rather than from the board, so this
    # keeps running under any Python: it only shells kicad-cli, and importing
    # pcbnew just to call GetCopperLayerCount() would change that.
    if override:
        layers = int(override.split("=", 1)[1])
    else:
        layers = int(board_config.load().board_for_path(board)["layers"])
    LAYERS = layer_string(layers)
    KCLI = kicad_env.kicad_cli()
    os.makedirs(os.path.dirname(os.path.abspath(out_zip)), exist_ok=True)

    with tempfile.TemporaryDirectory() as tmp:
        run([KCLI, "pcb", "export", "gerbers", "-o", tmp, "--layers", LAYERS,
             "--subtract-soldermask", "--no-protel-ext", "--check-zones", board])
        run([KCLI, "pcb", "export", "drill", "-o", tmp, "--format", "excellon",
             "--excellon-separate-th", "--generate-map", "--excellon-units", "mm", board])

        files = sorted(f for f in os.listdir(tmp)
                       if os.path.isfile(os.path.join(tmp, f)))
        if not files:
            sys.exit("kicad-cli produced no files")
        with zipfile.ZipFile(out_zip, "w", zipfile.ZIP_DEFLATED) as z:
            for f in files:
                z.write(os.path.join(tmp, f), f)

    size = os.path.getsize(out_zip)
    print(f"{out_zip}  ({size/1024:.0f} kB, {len(files)} file(s))")
    for f in files:
        print(f"   {f}")

    # A 4-layer board must ship four copper files. Silently plotting two is the kind of thing
    # nobody notices until the boards arrive with the inner layers blank.
    cu = [f for f in files if any(k in f for k in ("F_Cu", "In1_Cu", "In2_Cu", "B_Cu"))]
    if len(cu) != 4:
        sys.exit(f"expected 4 copper layers, got {len(cu)}: {cu}")
    print("GERBERS: 4 copper layers, masks, silks, pastes, outline and drill files present")
    return 0


if __name__ == "__main__":
    sys.exit(main())
