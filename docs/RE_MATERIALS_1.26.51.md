# RenderDragon material RE — MCPE 1.26.51 (ARM64)

## Uniform / flag names in libminecraftpe.so

- `HideGlowOutline`
- `ITEM_IN_HAND_EDGE_BRIGHTNESS` / `_TIGHTNESS` / `_SHARPNESS`
- `ENTITY_EDGE_BRIGHTNESS` / `_TIGHTNESS` / `_SHARPNESS` / `_LOD_SCALAR`
- `ItemInHandColor` / `ItemInHandColorGlint`
- `.material.bin`
- `Could not find specified material`

## Code xrefs (file offset == VA for this SO)

| String | Example code VA |
|--------|-----------------|
| HideGlowOutline | 0x106300d4, 0x10630bec, 0x10630c54 |
| ItemInHandColor | 0x10fe280c … |
| ItemInHandColorGlint | 0x10fe2a28 … |
| .material.bin | 0x11363b50, 0x113a0b74 |
| Could not find specified material | 0xf24b4d0 |

## Signatures added (SignatureId)

1. **HideGlowOutlineQuery** — frame near HideGlowOutline use (~0x1063007c)
2. **ItemInHandShaderSetup** — ItemInHandColor family (~0x10fe2650)
3. **MaterialBinPathBuilder** — .material.bin path (~0x11363a84)
4. **MaterialMissingError** — missing material path (~0xf24b4d0)
5. Existing **RenderMaterialGroupCommon**

## Phase plan

1. Resolve-only (1.16.0) — confirm addresses in status.txt  
2. Read-only hooks — log calls, do not change args  
3. MaterialUniformOverrides / ENTITY_EDGE_* write  
4. CreateMaterialImmediate + embed material.bin  
5. MaterialFilter swap on actor / ItemInHand draws  

## Note

Vibrant Visuals edge uniforms may only apply when that pipeline is active.  
Glow outline may be gated by `HideGlowOutline` option bit.

## Phase 3 findings (EDGE registry)

Uniform **name registry** (not direct ADRP xrefs to strings):

| File offset | Content |
|-------------|---------|
| 0xdcae78 | ptr → `ITEM_IN_HAND_EDGE_BRIGHTNESS` |
| 0xdcae90 | ptr → `ITEM_IN_HAND_EDGE_TIGHTNESS` |
| 0xdcaea8 | ptr → `ITEM_IN_HAND_EDGE_SHARPNESS` |
| 0xdcaec0 | ptr → `ENTITY_EDGE_BRIGHTNESS` |
| … | ENTITY_EDGE_TIGHTNESS / SHARPNESS / LOD_SCALAR |

Layout pattern: `{ reloc_slot, 0x403, c_str_va }` repeating.

Rela.dyn maps hashed-string objects at VA `0x12b8bc10+`.

No code ADRP directly to EDGE C-strings in scanned ranges — values applied via **MaterialUniformOverrides** + registry, not string literals in draw code.

`HideGlowOutlineQuery` resolves but **never called** in user sessions (glow option path inactive).

Phase3 runtime: dump floats on ItemInHandShaderSetup args to locate override slots.

## Phase 3 experiment result (FAILED)

ItemInHandShaderSetup args:
- a0 stable heap object, mostly zeros + [8]=1.0
- a1 [2]=1.012 [3]=0.744 looked edge-like but writing produced garbage
  (readback 1.6e16) — **not float uniform slots**, likely mixed pointer/matrix memory
- HideGlowOutlineQuery never invoked at runtime

Conclusion: Color* glint swaps crash; float poking ItemInHand args does nothing useful.
Real chams require MaterialFilter / CreateMaterialImmediate + material.bin (Stray-style).
