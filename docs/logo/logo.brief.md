# Hologram Opacity logo: brief and provenance

The mod's logo for ficsit.app (SMR). The mod author picked it on 2026-11-11 from five hand-drawn concepts
(option 4, "Ghost Factory"), and it was then refined for small sizes. Hand-authored SVG: no image generator was
used (the Recraft MCP server was not connected in that session).

```
Subject:     a sawtooth-roof factory with a chimney. The left half is a solid hologram (scan lines, bright
             edges); the right half is the same hologram turned down (scan lines at 22% opacity, dashed
             outline). A before/after slider handle splits them. FICSIT-orange ground bar.
Style:       flat vector, game-UI flavoured, no text, no game logos or FICSIT marks
Palette:     hologram #00D3FF (the game's default Hologram Color), stripe #0098C2, light #7BEAFF,
             edge #B5F5FF; FICSIT orange #F08F3D, shade #C46F25; background #1F262F -> #0D1014 with a
             5% white 64 px grid; window ink #06121A; handle #FFFFFF / #071118
Background:  full-bleed dark square; the motif stays inside the inscribed circle because Discord shows the
             ficsit.app logo as a round webhook avatar
Output:      hologram-opacity-logo.svg (master) and hologram-opacity-logo.png (512 x 512, the one to upload;
             ficsit.app converts uploads to WebP without resizing)
Size:        512 x 512 viewBox
Variants:    1 (refined from option 4 of 5)
```

Refinements over option 4 as first shown: a larger building and chimney; two 44 px windows per half instead of
three 32 px ones; the ghost half shows faint scan lines (like a low-opacity hologram in game) with bolder 18/12
dashes; a larger slider handle; a soft cyan glow behind the solid half.

Rendering: headless Microsoft Edge at 2x device scale (1024 px), downscaled with Pillow (LANCZOS). Checked at 512,
96, 64 and 32 px, on light and dark pages, and in a round crop.
