# meshoptimizer subset

- Upstream: https://github.com/zeux/meshoptimizer
- Version: v1.3
- Commit: `9e1f07b159d3cb777f1c67ed31fc11fd117986f4`
- License: MIT; full text in `LICENSE.md`.
- Files are unmodified upstream sources. Index remapping, meshlet building,
  meshlet/sphere bounds, cluster partitioning and attribute-aware simplification
  are compiled into the private geometry cooker.
- `vertexcodec.cpp` from the same pin supplies lossless four-byte shadow-page
  encoding/decoding through RenderData's private `hs_meshoptimizer_codec` target.
  Geometry algorithms remain cooker-only. The ordinary low-level build does not
  link either target; portable RenderData tests can use the CPU codec.
- Codec source SHA-256:
  `6af26a85ed38daffb97d988e9704ffc257c690af05048304044bfc786699550f`.
- Adoption evidence: `Build/tooling/geometry-qualification/REPORT.md` and
  `qualification.json`, including actual selected environment/character assets,
  duplicate and degenerate primitive preservation, and deterministic output.
