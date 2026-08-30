# pedal-core

The library's domain language: the words this repo uses for its own concepts, defined once so
that code, docs, issues and conversations all use the same one. Definitions say what a thing
**is**, not how it is built — the code is where the implementation lives.

A term earns a place here when it is genuinely the library's own: a concept every product in the
family shares, and none of them owns. A term that only makes sense for one pedal belongs in that
pedal's glossary. *Edit Buffer*, *Slot* and *MIDI Jack* are the multi-effect's, and are defined
[there](https://github.com/DarrenInwood/killroom-analog-multi-effect/blob/main/CONTEXT.md) —
*MIDI Jack* being the port this library routes, whatever connector a product fits to it.

## Language

### Controls

**Action**:
What a switch does when a foot reaches it — bypass, tap, preset up, a held freeze. One
vocabulary, shared by the two switches on the front panel and by both contacts of the external
jack, so a label, a stored byte and a SysEx payload mean the same thing wherever they came from.
Each action belongs on a **gesture**: some can only be pressed, some can only be held.
_Avoid_: function, command, assignment (the assignment is the pairing of a switch with an action)

**Gesture**:
What a foot did to a switch, as distinct from what the switch is assigned to do: a press,
emitted on the release where no hold fired; a hold, fired while the switch is still down; and
the release that closes a hold. The same grammar for the panel switches and the jack.
_Avoid_: event (an event is the pairing of a switch and a gesture), click, tap (a tap is an
action, not a gesture)

### The screen

**The performance screen**:
What the pedal shows while it is being played — the algorithm, the preset, the knob columns and
what the footswitches currently do. One value, pushed whole. Every other screen is a product's
own, drawn through its hooks.
_Avoid_: home screen, main screen, the normal screen

**Frame pacing**:
The decision of whether the screen redraws on this pass and what belongs on the frame when it
does: the idle cap, the faster cadence while something animates, the dwell a transient gets, and
the boot splash and fault hold that own the screen outright.
_Avoid_: refresh rate, frame rate, tick (pacing decides *whether* a tick draws)

**Transient**:
Something that takes part of the screen for a while and then hands it back — the parameter focus
panel a knob edit unrolls, a message banner, the save animation. Distinct from a screen, which
stays until something changes it.
_Avoid_: overlay (the drawing is an overlay; the thing is a transient), popup, toast

**The slide**:
The transition between two screens: the frame being left and the frame arriving, composited
column by column until the arriving one has crossed. A **band** slide composites only a range
of pixel rows and leaves the rest of the screen to the ordinary redraw, so a change confined to
one region of the layout reads as that region moving rather than the whole display.
_Avoid_: wipe, scroll, animation (nothing scrolls; two captured frames are composited)

**The host display seam**:
The stubbed hardware a host program compiles the real display stack behind — no SPI, no pins,
and a clock it can advance. What lets the firmware's own framebuffer be photographed or asserted
on a desktop, with no second implementation of the layout anywhere.
_Avoid_: mock display, display harness, simulator (nothing is simulated; the driver is real)

**The shoot**:
Running such a host program to photograph the framebuffer for the documentation. A screen the
pedal cannot draw is a screen that cannot appear in the docs. The multi-effect's word.
_Avoid_: screenshot generation, render pass

### Tempo

**The tempo layer**:
Everything that decides and shows the pedal's tempo: the tap and clock engine, the arbitration
between a tapped tempo and an incoming one, the LED that flashes the beat, and the clock the
pedal generates for the pedals below it in the chain. The tempo-synced parameter remains the
single source of truth for the tempo itself.
_Avoid_: tempo engine (that is the tap/clock part alone), tempo system

### The wire

**The global block**:
Everything the pedal holds that is not part of a preset — the MIDI channel and routing, the
noise gate, the external jack's assignments, the bypass state. Carried as a list of records, so
a product sends only what it has and a reader skips what it does not know.
_Avoid_: settings, globals, config (the pedal's own configuration is a different thing from the
frame that carries it)

**The image descriptor**:
What an application image says about itself so the bootloader can decide whether to run it: a
magic marker, the image's total length, which product it is for, and its version — followed by a
CRC32 trailer over everything preceding. An image that fails it does not crash the pedal; it
leaves it in DFU.
_Avoid_: header, manifest, firmware header

### MIDI

**The MIDI Out router**:
What arbitrates the MIDI Out jack between the three things that want it: the inbound stream
being echoed, the other transport being cross-routed, and the pedal's own traffic. It exists
because only System Real-Time bytes may appear inside another message, so everything else has to
leave the jack whole — a message contending with a frame already streaming waits rather than
splicing into it.
_Avoid_: DIN Out router, MIDI thru, merger, mixer

**Carries-policy**:
Whether a given source's traffic reaches a given **port** at all, for the current routing — the
MIDI jack the router writes, and the USB port a caller writes on the router's answer. One
question with one answer, asked before anything is written, and where the rules that surprise
people live: Active Sensing is dropped on every setting and on both ports, the clock family rides
its own switch, and a pedal generating its own clock drops the one arriving. The clock rules are
the jack's alone — a host watching an inbound clock over USB is not a second clock on a chain.

What it is *not*: whether the pedal's own traffic is worth sending in the first place. The
*tx_params* switch separates an unasked-for echo of a knob the player moved from a message the
product meant to send or a reply a host asked for, and the source those arrive under cannot tell
them apart — so it stays with the responder rather than joining this table.
_Avoid_: routing rules, filter
