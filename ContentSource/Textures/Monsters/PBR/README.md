# PBR material mapping

Every original FBX under `ContentSource/Archive/Monsters/2026-08-24-unreal-export/Mesh/PBR` contains an assigned material named `DefaultPBR_MAT`.
Map the textures below to that material. The corresponding source slot in Unreal is named `lambert1`.

| Material input | Texture | Channel / operation |
|---|---|---|
| Base Color | `ContentSource/Textures/Monsters/PBR/BasecolorDefault_TEX.png` | RGB |
| Emissive Color | `ContentSource/Textures/Monsters/PBR/Emissive_TEX.png` | RGB multiplied by 80 |
| Roughness | `ContentSource/Textures/Monsters/PBR/RAM_TEX.png` | R |
| Metallic | `ContentSource/Textures/Monsters/PBR/RAM_TEX.png` | B |
| Ambient Occlusion | None | Not connected in the original Unreal material |

The FBX files retain their original UV coordinates. `material_mapping.json` distinguishes the Unreal source slot (`lambert1`) from the exported FBX material (`DefaultPBR_MAT`) for all eight PBR meshes.
