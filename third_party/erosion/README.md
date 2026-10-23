# Erosion provenance

HLSL port of Rune Skovbo Johansen's Advanced Terrain Erosion Filter (2025), via Luke Mitchell's C# translation (2026).

Reference: https://github.com/lpmitchell/AdvancedTerrainErosion/blob/main/package/Runtime/LPMGames.Terrain.Erosion/AdvancedTerrainErosion.cs

Exact reference Git blob SHA: `115812e21b7286c5627d6278401d9386c30d168e` (content-addressed source identity).

See THIRD_PARTY_NOTICES.md and LICENSE. Noise derivative formulas retain Inigo Quilez's MIT attribution; erosion/Phacelle code retains MPL-2.0. The shader files contain the applicable notices.

Local adaptations: split lattice coordinates, integer seeded hashing, fixed octave counts, HLSL buffer interfaces, removal of unused ridge-map/approximate output-derivative calculations, and a separate finite-difference normal pass. No Unity code or runtime dependency is included.
