# 5J-V5: interaction and habitat variety

Status: implemented and natively verified. See the [verification and measurement report](measurements/vegetation_5j_v5/README.md) for evidence and remaining V6 performance/visual gates.

## Responsibilities and interfaces

Vegetation owns a bounded GPU bend/recovery field independent of camera residency.
Persistent settings contain enable and recovery time; transient input contains a
WorldPosition anchor, reset generation, and up to 16 grounded capsules (coincident
endpoints are circles). Invalid input fails validation; append beyond capacity is
rejected and counted. Radii are 0.1–4 m and swept segments at most 64 m;
teleports must reset rather than stamp a cross-world segment. The demo owns proxy controls, not the renderer. No character,
collision, vertical reach, persistent trampling or saved field state is introduced.

The field is 128x128 float2 values at 0.25 m spacing (32 m square). Three buffers (384 KiB payload)
retain submitted snapshots, with per-frame descriptor/uniform ownership. Origins
snap in double/world coordinates before float conversion. Compute copies overlapping
texels, clears exposed strips, stamps shapes with bounded response and decays untouched
values with 1.5 s half-life. Contact response is 0.05 s. The outer 2 m tapers to zero.
Explicit resets, non-overlapping scrolls and time discontinuities invalidate history.
Camera movement never moves the field. Previous submitted snapshots drive motion.
Queue barriers cover compute writes and all previous vertex/compute readers; only
successful submission commits state, including the demo sweep endpoint. No frame allocation, waits or CPU visibility
readback is added. Actual allocation requirements count against 256 MiB.

Shared vertex deformation anchors roots and combines species bend/flatten response
with wind, terrain slope and grass morphs. Depth, colour and CSM use identical geometry.
Bounds include both maximum displacements. Reset-affected plants reject history locally.
Grass remains receive-only. Interaction updates precede caster selection and rendering.

## Population and content

Keep 4 m cells, 128 candidates (126 grass, one fern/flower, one shrub/heather),
32-byte instances, 12,288 engine slots and at most 32 cell updates per frame.
Explicit species metadata replaces category-first discovery. Identity remains seeded
by world/cell/species/candidate, independent of camera and quality. Algorithm, library
and habitat revisions invalidate population; attachment remains a separate operation.

Blended habitats use stable lattice noise: 128 m broad patches, 8 m species clusters,
32 m clearings. Uniform meadow/moor/understory remain inspection presets. Shared HLSL
evaluation supplies terrain expected coverage and population probabilities, retaining
rock exclusion, canonical suitability and authored clearing inputs. Integer lattice
origins plus local offsets prevent large-world precision loss.

Six assets: two grass tufts, fern, bilberry-like shrub, daisy clump and purple heather.
Three LODs, at most two parts each, fit existing eight-asset/48-bin bounds. Format 4
stores species, suitability and interaction metadata; v2/v3 load with compatibility
metadata and zero interaction response. V1 remains reproducible.

Offline source artwork and recipes are retained. Four foliage images share a 2x2
colour/normal atlas; mips are built per tile with padding and stop before tiles merge.
Both fixed/bindless bindings and compressed/RGBA assets remain supported. Runtime
loads saved assets. The texture reader/writer retains complete-chain validation by
default; foliage explicitly opts into truncated chains. Tile chains stop at 4×4,
keeping compressed blocks independent. Finish silhouettes, alpha edges, colour, transmission and motion
in close, grazing and distant views; final acceptance is visual as well as numerical.

## Demo and persistence

The ground-footprint proxy is inactive until placed. Arrow keys move it along world X/Z at 3 m/s,
Shift at 6 m/s; a radius-4 m circular route permits repeatable camera-independent review.
Removing the proxy stops stamping and lets the existing field recover; disabling
interaction or resetting clears it. Default contact radius is 0.65 m. Rendering/Vegetation controls placement, reset,
strength, radius and marker. ImGui keyboard capture is respected. Bookmarks retain
static proxy placement/settings and reset the trail on load; old bookmarks stay uniform.
Ordinary UI persistence remains window-only. Engine startup uses blended habitats.

## Verification

Owned CPU/build fixtures cover input bounds, world scrolling, species/population
identity, large/negative coordinates, library/bookmark compatibility, atlas mips and
incremental dependencies. GPU fixtures cover contact/recovery, scrolling, teleports,
separate camera/proxy, submitted history, resource reuse, depth/colour/CSM agreement,
conservative bounds and both bindings. Run Debug/Release, SPIR-V checks and Vulkan
synchronization validation. Native engine review covers strong wind, TAA on/off,
low sun and optional GTAO/Hi-Z. Record fresh paired interaction and vegetation off/on
1080p/native measurements at 80/200 m, median/p95, memory and allocations. Targets
remain ~1.5 ms GPU, ~0.25 ms CPU and 256 MiB; failures and unavailable gates stay open.

## Concrete layout and compatibility

The interaction compute uniform is 560 bytes: integer scroll, response/control,
16 endpoint vectors and 16 radius/strength vectors. Two frame-owned uniforms and
descriptor sets refer to the three field buffers; no descriptor is edited while its
frame is in flight. Field compute runs immediately before caster selection. The
vegetation uniform grows to 3504 bytes with eight response vectors and two field
mappings; bindings 11/12 are current/previous field storage buffers. Diagnostic
field readback is allocated only for explicitly diagnostic/fixed fixtures.

Population jobs are 1936 bytes, still capped at 32 per frame. Metadata supplies
per-asset meadow/moor/understory affinities, clustering, footprint, conformity and
scale range. The two decorative slots choose within the fern/daisy and shrub/heather
families; grass identities retain their original hash salt. Population algorithm
revision 5 and library metadata revisions invalidate residency independently of
candidate identity. Caster lists reserve two parts per decorative slot and enclose
the largest authored caster range, including alternatives not used by startup placeholders.

Bookmark format 5 stores habitat mode, interaction configuration and static proxy
placement; formats 1–4 remain readable and load uniform habitats. Field contents and
transient contacts never enter saved JSON. The ordinary engine offers species
isolation alongside the proxy controls. Existing specimen/profiling tools accept
`--asset daisy|heather`; no additional viewer is introduced.
