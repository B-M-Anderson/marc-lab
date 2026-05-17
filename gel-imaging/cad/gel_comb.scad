// Marc Lab — Parametric Gel Comb
// Generates 8-tooth and 16-tooth variants side by side for a single slice job.
// Print with teeth pointing DOWN; supports on handle flat face only.
// Resin (MSLA): standard or ABS-like resin. Min feature: 1.2mm tooth width.

// ─── SHARED GEOMETRY ─────────────────────────────────────────────────────────
tooth_height     = 3.0;   // mm — depth the tooth sits in the gel (sets well depth)
handle_height    = 15.0;  // mm — portion above gel surface; grip zone
handle_thickness = 3.0;   // mm — comb body thickness front-to-back
slot_width       = 1.6;   // mm — tab that registers into enclosure wall channel
slot_depth       = 2.0;   // mm — how far the tab extends into the wall channel
slot_height      = 4.0;   // mm — vertical extent of the registration tab
fillet_r         = 0.3;   // mm — tooth tip radius; prevents stress cracking at root

gap_between      = 20.0;  // mm — space between the two rendered combs (for slicing)

// ─── MODULES ─────────────────────────────────────────────────────────────────

module tooth(tooth_width, tooth_height, handle_thickness) {
    // A single gel comb tooth with a rounded tip.
    // Origin at tooth base centre, extends downward in -Z.
    hull() {
        // Body of tooth (rectangular, minus tip rounding)
        translate([0, 0, -(tooth_height - fillet_r)])
            cube([tooth_width, handle_thickness, tooth_height - fillet_r], center=true);
        // Rounded tip
        translate([0, 0, -(tooth_height - fillet_r)])
            rotate([90, 0, 0])
                cylinder(r=fillet_r, h=handle_thickness, center=true, $fn=16);
    }
}

module slot_tab(handle_thickness) {
    // Registration tab on each end of the handle.
    // Slides into a matching channel in the enclosure side wall.
    cube([slot_width, handle_thickness + slot_depth * 2, slot_height], center=true);
}

module gel_comb(tooth_count, tooth_width, tooth_gap) {
    total_width = tooth_count * tooth_width + (tooth_count - 1) * tooth_gap;
    handle_w    = total_width + slot_width * 2 + 2.0; // 1mm overhang each side

    union() {
        // Handle bar
        translate([0, 0, handle_height / 2])
            cube([handle_w, handle_thickness, handle_height], center=true);

        // Registration tabs — one each end, centred vertically on the handle
        for (side = [-1, 1])
            translate([side * (handle_w / 2 + slot_width / 2), 0, handle_height / 2])
                slot_tab(handle_thickness);

        // Teeth — distributed evenly along the handle
        for (i = [0 : tooth_count - 1]) {
            x_pos = -total_width / 2 + tooth_width / 2 + i * (tooth_width + tooth_gap);
            translate([x_pos, 0, 0])
                tooth(tooth_width, tooth_height, handle_thickness);
        }
    }
}

// ─── RENDER ──────────────────────────────────────────────────────────────────
// 8-tooth comb (wider wells, ~15µL at 3mm depth)
// tooth_width=2.0, tooth_gap=3.0 → 10mm lane pitch (standard)
gel_comb(tooth_count=8, tooth_width=2.0, tooth_gap=3.0);

// 16-tooth comb (narrow wells, ~8µL at 3mm depth) — offset to the right
// tooth_width=1.2, tooth_gap=2.0 → 3.2mm lane pitch (tight but printable in MSLA)
translate([80 + gap_between, 0, 0])
    gel_comb(tooth_count=16, tooth_width=1.2, tooth_gap=2.0);

// ─── PRINT NOTES ─────────────────────────────────────────────────────────────
// Orient: teeth pointing DOWN on build plate
// Supports: auto-support on handle flat face; zero supports on tooth surfaces
// Layer height: 0.05mm for tooth tip accuracy
// Exposure: standard for your resin; reduce bottom layers to 4 to ease release
// Post-cure: IPA wash 2min, UV cure 60s — do not over-cure (embrittlement)
// Slot clearance: enclosure wall channel should be 1.7mm wide (0.1mm per side)
