# SystemVisuals 0.3.0 — Item ESP upgrade

## World-projected labels

- Captures **view-projection** + **model** matrices via `glUniformMatrix4fv` (same approach as EntityOutline).
- Arms on **setupActorGlint** for non-player actors and pairs the next model translation with that actor.
- Names from **ActorGetNameTag**.
- Draws labels in **NDC** at the entity with **depth test** (hidden behind walls when depth buffer has geometry).
- Soft **FOV** clip: discards labels outside ~screen and behind camera.

Mod Menu → **Item ESP**: World labels, Depth test, Max distance, Hide players.
