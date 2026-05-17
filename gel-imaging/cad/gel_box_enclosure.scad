// Marc Lab — Gel + Imaging Station Enclosure
// DIMENSIONED STUB — geometry locked, module bodies TBD in session 2+
//
// Architecture: unified resin-printed enclosure.
//   Base: gel box cavity + LED tray pocket + electrode slots + buffer zone
//   Hood: lightproof imaging chamber above gel surface
//   Lid:  hinged, camera mount + amber filter slot + Pi cable routing
//
// ALL dimensions below are LOCKED. Do not change after first print validation.
// Changes to gel_cavity or hood_height require reprinting the full enclosure.

// ─── LOCKED GEOMETRY ─────────────────────────────────────────────────────────

// Gel cavity (internal, buffer-filling volume)
gel_cavity_l     = 100.0;  // mm — 10cm internal length (X axis, along lanes)
gel_cavity_w     = 70.0;   // mm — 7cm internal width  (Y axis, across lanes)
gel_cavity_depth = 30.0;   // mm — TAE fill depth (25mm) + 5mm safety clearance

// Structural
wall_thickness   = 3.5;    // mm — MSLA structural minimum at this scale
base_thickness   = 4.0;    // mm — enclosure floor; LED tray pocket below gel floor

// LED tray pocket (below gel floor, recessed into base)
led_tray_l       = gel_cavity_l;  // flush with gel cavity
led_tray_w       = gel_cavity_w;  // flush with gel cavity
led_tray_h       = 12.0;  // mm — LED strip (5mm) + diffuser (3mm) + clearance (4mm)
led_tray_lip     = 1.5;   // mm — perimeter lip height that mates with tray

// Imaging hood (sits on top of enclosure base, lightproof)
hood_height      = 80.0;   // mm — gel surface to camera lens = Pi v2 focus distance
                            //      (Pi v2 lens unscrewed 2-3 turns focuses at ~80mm)
hood_wall        = 3.5;    // mm — same as base wall for uniform print appearance

// Lid (hinged, sits on top of hood)
lid_thickness    = 4.0;    // mm — structural; camera mount loads this face

// Camera port (in lid roof, centred over gel cavity)
camera_port_d    = 25.0;   // mm — clearance for Pi camera lens assembly
filter_slot_w    = 32.0;   // mm — amber filter slide slot width
filter_slot_h    = 2.0;    // mm — filter sheet thickness slot (1.5mm sheet + clearance)
filter_slot_d    = 5.0;    // mm — how deep filter slides in from front face

// Comb slots (in side walls, tooth registration channels)
comb_slot_w      = 1.7;    // mm — 1.6mm comb tab + 0.1mm clearance per side
comb_slot_depth  = 2.5;    // mm — channel depth into wall face
comb_slot_z      = gel_cavity_depth - 5.0;  // mm — slot top position from base floor

// Electrode slots (in end walls)
electrode_wire_d = 0.6;    // mm — 0.5mm Pt wire + 0.1mm clearance; run in from ends
electrode_z      = gel_cavity_depth / 2;    // mm — mid-depth of gel cavity

// Banana jack holes (exterior of end walls)
banana_jack_d    = 4.2;    // mm — 4mm jack + 0.2mm clearance
banana_jack_z    = gel_cavity_depth / 2;

// Hinge pockets (rear wall exterior, for 25×25mm stainless butt hinge)
hinge_pocket_w   = 25.0;   // mm — hinge leaf width
hinge_pocket_h   = 25.0;   // mm — hinge leaf height
hinge_pocket_d   = 1.5;    // mm — hinge leaf thickness (flush-mount)

// Derived outer dimensions (for reference and BOM)
outer_l = gel_cavity_l + 2 * wall_thickness;   // 107mm
outer_w = gel_cavity_w + 2 * wall_thickness;   // 77mm
outer_h = base_thickness + led_tray_h + gel_cavity_depth + hood_height + lid_thickness;
                                                // ~150mm total height

// ─── MODULE STUBS (geometry TBD session 2+) ──────────────────────────────────

module enclosure_base() {
    // Gel box cavity + base floor + LED tray pocket + electrode slots + comb slots
    // TODO session 2: full geometry
    echo("enclosure_base: outer_l=", outer_l, " outer_w=", outer_w);
    echo("  gel cavity:", gel_cavity_l, "×", gel_cavity_w, "×", gel_cavity_depth, "mm");
    echo("  electrode slots at z=", electrode_z, "mm, dia=", electrode_wire_d, "mm");
    echo("  comb slots at z=", comb_slot_z, "mm, width=", comb_slot_w, "mm");
}

module imaging_hood() {
    // Lightproof box above gel surface, walls only — open bottom mates with base top
    // TODO session 2: full geometry
    echo("imaging_hood: height=", hood_height, "mm, wall=", hood_wall, "mm");
}

module led_tray() {
    // Removable snap-fit tray: LED strip + resin diffuser layer
    // Lip registration: 1.5mm raised perimeter lip mates into base channel
    // Assembly: install with PTFE tape wrap around perimeter before insertion
    // TODO session 3: full geometry + diffuser layer
    echo("led_tray:", led_tray_l, "×", led_tray_w, "×", led_tray_h, "mm");
    echo("  lip height:", led_tray_lip, "mm");
}

module camera_mount() {
    // Fixed focal mount in lid — Pi camera press-fits, no adjustment after install
    // Amber filter slot: filter slides in from front, sits between lens and gel
    // TODO session 3: full geometry
    echo("camera_mount: port_d=", camera_port_d, "mm");
    echo("  filter slot:", filter_slot_w, "×", filter_slot_h, "×", filter_slot_d, "mm");
}

module lid() {
    // Hinged lid: camera_mount() + Pi cable channel + hinge pockets (rear)
    // Hinge: 25×25mm stainless butt hinge, press-fit into hinge_pocket geometry
    // TODO session 3: full geometry
    echo("lid: thickness=", lid_thickness, "mm");
    echo("  hinge pocket:", hinge_pocket_w, "×", hinge_pocket_h, "×", hinge_pocket_d, "mm");
}

// ─── RENDER (stub — shows echo output only until session 2 geometry is added) ─
enclosure_base();
imaging_hood();
led_tray();
camera_mount();
lid();

// ─── ASSEMBLY NOTES ──────────────────────────────────────────────────────────
// Print order: enclosure_base first (validate cavity dimensions with calipers)
// Then: led_tray (confirm lip engagement + PTFE seal)
// Then: lid (confirm hinge alignment + camera focus distance)
// Resin: black or grey pigmented — clear resin transmits ambient light, unacceptable
// All M3 fasteners use heat-set brass inserts (press in with soldering iron tip)
