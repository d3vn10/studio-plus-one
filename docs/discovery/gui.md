# ReSkate Studio (old, closed source) GUI feature inventory

**How this was found:** strings and RTTI symbols in `ReSkate Studio.exe` and `reskate_cli.exe`, plus a read-only look at `%LOCALAPPDATA%\ReSkateStudio` on the test PC.

**Screenshot attempt failed:** I tried a live capture with `--game … --ui-capture <png> --ui-page PROJECT`. It exits 1 over ssh with "creating the DX12 swap chain failed (HRESULT 0x887A0022)", because ssh has no desktop session. I stopped there. A real capture would need a task running in his logged-on session. `settings.db` was backed up first and is unchanged.

**Files on this side:**
- Strings: a scratch folder (`studio_a.txt`, `cli_a.txt`)
- Backup on the test PC: `<test folder>\gui\settings.db.orig`

**Tech:** Dear ImGui 1.91.9b with a DX12 backend (Win32 and WARP fallback), XAudio2 for audio, Media Foundation for WebM, Assimp for FBX/GLB, dxcompiler and dxtex for shaders and textures, and vgmstream plus an EA `sx` encoder in `Native\Audio`.

**Navigation:** the left nav reads STUDIO (logo), ASSETS, COSMETICS, CUSTOM MAPS, PROJECT, SETTINGS. There is no separate MODS page. The mod load order and build/deploy panels live in the project area. Which page they sit on is inferred from string order, since no screenshot was possible. The global top bar has **Export .fbmod** and **Launch** (Launch runs `ReSkateLauncher.exe`) [GUI-ONLY]. There is also an Overview panel showing CPU workers, I/O workers and native asset count [GUI-ONLY].

## First run: agreement screen
- "ReSkate Studio - User Agreement" modal, "First-use agreement / Version %d". It has these sections: YOUR CONTENT AND RESPONSIBILITY, UNOFFICIAL PROJECT, CUSTOM CONTENT AND SERVERS, USE AT YOUR OWN RISK, NO WARRANTY, LIMITATION OF LIABILITY, OTHER LICENSES AND RIGHTS, ACCEPTANCE [GUI-ONLY]
- A checkbox "I have read and agree to the terms above." with the buttons **Decline and Exit** and **Accept and Continue**. The choice is saved to `user-agreement.db` with `version`, `textHash` (SHA-256 of the text), `accepted` and `acceptedAtUtc`. Editing the text forces it to be accepted again [GUI-ONLY]

## ASSETS
- **Connect / index the game:** reads the game folder, checks the Skate.exe SHA-256 and TOC hashes, and builds or loads the `asset-index.bin` / `asset-types.bin` cache. Status shows "CONNECTING / INDEXING NATIVE ASSETS", "CONNECTED / N NATIVE ASSETS" and "Loaded cached native assets (TOC hashes match)". It refuses to change game folder while the project has unsaved changes [CLI partly: `assets`, `validate`, `find-name`, `find-type`; there is no persistent cache]
- **Browser:** a resizable splitter (width is saved), search box "Search name or *type", a kind dropdown, a type filter with its own search, an "Edited only" toggle, a folder tree, and a counter "%zu assets". Kinds are EBX assets, Resources, InitFS files, Levels & sublevels, Chunks, Lua scripts, Shaders, Animations [CLI partly: `find-name`, `find-type`, `find-type-usage`; no filtered tree]
- **Folder right-click "Export":**
  - Mesh to GLB, or Mesh to FBX: LOD0 with rig, weights and textures, subfolders included, existing files skipped [GUI-ONLY]
  - Texture to PNG (highest mip, extra faces and layers go in `<name>.png.layers`), or Texture to DDS (native format with all mips) [GUI-ONLY; CLI does single textures only]
  - Progress, cancel, and an "Export details" issue list [GUI-ONLY]
- **Inspector header:** title, object count, encoded bytes. Buttons are Export raw (`.chunk` / `.ebx`), **Import EBX** (replaces this asset's edits with an exported `.ebx`, 512 MB limit) and Revert asset [CLI: `ebx-raw` exports; import and revert are GUI-ONLY]
- **Inspector tabs:**
  - **PROPERTIES (EBX editor):** an object and instance tree with typed values, a colour swatch and picker for ColorXYZ/ColorValue, and boxed values shown as raw bytes. **Apply edits** / **Discard draft** keep a draft per asset and Ctrl+S saves all drafts [CLI read-only: `ebx-values`, `schema`; editing is GUI-ONLY]
  - INITFS: "Open InitFS editor" (see below)
  - BLUEPRINT (see below)
  - SCRIPT: read-only Lua source with line count [CLI: `lua`]
  - SHADER (see below)
- **Texture preview:** dimensions, mips, format. Export PNG [CLI: `texture-png`], Export DDS [CLI: `texture-dds`], Import PNG / DDS (a DDS needs a DX10 header) [GUI-ONLY]
- **Texture tint:** a Tint/Red/Green/Blue/Brightness/Hue/Saturation colour bar with live preview. "Apply to texture" or "Apply to cosmetic". On RIME textures it is applied to every resolution ("Replace Rime image resolutions") [GUI-ONLY]
- **Mesh (MeshSet) preview:** vertices, triangles, sections, LOD, material-binding warnings, and the "original game shader" vs "compatibility shader" label. Orbit with drag, pan with right or middle drag, zoom with the wheel, **F** to fit [GUI-ONLY; CLI `meshes`, `mesh-sections`, `mesh-bounds` print stats]
- **Mesh export:** FBX (rig, weights, embedded colour and opacity, plus a `.textures` companion folder) or GLB (LOD0, one file) [GUI-ONLY]
- **Replace mesh (FBX/GLB):** joined objects are routed by material name, and a single LOD fills the other native LODs. Options:
  - "Import edited bone transforms": changes the shared rig, and the change shows separately in Modified Resources
  - "Fill missing weights from original mesh"
  - Missing-weight bone override
  - SkeletonAsset or owning-entity override
  - Revert mesh
  
  [GUI-ONLY for normal meshes. CLI only has `cosmetic-mesh-import`, and `mesh-author` is a self-test]
- **Animations:** decode a native clip on its assigned rig, with a player and skateboard preview and a time scrubber. Open an FBX and preview its takes. "Export take to FBX". "Replace clip in project" bakes the take at native FPS and keeps loop, root motion and tags. Revert clip [CLI: `animation-info`, `animation-export`, `animation-import --take N`, `fbx-animation-info`; preview and revert are GUI-ONLY]
- **Levels & Sublevels:** a filtered container list, then a DX12 scene preview of placements with level/placement/mesh/material counts. **Shift-click adds a level to the scene.** **Flight camera:** WASD to move, Q/E down/up, Shift fast, wheel changes speed, right-click or Esc exits [CLI: `levels`, `level-preview` print stats; the viewer and flight camera are GUI-ONLY]
- **Audio and Video:**
  - Audio: NewWave/LocalizedWave with channels, Hz, duration, variations and segments. Timeline, volume, variation picker, Export WAV, Import WAV (512 MiB limit, encoded via sx) [CLI: `audio` gives info only; play, export and import are GUI-ONLY]
  - Video: MovieTexture2/WebM with dimensions, fps, codec and audio tracks. Playback with its own volume, Export WebM [CLI: `webm`, `video` give info and a decode test; playback and export are GUI-ONLY]
- **Lua Scripts:** list with filter, linked LuaAsset EBX metadata, "Open properties", Export .lua [CLI: `lua`]
- **InitFS Editor:** decrypts `Data/initfs_win32` (AES key found in Skate.exe). Filter, file list, text view with Find (Match case, Previous/Next, "%zu / %zu"), Apply text to project, Import file, Export file, Reload archive. Binary files can only be imported or exported. 64 MiB limit [CLI: `initfs-info` lists only; editing is GUI-ONLY]
- **Blueprint viewer:** hierarchy pane, node canvas and node details pane.
  - View options: Group lists, Connections only, Expand, Assets
  - Controls: Arrange, Refresh, Load more, search box
  - Double-click opens a child blueprint or list, and Back returns
  - Details show Identity (Type, Field, Instance GUID, Asset), Properties, Connections and Mapping notes, with "Inspect source asset" and "Inspect resource"
  
  [GUI-ONLY; CLI `ebx-references`, `find-importers`, `ebx-imports` are text only]
- **Shader inspection:**
  - Views: RECORD, DISASSEMBLY (DXIL/LLVM IR), BINDINGS, RAW BYTES (hex dump)
  - Active permutation, "Find shader material and mesh usages" (the usage index is cached), referencing-mesh picker, live native preview ("Render shader on <mesh>", "Native GPU draws n/m")
  - Copy text, Export text, Export bytecode, "Open named engine shaders" (`systems/render/shaderprogramdb`)
  
  [GUI-ONLY; CLI `shader-params` and `material-constants` are dumps only]
- **Drag and drop:** one file at a time. An `.ebx` needs a selected target EBX. A dropped image is assigned to the selected cosmetic's texture slot. Unsupported files get "The dropped file is not supported on this page." [GUI-ONLY]

## COSMETICS
- **Gate:** "ASSET DATABASE REQUIRED" with an OPEN SETUP button. Then "INDEX INSTALLED COSMETICS", cached in `cosmetics.bin` [CLI: `cosmetics`, `cosmetic-audit`]
- **Browser:** search by name, type or path. Type filter with search. Sort by Name A-Z / Z-A or Type A-Z / Z-A. Counter "%zu / %zu cosmetics". Splitter width is saved [GUI-ONLY]
- **Workspace modes:** NEW COSMETIC, PROJECT ITEMS, NATIVE COSTUME
- **3D preview:**
  - Gray mannequin with undershirt and undershorts, plus a "Show player model" toggle (saved)
  - Sock preview shoes
  - Sticker deck preview: tail, middle or nose
  - Mole and tattoo region selectors
  - Hair-colour swatches (preview only)
  - "Material notes" panel
  
  [GUI-ONLY]
- **Editor tab TEXTURES:**
  - Slot list showing texture, replacement and status ("+ Color *", "+ Placement *"). Click to view, adjust colour or export
  - Replace…, Use installed texture… (a TextureAsset path, or the asset selected in Assets), Reset to original, Reset to saved
  - Artwork import and export for sticker, mole, tattoo, player card and graphic (alpha becomes opacity)
  - **Placement editor:** canvas where drag moves, wheel resizes, Shift gives fine moves and Ctrl+click types a value. Fit modes are Original UV / Fit whole image / Fill canvas. Controls: Position, Scale, Rotation, Flip H/V, "Move color and opacity together", Surface aspect, Background, Center, Fit, Reset
  
  [GUI-ONLY; CLI `texture-clone-test` and `cosmetic-texture-probe` are tests]
- **Editor tab APPEARANCE:**
  - Colorways and preset values, "SAVE COLORWAY CHANGES"
  - Skin colour with an override-saturation offset [CLI: `skin-tone-create` from JSON]
  - Region colour override (##CosmeticTint)
  - Override UseGraphic
  - Sumo Bounce gameplay effect toggle
  - Full-body material routing target
  
  [GUI-ONLY except skin tone]
- **Editor tab VISIBILITY:** HidesHeadMesh, HidesBodyMesh, hair cut line (EnforcedCutLine 0-3), body and head region checkboxes (Region 0x…), equipped slot checkboxes, Restore source visibility [GUI-ONLY]
- **Editor tab THUMBNAIL:** "Auto thumbnail on save" (saved setting). Captures with the stock menu camera for each type (headwear ¾, eyewear front, socks by length, deck underside, and so on). Choose picture…, Load menu thumbnail, Export generated thumbnail… [GUI-ONLY; CLI `cosmetic-thumbnail-test` is a test]
- **Editor tab DETAILS:** type, asset, main texture, material section
- **Mesh options:** "Choose model" for hair variants. Import custom mesh (FBX/GLB) with a "replace original" toggle (off means a private mesh for this cosmetic only). Fill-weights bone and SkeletonAsset override. Export cosmetic mesh [CLI: `cosmetic-mesh-import`; export is GUI-ONLY]
- **CREATE NEW COSMETIC** (with a name field) or **SAVE COSMETIC CHANGES** [CLI partly: `cosmetic-create-probe` is a probe; no real create command]
- **PROJECT ITEMS:**
  - Select a project cosmetic, Delete
  - Apply Gameplay Effects (Sumo)
  - Repair Material Routing (from the recorded donor)
  - "Clean unused cosmetic assets…": a review list, then "Remove listed resources"
  
  [GUI-ONLY]
- **NATIVE COSTUME PACKAGE:** pick a folder (`recipe.json`, `mesh.res`, `chunks/<guid>.chunk` with exactly 8 chunks, optional `textures/<category>/texture.{ebx,res,resmeta,chunk}`), then "VALIDATE & ADD TO PROJECT" or Cancel. Dropping a folder selects it [CLI: `native-costume-build`, `native-costume-info`, `native-costume-donors`]

## CUSTOM MAPS
- **"BUILD A MAP" in 3 steps:** Blender scene (.blend or .fbx, BROWSE or drop), Map name, **BUILD & INSTALL**. The step labels are "Step 1 of 3: reading the scene in Blender", "Step 2 of 3: building the level" and "Step 3 of 3: installing into Skate". Shows elapsed time and CANCEL. Step 1 is skipped when the scene is unchanged ("reusing it") [CLI: `compile-map … --deploy`; Build Skate Map.bat does the same]
- **Options:**
  - Time of day: Morning, Afternoon, Evening [CLI: `--time-of-day` also has noon and night]
  - Streaming: Automatic or Off [CLI: `--stream` / `--no-stream`]
  - Chunk size: 200 m or 50 m [CLI: `--cell-size 200|100|50`]
  - Chunk load distance [CLI: `--cell-load-distance`]
  - Detail levels (LODs) toggle [CLI: `--no-lods`; LOD ratios, distances and min triangles are CLI only]
  - "Include my enabled mods" [CLI: pass extra fbmods]
  - Pause map picture "3D with shadows" (used when there is no `assets/<scene>/map.png`) [CLI: `--pause-map 2d|3d`]
  - Map ID / slug (30 characters, lowercase and underscores, sets the Mods folder and level name) [CLI: `--mod-folder`]
- **Not in the GUI, CLI only:** all GI, Enlighten and lighting flags (`--no-gi`, `--enlighten`, `--gi-*`, `--no-bam-lighting`, `--no-live-tod`, `--no-auto-reflection`) and vista flags
- **Analysis summary** after the Blender handoff: authored objects / retail placements, meshes / vertices / triangles, collision, grind curves, materials / alpha / decals, gameplay profiles, lights / TOD lights, ambient audio volumes, audio emitters, trigger effects, VFX placements, pedestrian routes / vehicle lanes, NPC loops, lighting warnings [CLI: `map` and `compile-map` print stats]
- **BUILD LOG** panel with timestamps, a "build notes" warning list, "Installed at …" and the hint 'Start Skate and pick "<name>" from the map list' [GUI-ONLY presentation]
- Separate actions "Import and analyze Blender map" and "Analyze normalized map" [CLI: `map`]
- Empty state "Choose or drop one .blend scene, then import and analyze it. Nothing is written to Skate."

## PROJECT
- **PROJECT DETAILS:** file name (UNTITLED.FBPROJECT), "UNSAVED CHANGES" badge, Open, Save (Ctrl+S, which applies all asset drafts first), SAVE AS, EXPORT .FBMOD (saved edits only), **TEST & LAUNCH / CTRL+T** [CLI: `project-info` and `fbmod-info` read only; save, export and test are GUI-ONLY]
- **MOD METADATA:** title, author, version, description ("Untitled Mod" default) [GUI-ONLY]
- **Project cosmetics** table: Cosmetic, Action, "Clean unused cosmetic assets…" [GUI-ONLY]
- **MODIFIED RESOURCES** list ("%zu resources in this project"). Details show kind, payload bytes, bundles, resource type, RID, added, marker. Actions: Export payload, Import .ebx over it, Remove from project [GUI-ONLY]
- **LOAD ORDER (mods):** ADD .FBMOD, REMOVE, MOVE UP, MOVE DOWN, an enabled checkbox per mod, and a table with Author and Version. The list is saved in settings.db under `mods` [GUI-ONLY; CLI `compile-mod` takes the order as arguments]
- **CHECK CONFLICTS:** "CONFLICT RESOLUTION", "%zu CONFLICTS / %zu WARNINGS", entries shown as winner -> loser, with IDENTICAL marked [CLI partly: `compile-mod` prints `conflicts=`]
- **BUILD & DEPLOY:** staging folder, mod folder name (1-64 characters with rules), **BUILD PATCH** showing "%zu bundles / %zu resources staged", **DEPLOY LAST BUILD**, **UNDO LAST DEPLOY** [CLI: `compile-mod`, `deploy-mod`, `deploy-patch`; undo/restore is GUI-ONLY]
- **"Deploy to Mods" advanced panel:** game root, staging root containing `Patch/layout.toc`, mod name, Deploy, Undo Last Deployment. Messages include "Previous version retained", "Old Patch folder disabled and kept at" and "Previous version restored" [CLI: `deploy-mod`; undo is GUI-ONLY]

## SETTINGS (Setup)
- **SKATE INSTALLATION:** Game folder with CHOOSE, CONNECT / INDEX ASSETS, LAUNCH RESKATE, and "Current and legacy game layouts are detected automatically" [launch is GUI-ONLY]
- **BLENDER AUTHORING TOOLS:**
  - Blender executable with CHOOSE
  - **INSTALL / UPDATE SKATE MAP ADD-ON:** installs `sk8_map_export.py` and writes `sk8_vfx_catalog.json` (native VFX blueprints from the game). Blender confirms back with the SHA-256 of both
  - Status lines "Skate Map %s enabled in Blender %s" and "%zu native VFX blueprints available"
  
  [GUI-ONLY]
- **STUDIO PREFERENCES** with SAVE SETTINGS [GUI-ONLY]
- **BACKGROUND ACTIVITY / BACKGROUND JOBS** table: Status (Queued, Running, Completed, Cancelled, Failed), Progress, Detail [GUI-ONLY]

## Global
- **Keys:**
  - Ctrl+S saves the project and drafts
  - Ctrl+T runs Test & Launch
  - F fits the mesh preview
  - Flight camera: WASD, Q/E, Shift, wheel, right-click/Esc
  - Placement editor: Shift fine-move, Ctrl+click to type a value
- **Close guard:** "A project operation is still running…" and "Save changes to the current project before closing?"
- **Startup options:** `--game <dir>`, `--workspace <dir>`, `--ui-capture <png> --ui-page <page>`, `--ui-cosmetic`, `--ui-asset`. The last two are headless screenshot hooks. Startup errors go to `%TEMP%\ReSkateStudio-startup.log` [GUI-ONLY]
- **File dialog filters:**
  - ReSkate projects `*.fbproject`; Frosty mods `*.fbmod`
  - Texture images `*.png;*.tga;*.jpg;*.jpeg;*.bmp;*.dds`; Artwork with alpha `*.png;*.tga;*.bmp`; Transparent PNG
  - FBX / GLB mesh; FBX animation
  - Lua source; Shader text; Direct3D shader container `*.dxbc`
  - WebM video; Wave audio
  - Blender and FBX scenes `*.blend;*.fbx`
  - Raw resource payload; Executables (Blender)

## Settings and state files
- `%LOCALAPPDATA%\ReSkateStudio\settings.db` is a custom binary key/value store (header `82 EE`, typed entries). Keys: `gameRoot`, `blenderExecutable`, `mapSource`, `mapOutput` (default `…\Builds\Maps`), `mapName`, `mapSlug`, `modStaging` (default `…\Builds\Mods`), `modFolder` (default `ReSkateStudio_Mods`), `assetBrowserWidth`, `cosmeticBrowserWidth`, `cosmeticAutoThumbnail`, `cosmeticShowPlayerModel`, `mods` (list with `enabled`). On the test PC, `gameRoot` currently holds `<ReSkate folder>`.
- `user-agreement.db` holds `version` (2), `textHash`, `accepted` and `acceptedAtUtc`.
- `Cache\`:
  - `asset-index.bin` (about 295 MB) and `asset-types.bin`, tied to the Skate.exe SHA-256 and TOC hashes
  - `cosmetics.bin`
  - `CosmeticThumbnails\`
  - the shader usage index (in memory, per asset database)
- `Builds\Project\CurrentProject.fbmod` plus `Builds\Project\Stage\Patch\…` (staged `layout.toc`, `initfs_win32`, Win32 TOCs and CAS files). `Patch` is only the staging container: deployment copies its contents into `Mods\<name>`, where `layout.toc` and the optional `.reskate-studio-patch` provenance file are at the mod root. The marker records the Skate.exe SHA-256; it does not enumerate mod contents. Other folders include `Builds\Maps`, `Builds\Mods`, `Imports`, `shaders-disassembly`, and `ReSkateStudio\MediaWork` (temporary WebM and WAV).
- Map import output folder: `.reskate-studio-map-import` marker, `.import-lock`, `.pending`, `.previous`, `.building-*`, `map.json`, `conversion-report.json`, `result.json`, `map_image.json`.
- Files written into the game folder:
  - `Mods\<name>\` with `reskate-build.json` (schema, tool, version, map, built) and `reskate-levels.json`
  - `.ReSkateStudio-last-deploy.db`, a receipt with modRoot, backupRoot, retiredPatch and displacedRoot
  - `.frosty-managed-skate-patch`
  - Transaction folders `.ReSkateStudio-Mod-{pending,backup,rollback,failed,removed}-*` and `.ReSkateStudio-Patch-{backup,retired}-*`
  - Mutex `Local\ReSkateStudioPatch_*`
- Deploy refuses when: there are symlinks or junctions, a folder is not owned by Studio, Skate.exe differs from the staged build's SHA, staging sits inside the game tree, or another deploy is running.
- `imgui.ini` is written to the working directory.

## Workflows
1. **First run:** agreement, then Settings: choose the game folder, CONNECT / INDEX ASSETS (cache), choose blender.exe, and INSTALL SKATE MAP ADD-ON (needs indexing first for the full VFX catalog).
2. **Custom map:** drop or browse a .blend, name it, set Options, then BUILD & INSTALL. Blender runs headless (`--background --factory-startup --disable-autoexec --python studio_map_import.py -- --input … --output … --map-name … --map-id … --pause-map … --timeout-seconds`) and writes `map.json`. Then native analysis, compile into a staged Patch, and a transactional install to `Mods\<slug>`. Then pick the map in Skate.
   - CLI equivalent: `compile-map <game> <blend> <staging> --deploy`, or Build Skate Map.bat, which waits for Skate.exe to close.
3. **Asset edit:**
   - Assets: find the asset, then use one of:
     - edit EBX Properties, then Apply edits
     - import PNG/DDS, or tint
     - replace the mesh from FBX/GLB
     - replace the animation from FBX
     - import WAV
     - edit InitFS text
   - Ctrl+S saves the project, then Ctrl+T.
   - Test & Launch: exports the saved project to `CurrentProject.fbmod`, builds it with the enabled mods, deploys to `Mods\ReSkateStudio_Project` and runs `ReSkateLauncher.exe`.
4. **Cosmetic:** pick a donor, edit TEXTURES, APPEARANCE, VISIBILITY, THUMBNAIL and mesh, then CREATE NEW COSMETIC or SAVE COSMETIC CHANGES. Then Ctrl+S, then rebuild or test.
5. **Native costume:** choose a package folder, then VALIDATE & ADD TO PROJECT, then save and build.
6. **Mod pack:** ADD .FBMOD several times, reorder, CHECK CONFLICTS, BUILD PATCH, DEPLOY LAST BUILD, and UNDO LAST DEPLOY to roll back.
7. **Share:** EXPORT .FBMOD (saved edits only).

## Gaps in the CLI (nothing in the CLI covers these)
- Project save and open
- EBX editing and import
- Texture import
- Mesh export and replacement for non-cosmetic meshes
- Audio and video export, WAV import
- InitFS editing
- Blueprint graph
- Shader disassembly and live preview
- Folder batch export
- Cosmetic create, edit, delete and repair
- Visibility and thumbnails
- Load-order management
- Undo deploy
- Launch
- Blender add-on install
- Map analysis summary and logs

The CLI does have test-only commands with no GUI equivalent, such as the `*-probe`, `*-selftest` and `gi-*`/Enlighten tools, `dump-res`, `lighting-scan` and `physics-*`.