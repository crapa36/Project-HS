# meshoptimizer content-tool subset

- Upstream: https://github.com/zeux/meshoptimizer
- Version: v1.3
- Commit: `9e1f07b159d3cb777f1c67ed31fc11fd117986f4`
- License: MIT; full text in `LICENSE.md`.
- Files are unmodified upstream sources. Index remapping, meshlet building,
  meshlet/sphere bounds, cluster partitioning and attribute-aware simplification
  are compiled into the private geometry cooker.
- Runtime, RenderData and the ordinary low-level build do not link this library.
- Adoption evidence: `Build/tooling/geometry-qualification/REPORT.md` and
  `qualification.json`, including actual selected environment/character assets,
  duplicate and degenerate primitive preservation, and deterministic output.
