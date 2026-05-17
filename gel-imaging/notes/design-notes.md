# Gel Imaging Station — Design Notes

## Overview
Integrated unit combining gel electrophoresis box, blue LED transilluminator, and Raspberry Pi camera timelapse system in a single resin-printed enclosure. Goal: lightproof, compact, open-source, built from scratch.

---

## Key Design Decisions

### Enclosure
- Unified resin-printed enclosure (single build, not separate units bolted together)
- External dimensions: ~107 × 77 × 150mm (gel cavity + walls + hood + lid)
- Internal gel cavity: **100 × 70mm** (locked — do not change without reprinting)
- Lightproof hood — no ambient light leak during imaging
- Hinged lid for easy gel loading/removal
- Material: black or grey pigmented resin (MSLA) — clear resin transmits ambient light, unacceptable
- All M3 fasteners use brass heat-set inserts (press in with soldering iron)

### Transilluminator
- Blue LED transilluminator tray (~470nm), fits below gel cavity in recessed pocket
- LED strip (5mm) + resin diffuser layer (3mm) + 4mm clearance = 12mm tray height
- Tray is removable/swappable via snap-fit + PTFE tape perimeter seal (see Buffer Containment below)
- Uniform illumination across 100 × 70mm cavity is the design priority

### Imaging System
- **Raspberry Pi 4B** + **Pi Camera Module v2** (8MP, Sony IMX219)
- Camera mounted in lid, fixed focal distance: **80mm** from lens to gel surface
- Pi v2 manual focus mod: unscrew lens 2–3 turns from factory position → shifts focus from ~50cm to ~80mm. Well-documented, reversible, no tools needed.
- Fixed focal mount — no adjustments after initial setup
- **Amber filter** slide-slot integrated into lid (filter slides in from front face, 2mm slot)
- Timelapse: `firmware/timelapse.py` — captures every 30–60s, stitches to MP4 via ffmpeg
- Pi cable routed through hood wall channel to keep ribbon outside gel zone

### Gel Combs
- Parametric design (`cad/gel_comb.scad`) — tooth width and spacing as variables
- **8-tooth comb:** tooth_width=2.0mm, tooth_gap=3.0mm → ~15µL wells at 3mm depth
- **16-tooth comb:** tooth_width=1.2mm, tooth_gap=2.0mm → ~8µL wells (MSLA minimum feature)
- Registration slots on comb handle ends engage 1.7mm channels in enclosure side walls
- Print orientation: teeth DOWN; supports on handle flat face only
- Post-cure: IPA 2min, UV 60s — do not over-cure (embrittlement at tooth tips)

---

## Design Decisions — Resolved

### 1. LED Driver: Resistor only (no IC)

**Decision:** Simple series resistor from regulated 5V USB supply. No constant-current IC.

**Rationale:** The transilluminator is a uniform illumination source, not a precision photometric instrument. SYBR Safe / GelGreen imaging tolerates ≤5% intensity drift across temperature — well within what a resistor-limited supply delivers from a regulated USB port. Constant-current ICs (LM3409, AL8805) are reserved for the fluorimeter, where excitation intensity stability directly affects measurement accuracy. For the gel box, calculate R_series = (5V − Vf_total) / I_target for each LED string. Simple, debuggable, no PCB needed.

### 2. Pi Camera: v2 with manual focus mod

**Decision:** Pi Camera Module v2 ($25) with lens unscrewed 2–3 turns. Upgrade to HQ camera later if OpenCV band detection is added.

**Rationale:** The Pi v2 lens is factory-focused at ~50cm. At 80mm working distance, the image is blurred. Unscrewing the lens 2–3 turns (no tools, friction-fit) re-focuses to ~80mm — well-documented mod, reversible, takes 30 seconds. Field of view at 80mm: ~75 × 56mm, which crops 25mm on the long axis of the 100mm gel cavity. Acceptable for most gels; raise the camera mount 10mm if the full gel length is needed. HQ camera ($50 + lens $20–30) offers more control but isn't justified until band-detection software warrants it.

### 3. Buffer Containment: Mechanical lip + PTFE tape

**Decision:** 1.5mm raised perimeter lip on LED tray nests into a matching channel in the enclosure floor. PTFE plumber's tape wraps the tray perimeter before insertion.

**Rationale:** TAE buffer wicks under any gap. The lip provides positive mechanical registration that prevents lateral movement and reduces the gap to near-zero. PTFE tape (chemically inert to TAE, ~$3) compresses into remaining micro-gaps without bonding permanently — the tray remains removable for cleaning. Do not use silicone caulk (cannot re-open) or super glue (resin compatibility issues). Electronics (LED strip) must sit below the gel floor level so any buffer leak drains down and out, never up toward the LEDs.

### 4. Hinge: Hardware stainless butt hinge

**Decision:** 25 × 25mm stainless steel butt hinge, press-fit or M3-screwed into printed pockets in the lid and enclosure rear wall. Two hinges, symmetrically placed.

**Rationale:** Print-in-place resin hinges fail under cyclic loading. The gel lid opens and closes dozens of times per experiment; resin fatigue at the layer interface causes failure within weeks. A standard stainless butt hinge ($1.50/pack of 4, Amazon) press-fit into 1.5mm-deep pockets (one drop of super glue) lasts indefinitely. Print-in-place is appropriate for low-cycle applications (snap latches, living hinges on cuvette lids) — not here.

### 5. Power: Two isolated USB supplies

**Decision:** Pi 4B powered by its USB-C port (dedicated 3A USB-C adapter). LED transilluminator powered by a separate USB port/adapter (phone charger or powered USB hub, 5V @ ≥2A).

**Rationale:** Powering the LED array from Pi GPIO pins risks 300–800mA draw, causing Pi undervoltage, camera noise, and SD card corruption. Two independent USB supplies keep the digital and LED domains cleanly separated at zero cost. The bench PSU being purchased for electrophoresis is a separate domain (300V/500mA) — keep it isolated from 5V electronics entirely. If relay-based LED switching is added later (Pi GPIO → transistor → relay coil → LED supply enable), the relay coil runs from Pi GPIO through a transistor driver, never direct LED current.

---

## Open Questions
All open questions resolved above.

---

## References / Inspiration
- OpenGelBox (existing open-source designs)
- GelGreen / SYBR Safe excitation/emission spectra (470nm excitation, 520nm emission peak)
- Raspberry Pi Camera Module v2 datasheet — IMX219, 62.2° diagonal FOV
- Pi v2 manual focus mod: Raspberry Pi Forum, multiple documented procedures

---

*Last updated: 2026-05-16 — all design questions resolved, CAD stubs committed*
