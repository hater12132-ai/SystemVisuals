# Actor-selectable chams (community path, 1.26.x)

Source: Discord show-and-tell (alteik) — "actor-selectable proper-working non-fps-raping chams for 1.26.50"

## Approach (not GLES)
- Pure **game function hooks / mid-hooks**
- No OpenGL post-process
- Actor filtering via **RenderParams** fields

## RenderParams (decl dump, size 0x200)
| Off | Field |
|-----|--------|
| 0x00 | BaseActorRenderContext* |
| 0x38 | Actor* mActor |
| 0x48 | Actor* mPlayer |
| 0xa0 | ActorRenderData* |
| 0x108 | float mParams[8] |
| 0x1e8 | Flags |

## Related
- `ActorAnimationPlayer::applyToPose(ApplyAnimationContext&, RenderParams&, …)` (virtual)
- PaperDollBaseActorRenderContext = inventory dummy (filter out)

## SystemVisuals
- setupActorGlint still used for Color* attempt
- Experimental: poke entityContext+0x108 as mParams when actor selected
- Next: find function that takes RenderParams& and mid-hook after actor fill
