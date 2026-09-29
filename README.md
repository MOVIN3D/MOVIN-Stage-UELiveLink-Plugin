# MOVIN Stage LiveLink

Receives the MOVIN Stage stream over a single UDP port (default 11235) and delivers:

- **Motion** — OSC messages at `/VMC/Ext/Root/Pos` and `/VMC/Ext/Bone/Pos` as LiveLink animation subjects. Every actor name in the stream becomes a subject on its first packet; there is no actor list to configure. The `/VMC/` address prefix is legacy naming on the sender; this is not the VMC protocol.
- **Point cloud** — `/MOVIN/PointCloud` chunks reassembled into whole frames and rendered through Niagara.

The two streams share one socket. Motion runs at 60 fps and the point cloud at 20 fps.

## Modules

| Module | Contents | Dependencies |
|---|---|---|
| `MOVINStageLiveLink` | UDP receive, OSC parsing, point cloud reassembly, LiveLink source | no rendering dependencies |
| `MOVINStagePointCloudRender` | the two renderer components | Niagara, LidarPointCloudRuntime, RenderCore, RHI |

The split keeps the LiveLink source free of rendering dependencies, so it can be used headless.

## Running alongside the MOVIN LiveLink (Tracin) plugin

Both plugins can be enabled in one project. Nothing collides at build or load time: different plugin
names, module names, `/Script/` packages and class names throughout. Three things need attention,
and all three fail *quietly* rather than with an error.

**1. Give each source its own UDP port.** This plugin defaults to **11235**, MOVIN LiveLink to
**11236**, so out of the box they do not clash. If you do point them at the same port, this plugin
now refuses to start and the Live Link panel reads `Port <n> is in use - another LiveLink source may
already own it`. It binds without `SO_REUSEADDR` specifically so the OS rejects the duplicate: with
the flag set, both sockets bind successfully on Windows and datagrams reach only one of them, with
nothing logged anywhere. Fix the port in the panel and the source restarts on its own.

**2. Keep subject names distinct.** Live Link resolves subjects by name alone, so two sources
publishing the same name leave the Live Link Pose node choosing between them arbitrarily. This
plugin names its subjects after the incoming actor names (`MOVIN_0`, `MOVIN_1`, … as MOVIN Stage
sends them) and MOVIN LiveLink typically publishes `MOVINMan`, so they normally do not overlap, but
the actor names are whatever the sender chooses. Set **`SubjectPrefix`** (for example `Stage_`) if
you want the separation guaranteed; it changes only the published name, never the incoming actor
name.

**3. Pick the right Retarget Asset.** Both products stream `ULiveLinkAnimationRole`, so the Live Link
Pose node's **Retarget Asset** dropdown lists choices for both. This plugin's subjects need
**`MOVIN Stage OSC Remap (NeoMOVINMan)`**; MOVIN LiveLink's subjects use the stock
`LiveLinkRemapAsset`. Cross them and the character half poses with no error, because the two bone
name sets share `Hips`, `Spine`, `Neck` and `Head`. When nothing matches at all, the first frame
logs a warning under `LogMOVINStageLiveLink` naming the likely cause.

Telling the two apart in the editor: this plugin is **MOVIN Stage LiveLink** in the Plugins browser,
**MOVIN Stage (OSC)** in the `+ Source` menu, and reports `MOVIN Stage` / `MOVIN Stage - <endpoint>`
in the Live Link panel's Type and Machine Name columns. Its settings sit under `MOVIN Stage|…`,
its components under the `MOVINStage` class group, and its logs under `LogMOVINStageLiveLink`
(MOVIN LiveLink uses `LogMOVINLiveLink`).

One non-conflict worth knowing: this plugin's descriptor force-enables **Niagara** and
**LidarPointCloud** project wide, which MOVIN LiveLink does not require. Nothing breaks, but the
cook and package footprint grows.

All of the above is covered by tests. The development project vendors the MOVIN LiveLink plugin
(see `Plugins/MOVINLiveLink/ORIGIN.md`) and runs both at once:

```bash
UnrealEditor-Cmd.exe <Project>.uproject -unattended -nullrhi \
    -ExecCmds="Automation RunTests MOVIN.Coexistence; Quit" -TestExit="Automation Test Queue Empty"
```

`MOVIN.Coexistence.SimultaneousReceive` runs both sources against interleaved UDP traffic on 11235
and 11236 and checks each publishes its own subject with real frame data.
`MOVIN.Coexistence.PortContention` puts them on the *same* port in both orders — plus two Stage
sources on one port — and checks the second one always refuses and says which port it lost.

## Skeleton retargeting - read this if motion looks wrong

The plugin ships `UMOVINNeoMOVINManLiveLinkRemapAsset`, and the LiveLink Pose node in your Anim
Blueprint **must** be set to use it. It is not optional decoration: the OSC stream uses names like
`Chest`, `LeftUpperArm` and `LeftToes`, and this asset translates them to the NeoMOVINMan rig's
`Spine1`, `LeftArm` and `LeftToeBase`, plus a thirty bone finger map. With the engine's stock
`ULiveLinkRemapAsset` those names pass through unchanged, miss the target skeleton, and the character
poses partially or strangely - which looks like a plugin bug but is a retarget asset setting.

Check the Anim Blueprint's **Live Link Pose** node → **Retarget Asset**. It should read
**MOVIN Stage OSC Remap (NeoMOVINMan)**, not `LiveLinkRemapAsset`.

If the character does not move at all, look for this in the Output Log under `LogMOVINStageLiveLink`:

```
MOVIN Stage OSC remap matched none of the <n> streamed bones against this Skeleton.
```

That means the asset is attached to a subject it does not describe — the Live Link Pose node is bound
to another product's subject, or the Skeleton is not a NeoMOVINMan rig.

The mapping targets the NeoMOVINMan bone naming convention. On a different rig you need matching
entries, so either use `NeoMOVINMan_UE` or extend the map in
`MOVINNeoMOVINManLiveLinkRemapAsset.cpp`.

### Bone lengths come from the mesh, positions from the stream

MOVIN Stage streams bone offsets calibrated to the performer, not to the mesh. On a reference
capture the upper arm measured 1.25x and the shin 0.91x the NeoMOVINMan lengths. Applying those
offsets stretches the skin, and because they change whenever the stage recalibrates, they also move
the reference pose an IK Retargeter measures limb extension against - which shows up as a retargeted
character whose knees bend or feet float after a recalibration.

So by default the remap asset applies only **rotations** to the body and keeps the mesh's own bone
lengths. Two bones are the exception and always take the streamed translation while
`bApplySourceRootTranslation` is on: `Root`, which is the ground position, and `Hips`, which is the
pelvis height above it. Hips has to travel with Root because the stream's root frame is turned 90°
from the mesh's `RootBone` bind frame; on the authored offset the pelvis would land 86 cm in front of
the root rather than 94 cm above it.

Set `bApplySourceBoneTranslations` on the remap asset if you want the performer's exact proportions
instead. For an IK Retargeter source that should stay off.

## Setup

### 0. Installing the prebuilt package

The `Prebuilt/MOVINStageLiveLink-<version>-UE<engine>-Win64.zip` archives are built with Epic's
`BuildPlugin` for one engine version each. Unzip so that the `MOVINStageLiveLink` folder sits at
`<YourProject>/Plugins/MOVINStageLiveLink/` and reopen the project. The editor modules load as they
are, and the package also carries the precompiled game targets, so a **Blueprint-only project can
package a game** with it. Debug symbols are not included. The archive ships this README and
`NeoMOVINMan_UE.fbx`, the rig the remap asset targets.

Pick the archive that matches your engine version exactly; a plugin built for 5.8 will not load in
5.7. From source, `Scripts/Build-Prebuilt.ps1 -Versions 5.8` produces the archive.

### 1. Add and configure the LiveLink source

Window → Virtual Production → Live Link → **+ Source → MOVIN Stage (OSC)**.

The factory is a plain menu entry with no creation dialog, so everything is configured **after**
adding: select the source in the list and the details panel below shows its settings.

Subjects appear on their own. Each actor name in the stream becomes a Live Link subject the moment
its first packet arrives, so there is no list of performers to maintain and no cap tied to a
configuration field. Subjects stay listed until the source is removed. As a guard against a sender
that mints a new actor name per packet, a source stops accepting new actors past 64 and logs a
warning under `LogMOVINStageLiveLink`.

Point cloud reception is **off by default** so an existing motion only setup behaves exactly as
before. To turn it on, tick **MOVIN | Point Cloud → Enable Point Cloud**.

Changing any value stops the receive thread, swaps the configuration and restarts it - which is why
the settings can be edited while the source is live. Each edit also rewrites the source's
`ConnectionString`, so saving a Live Link preset afterwards reproduces the configuration exactly.

### 2. Render the point cloud

Two renderer components are provided. Both find the stream automatically — leave `SourceGuidFilter`
at its default to follow whichever LiveLink source most recently started streaming.

| Component | Needs an asset? | Effects | Cost per received frame |
|---|---|---|---|
| **MOVIN Stage Point Cloud (Lidar Renderer)** | **No** | None. Fixed point renderer: size, colour source, orientation only | Higher: a 20 byte point struct and a per-element buffer copy, a few ms of game thread |
| **MOVIN Stage Point Cloud** (Niagara) | Yes, an authored system (~30 min, guide below) | **Full Niagara stack** | Lower: one bulk texture upload |

**Start with the Lidar renderer** to confirm the data path — drop the component in and points appear,
nothing to author. That isolates network and coordinate problems from authoring problems.

**Move to Niagara when you want effects.** That is the real reason to pay the authoring cost; the
cheaper upload is a side benefit. The Lidar renderer cannot do effects at all.

Both refuse to interpolate between frames and both hold the previous frame until a new one arrives;
see "Behaviour worth knowing" below for why.

#### Lidar renderer

Add a **MOVIN Stage Point Cloud (Lidar Renderer)** component. It creates a transient
`ULidarPointCloud` in `SetOptimizedForDynamicData` mode and replaces its contents each frame, so the
engine's Lidar point renderer draws it — including screen space point sizing and circular sprites.
Point appearance comes from the inherited `ULidarPointCloudComponent` properties (`PointSize`,
`PointSizeBias`, `PointOrientation`, colour source, and so on).

Requires the engine's **LidarPointCloud** plugin, which the plugin descriptor already depends on.

The cloud volume is always centred on the component origin, because
`ULidarPointCloud::Initialize` sets the asset's `LocationOffset` to the box centre and an off-centre
box would silently shift every point.

#### Niagara renderer

Add a **MOVIN Stage Point Cloud** component and assign your Niagara system to its
`Niagara System Asset`.

Each tick the component pulls the newest frame, copies it into a staging buffer and uploads it to a
`PF_A32B32G32R32F` texture of `1024 x ceil(MaxPoints / 1024)` — one texel per point, XYZ in RGB, in
Unreal centimetres. It then pushes these Niagara **user parameters**:

| Parameter | Type | Meaning |
|---|---|---|
| `PointCloudPositionTexture` | Texture | The position texture |
| `PointCloudTextureWidth` | int32 | Row stride, always 1024 |
| `PointCloudTextureHeight` | int32 | Row count. Unused by the guide below; provided for UV based systems |
| `PointCloudActiveCount` | int32 | Points valid this frame |
| `PointCloudPointSize` | float | Forwarded from the component's `PointSize` |

The component also calls `SetSystemFixedBounds` every frame from the cloud's own extents, and
`SetForceSolo(true)` on construction. **Do not remove the solo call**: `UNiagaraComponent` disables
its own component tick unless the system runs in solo mode, because in batched mode
`FNiagaraWorldManager` ticks the system directly and `TickComponent` is never called
(`NiagaraComponent.cpp:917-923`). Since the texture upload runs from `TickComponent`, without solo the
upload stops as soon as the system instance is created — two frames get through and then nothing, with
no error anywhere.

---

## Building the Niagara system

The plugin cannot ship the system: a Niagara asset has to be authored in the editor. This is the
procedure that was actually used to get it working, with the traps that cost the most time called out.

Every name, type and pin below is verified against UE 5.7. Expect about 30 minutes.

### Step 1 — System and emitter

Content Browser → **FX → Niagara System**. When asked, pick **Empty**.

Add an emitter from the **`Simple Sprite Burst`** template. It already contains `Emitter State`,
`Initialize Particle`, `Particle State` and a Sprite Renderer, so there is far less to add than from
an empty emitter — and an empty emitter has **no renderer**, which compiles cleanly and draws nothing.

### Step 2 — User parameters

**`Window` menu → `User Parameters`.** The add button lives only in that tab; the `Parameters` panel
has no way to create them.

Press **`+`** four times. The tooltip on the right button reads *"Add a new User Parameter to this
system…"*. Rename entries by right-clicking → Rename.

| Name | Type | Default (editor preview only) |
|---|---|---|
| `PointCloudPositionTexture` | `Texture` | leave empty |
| `PointCloudTextureWidth` | `int32` | **1024** |
| `PointCloudActiveCount` | `int32` | 0 |
| `PointCloudPointSize` | `float` | 2.0 |

Names must match exactly — a typo fails silently, with no error anywhere. The `User.` prefix shown in
the list is normal; the component's `SetVariableInt("PointCloudTextureWidth", …)` resolves into that
namespace, so only the part after the dot has to match.

### Step 3 — Emitter properties

Click the **`프로퍼티` / Properties** row at the top of the emitter stack.

| Setting | Value | Why |
|---|---|---|
| `Local Space` | checked | Points arrive in component space |
| `Sim Target` | **`GPUCompute Sim`** | `LoadTexture2D` is GPU only |
| `Calculate Bounds Mode` | **`Fixed`** | `Dynamic` is documented CPU only, so a GPU emitter reports no bounds and the cloud is frustum culled the moment the camera looks away |
| **Advanced →** `Allocation Mode` | **`Manual Estimate`** | Stops per frame reallocation |
| `Pre Allocation Count` | `10000` to start | See step 7 |

`Allocation Mode` and `Pre Allocation Count` are `AdvancedDisplay`, so they are **invisible until you
expand `Advanced`** at the bottom of the `Emitter` section. `Pre Allocation Count` additionally stays
hidden until `Allocation Mode` is anything other than `Automatic Estimate` — change the mode first.

Choosing `Fixed` bounds reveals a `Fixed Bounds` box. Its value does not matter; the component
overwrites it every frame.

### Step 4 — Spawning, lifetime, and modules to delete

**`Emitter Update` → `Emitter State`:**

| Input | Value |
|---|---|
| `Loop Behavior` | **`Once`** |
| `Loop Duration Mode` | **`Infinite`** |

This pair is the one to get right. `Loop Behavior = Infinite` re-fires the burst on every loop, so
with immortal particles the count grows without bound and the editor grinds to a halt. `Once` with a
**fixed** duration is the opposite failure: the loop ends, `Life Cycle Mode = Self` completes the
emitter and the particles are destroyed. `Once` + `Duration Infinite` means one loop that never ends —
the burst fires exactly once and the population is permanent.

**`Spawn Burst Instantaneous`:** `Spawn Count = 10000`, `Spawn Time = 0`.

**`Particle Spawn` → `Initialize Particle`:** set `Lifetime` to something huge, e.g. `100000`.

**`Particle Update` → `Particle State`:** leave the module, but make sure
`Kill Particles When Lifetime` is unchecked.

**Delete `Solve Forces and Velocity`.** It integrates velocity into `Particles.Position` and will
overwrite everything the module below writes. Deleting `Scale Color` is also recommended: it drives
alpha from `NormalizedAge`, which is meaningless when particles never age.

Particles are not spawned per frame — they are **slots**. The burst creates N once, and each frame
every particle re-reads its own texel, so the same N particles are repositioned. Nothing accumulates.

### Step 5 — The Scratch Pad module

**`Particle Update` group → `+` → Scratch Pad module.** It must be the **last** module in Particle
Update; anything after it can overwrite the position.

The fresh module contains `InputMap` → `Map Set` → `Output Module`, plus a `Map Get`.

**5a. `Map Get` pins** — press its `+` once per row:

| Pin | Type |
|---|---|
| `Module.Texture` | **`Texture Sample`** (the texture data interface's display name) |
| `Module.TextureWidth` | `int32` |
| `Module.ActiveCount` | `int32` |
| `Module.PointSize` | `float` |
| **`Particles.UniqueID`** | `int32` — **select the existing parameter, do not create one** |

`Particles.UniqueID` is the per-particle index. The engine writes it once at spawn as
`TotalSpawnedParticles + ExecIndex()`, so for a single burst it is `0 … N-1` and it is stored as a
particle attribute, which makes it reliable to read in an update script. `Engine.ExecIndex` looks like
the obvious choice and did **not** work here — prefer `UniqueID`.

**5b. Maths.** Before searching for these nodes, **uncheck `라이브러리만` / Library Only** and set the
source filter to **Show All** in the node menu, or the operators will not appear.

| Node | Search | A (top pin) | B (bottom pin) |
|---|---|---|---|
| `Modulo` | `Modulo` or **`%`** | `Particles.UniqueID` | `Module.TextureWidth` |
| `Divide` | `Divide` or **`/`** | `Particles.UniqueID` | `Module.TextureWidth` |

Order matters and is easy to get wrong because both nodes render as a compact symbol with the pin
names hidden. `Modulo` is `A % B`, `Divide` is `A / B`. Hover a pin to see whether it is `A` or `B`.
Both are numeric ops, so integer inputs give integer maths automatically.

**5c. Texture read.** Drag from the `Module.Texture` pin into empty space and pick **`LoadTexture2D`**.

| Pin | Connect |
|---|---|
| `Texture` | `Module.Texture` |
| `TexelX` | `Modulo` output |
| `TexelY` | `Divide` output |
| `MipLevel` | `0` |
| → `Value` | `Vector4` output |

Use `LoadTexture2D`, not `SampleTexture2D`: integer texel fetch needs no UV normalisation, no
filtering and no half-texel offset. **Connect `TexelX` and `TexelY` deliberately and confirm both pins
become filled circles.** An unconnected pin silently defaults to `0`, so every particle reads texel
`(0,0)` — which looks like a single point that jitters as the frame data changes.

**5d. `Map Set` outputs.** Press `+` twice and pick from the **`Particles`** group:

| Pin | Type | Source |
|---|---|---|
| `Particles.Position` | **`Position`** | via a `Vector 4 -> Position` node |
| `Particles.SpriteSize` | `Vector2D` | via `Select`, below |

Check the namespace badge on each pin: it must read **`파티클` / Particles**. Picking `Local.Position`
by mistake compiles cleanly, raises no error, and leaves every particle at the origin.

`Particles.Position` is of type `Position`, not `Vector` — large world coordinates. So `Value` cannot
connect directly: drag from it, add **`Vector 4 -> Position`**, and map X→X, Y→Y, Z→Z. W is unused.

**5e. Size masking.**

```
Less Than (<) :  A = Particles.UniqueID,  B = Module.ActiveCount   → bool
Make Vector 2D:  X = Module.PointSize,    Y = Module.PointSize
Select        :  Selector = Less Than,  If True = Make Vector 2D,  If False = (0, 0)
                 → Particles.SpriteSize
```

`<` is also `A op B`, so `UniqueID` goes on the top pin. Particles past `ActiveCount` get size zero,
which degenerate culls — no kill and respawn churn, and the particle buffer stays stable. **Keeping the
buffer stable is mandatory here**, because `UniqueID` is what maps a particle to its texel.

Confirm `Map Set.Dest` still reaches `Output Module.OutputMap`, then press **`적용 및 저장` / Apply and
Save**.

### Step 6 — Link the module inputs

Back in System Overview, expand the `Scratch Module` row. The four inputs still hold literal values;
link them with the `∨` dropdown on each row:

| Input | Link to |
|---|---|
| `Module_TextureWidth` | `User.PointCloudTextureWidth` |
| `Module_ActiveCount` | `User.PointCloudActiveCount` |
| `Module_PointSize` | `User.PointCloudPointSize` |

A linked input shows the parameter name instead of an editable number. If a number box remains, it is
not linked and the module reads a constant.

**`Module_Texture` works differently.** Expand it and you will find two fields:

| Field | Set to |
|---|---|
| `Texture` | leave empty |
| **`Texture User Parameter`** | **`PointCloudPositionTexture`** |

The data interface has to be bound through that field, not by picking a user parameter from a
dropdown, because `SetVariableTexture` sets a `UTexture` **object parameter** and the interface
references it through this binding. Its engine tooltip says *"When valid the user parameter is used as
the texture rather than the one on the data interface"*.

Press **Compile** and check the `나이아가라 로그` / Niagara Log tab.

### Step 7 — Particle count

Raise **both** of these to the same value — `Spawn Count` alone starves the buffer, `Pre Allocation
Count` alone starves the particles:

| Where | Value |
|---|---|
| `Emitter Properties → Advanced → Pre Allocation Count` | `331776` |
| `Emitter Update → Spawn Burst Instantaneous → Spawn Count` | `331776` |

`331776` is `324 x 1024`, matching the source's default `MaxPoints`, so the renderer can always show
whatever the source caps at. Frames with fewer points are handled by the `ActiveCount` mask, so over
allocating costs only the hidden particles' negligible processing. Expect one hitch the first time as
the GPU buffers allocate.

Particles must be at least as many as the points you want to see: with 10,000 particles only the first
10,240 points of each frame appear, which looks like a correct but partial cloud.

### Traps, ranked by time lost

| Trap | Symptom |
|---|---|
| Component tick disabled because the system is not solo | `Frames : N published, 2 consumed` in the tooltip. Handled in code by `SetForceSolo(true)` — do not remove it |
| `TexelX` / `TexelY` left unconnected | One point, jittering as data updates |
| `Local.Position` instead of `Particles.Position` | Everything at the component origin, no error |
| `Engine.ExecIndex` instead of `Particles.UniqueID` | Constant texel, one point |
| `Loop Behavior = Infinite` | Particles accumulate without bound, editor grinds to a halt |
| `Solve Forces and Velocity` left in place | Positions drift or collapse |
| `Calculate Bounds Mode = Dynamic` on a GPU emitter | Cloud vanishes when the camera turns away |
| `%` or `÷` operands swapped | `1024 % index` is constant for index > 1024, so ~90% of particles land on one texel |

### Diagnosing from the symptom

Hover the LiveLink source for the statistics first — they split "not received" from "not rendered"
before you touch the graph.

| Symptom | Look at |
|---|---|
| `consumed` stuck at a small number | Component tick. Compare against the Lidar renderer: if that keeps consuming, the problem is Niagara specific |
| Everything at the component origin | `Particles.Position` namespace, or the texture reading zero |
| One point that jitters | All particles read one texel: `TexelX`/`TexelY` connections, or the index source |
| A line or stripes | One of `TexelX`/`TexelY` is constant |
| Nothing visible at all | `<` operands swapped, `ActiveCount` zero, or `PointSize` too small |
| Correct but partial cloud | Particle count lower than the point count — step 7 |

To prove the index and maths independently of the texture, feed `Modulo` and `Divide` into a
`Make Vector 4` and route that through `Vector 4 -> Position` instead of `Value`. A grid roughly
10 m x 0.1 m means the index side is healthy and the texture read is at fault.

### Effects on a point cloud

This is the reason to use Niagara at all. Add effect modules **after** the Scratch Pad module, so they
transform the position it read.

One property of the data shapes what is possible: **LiDAR points have no identity between frames.**
The i-th particle of one frame is not the same surface as the i-th particle of the next — 20 fps of
entirely new point sets flow through the same particle slots.

Works well, because it only depends on the current position:

- colour by height, distance or region
- noise displacement, jitter, warping
- world space forces applied as a positional offset
- camera distance driven size and opacity, screen constant sizing via the Camera Query interface
- spatial masking, scan lines, reveals

Does not work, because it needs state to accumulate across frames:

- trails and ribbons — the particle jumps to an unrelated position each frame
- velocity driven effects — velocity is meaningless
- age or lifetime fades — particles never age
- physics with accumulation, such as gravity or collision response — the position is overwritten every
  frame

If you need accumulating state, the design has to change: the particles would have to own their
motion rather than be repositioned from the texture.

## Connection string reference

These are the keys behind the settings panel. You do not normally type them - the panel writes them -
but they are what a Live Link preset stores, and what `FromConnectionString` accepts.

| Key | Default | Notes |
|---|---|---|
| `Endpoint` | `0.0.0.0:11235` | |
| `SubjectPrefix` | *(empty)* | Prepended to published subject names only; incoming actor names are unchanged. An `Actors` key from older presets is accepted and ignored |
| `Scale` | `100` | Metres to centimetres |
| `Timeout` | `1.0` | Seconds before the source reports "No motion" |
| `Coordinates` | `Unity` | `Unity` or `OpenGL` |
| `FingerNames` | `MovinManNotch` | `OSC` or `MovinManNotch`. `VMC` is accepted as a legacy alias for `OSC` |
| `ApplyRotationOffset` | `true` | Skeleton only; rotations only |
| `RotationOffset` | `0,-90,-90` | Unity Euler degrees |
| `PointCloud` | `false` | Master switch. Off reproduces the pre-point-cloud behaviour exactly |
| `MaxPoints` | `331776` | 324 x 1024, so the texture has no partial row |
| `PointCloudCoordinates` | `Inherit` | `Inherit`, `Unity` or `OpenGL` |
| `PointCloudScale` | `0` | Zero inherits `Scale` |
| `PointCloudOffset` | `0,0,0` | Unreal space centimetres |
| `PointCloudAddress` | `/MOVIN/PointCloud` | OSC address the MOVIN Stage system sends the point cloud on. Leave at the default unless MOVIN tells you otherwise |
| `PointCloudPartialPublish` | `true` | Publish frames that are missing chunks |
| `SeqResyncThreshold` | `100` | Backwards sequence jump treated as a sender restart |
| `RecvBufferBytes` | `33554432` | `SO_RCVBUF`. One frame burst is about 5 MB |
| `MaxDatagramsPerWait` | `8192` | Drain budget per readable notification |

Every key is emitted by `ToConnectionString`, which is what LiveLink presets serialise. A key added
to the parser but not to the writer is silently lost on save and reload — `Settings.RoundTrip`
guards against that.

## Diagnostics

Hover the source in the Live Link panel. Motion and point cloud rates are reported **separately**
because they run at different rates, so a single counter cannot say which stream stalled:

```
Motion : <n> pkt | <n> frames | 60.0 fps (expect 60) | <n> actors
PCD    : <n> chunks | 20.0 fps (expect 20) | <n> complete, <n> partial
Chunks : <n> lost (0.00%) | <n> dup | <n> late | <n> resync | <n> badOffset | <n> malformed
Points : <n> / <n> source (cap <n>)
Frames : <n> published, <n> consumed, <n> dropped, <n> starved
Socket : SO_RCVBUF <n> granted | DrainOverruns <n> | RecvErrors <n> | Unknown <n>
```

`SO_RCVBUF granted` is the figure the kernel actually allowed, not what was requested — the two
differ often enough to matter for burst absorption.

## Behaviour worth knowing

- **The point cloud is not interpolated.** LiDAR points carry no identity between frames, so the i-th
  point of one frame is not the same surface as the i-th point of the next; interpolating positions
  would only smear. The previous frame is held until a new one arrives, so the cloud steps at 20 fps
  while the skeleton moves at 60 fps. That is intended.
- **Partial frames are compacted, not zero filled.** Zeros would pile every missing point onto the
  origin as a visible spike; compaction yields a clean cloud of exactly the points that arrived.
- **`ApplyRotationOffset` is not applied to the point cloud.** It only rewrites bone rotations —
  positions pass through untouched — so it has nothing to do to a point.
- The sender negates X and the reference Python receiver negates it again to reach its own viewer
  space. The plugin's `Unity` branch already assumes X-negated input, so it must not negate again.

## Tests

```bash
UnrealEditor-Cmd.exe <Project>.uproject -unattended -nullrhi \
    -ExecCmds="Automation RunTests MOVIN.Stage; Quit" -TestExit="Automation Test Queue Empty"
```

`MOVIN.Stage.PointCloud.CapturedBytes` runs against datagrams captured from the real MOVIN
Stage server (embedded in `Tests/MOVINOscCapturedPointCloudData.h`) rather than only against bytes
these tests synthesise, so a shared misunderstanding between the parser and its tests cannot pass
unnoticed. `MOVIN.Stage.PointCloud.UdpRoundTrip` sends over a real loopback socket, which is
the only coverage of the drain loop's reliance on `Recv()` returning false as the "queue drained"
signal. `MOVIN.Stage.Network.ExclusiveBind` pins the behaviour the port protection depends on — that
a second bind on a held port is refused when the socket is built without `SO_REUSEADDR` — because
nothing else would notice if that flag came back.
