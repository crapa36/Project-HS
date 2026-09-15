# SlimeFamily source assets

See `Docs/SLIME_FAMILY_REPORT.md` at the repository root for source/runtime mapping, verification evidence, preserved legacy assets, and outstanding human/target-hardware acceptance.

`author_slime_family.py` defines `build()`, `export()`, and `studio()` for Blender. The saved `SlimeFamily.blend` retains current scenes (`.002`) and earlier authored revisions. Regeneration creates new scenes instead of deleting existing work. Three enemy rigs share 14 controls; the independent gel projectile uses a single root.

Only FBX, animation FBX and source PNG inputs are authoritative for cooking. Do not edit Build/Cooked outputs as source. BaseColor alpha controls translucency; Normal alpha stores roughness. Runtime DirectX UV convention flips the sampled tangent normal Y channel.