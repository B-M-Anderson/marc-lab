# Marc Lab

Open-source DIY biology instrument suite. Built by Bennett — BME senior, Iowa State University (graduating May 2027). Pharmacology minor. End goal: gene therapy research.

**Mission:** Build low-cost, high-reliability lab instruments that make gene therapy research infrastructure accessible. Everything documented, everything open-source.

---

## Instrument Suite

| # | Instrument | Status | Est. Cost |
|---|-----------|--------|-----------|
| 1 | [Gel + Imaging Station](gel-imaging/) | Design — Week 1 | ~$255–300 |
| 2 | Microcentrifuge | Sourcing — Week 4–6 | ~$50–150 |
| 3 | [Fluorimeter PCB](fluorimeter/) | Design starts Week 3 | ~$55–100 |
| 4 | [Potentiostat PCB](potentiostat/) | Design starts Week 5 | ~$90–160 |
| 5 | [qPCR Thermocycler](thermocycler/) | Design starts Week 6 | ~$160–280 |
| 6 | [Microfluidic Chip Platform](microfluidic-chip/) | Design starts Week 9 | ~$40–80 |

---

## Capstone: AI-Directed CRISPR Optimization Platform

Senior capstone project (both semesters, 2026–2027).

A closed experimental loop running entirely on Marc Lab hardware:
- **Fluorimeter** reads Cas12a trans-cleavage assay results in real time (FAM-BHQ reporter)
- **Bayesian optimization agent** (Python + Claude/Hermes via OpenClaw) proposes the next crRNA variant
- **Thermocycler** handles upstream isothermal amplification (LAMP/RPA)
- **Potentiostat** provides secondary electrochemical readout for redundant validation
- **Microfluidic chip** is the reaction vessel with integrated incubation heating
- **Plasmidsaurus** sequences top crRNA candidates externally

Loop converges on optimal guide sequences. Validated with E. coli BSL-1 work.

---

## Directory Structure

```
marc-lab/
├── gel-imaging/
│   ├── cad/            OpenSCAD source files
│   ├── firmware/       Pi Python scripts (timelapse, band detection)
│   └── notes/          Design decisions, build log
├── fluorimeter/
│   ├── kicad/          PCB schematic + layout
│   ├── firmware/       STM32 firmware
│   └── notes/
├── potentiostat/
│   ├── kicad/
│   ├── firmware/
│   └── notes/
├── thermocycler/
│   ├── kicad/
│   ├── firmware/
│   └── notes/
├── centrifuge/
│   └── notes/
├── microfluidic-chip/
│   ├── cad/
│   └── notes/
├── capstone/
│   ├── agent/          Bayesian optimizer + OpenClaw integration
│   ├── protocols/      Wet lab protocols
│   └── data/           Experimental results
└── shared/
    ├── python/         Shared utility libraries
    └── bom/            Master bill of materials
```

---

## Tech Stack

| Domain | Tools |
|--------|-------|
| PCB Design | KiCad |
| CAD | OpenSCAD, resin MSLA printing, FDM |
| Firmware | STM32 (C/HAL), Python (Pi) |
| Software | Python, OpenCV, Claude API (Hermes/OpenClaw) |
| Wet Lab | E. coli BSL-1, PCR, gel electrophoresis, Cas12a assays |
| Sequencing | Plasmidsaurus (~$7/sample) |

---

## Constraints

- Home lab only — no ISU facility access
- BSL-1 work only
- Budget: ~$150–200/month (summer 2026), scaling up
- No oscilloscope (yet — DS1054Z is on the wish list)

---

## Build Timeline

**Summer 2026:** Gel station → centrifuge → fluorimeter → potentiostat → thermocycler → chip
**Fall 2026:** Capstone system integration, Cas12a assay validation, agent loop v1
**Spring 2027:** Full system, open-source release, capstone demo, manuscript draft

---

## License

Hardware: [CERN OHL v2 — Permissive](https://ohwr.org/cern_ohl_p_v2.txt)
Software: [MIT](LICENSE)

---

*Marc Lab — Bennett Anderson — ISU BME 2027*
*Build started: May 16, 2026*
