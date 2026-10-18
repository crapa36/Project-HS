# Geometry descriptor capacity correction

Generation examined: `9260996f5b461412e6b3019a0d775daa83f729ddb6b16ee6013d1d47b0ff30cd`.

The 30 environment geometry catalogs reserve 90 complete-LOD slots. Archer, six monster slots, projectile and procedural geometry reserve another 9, so hierarchy physical slots begin at 99. Decoding all published hierarchy page directories gives 42 pinned root pages: tree canopy catalogs 3 each, cart 3, four crate/barrel catalogs 2 each, and the remaining 22 catalogs 1 each. HLOD stream pages are not pinned. Required startup slots therefore total 141; the former capacity 128 left only 29 root slots and was insufficient by 13.

CPU page ownership/version arrays, descriptor table sizes, scene descriptor validation and shader page bounds now use the shared RenderData capacity 256 (1025 raw SRVs including the draw-record descriptor). Shader compilation/action identity receives the same public constants. Existing per-page residency accounting, byte budgets, retirement and capacity failure remain enabled. Root pinning and geometry/HLOD contents are unchanged.

The independent byte budget still controls actual geometry allocation. Increasing descriptor address space does not admit all logical hierarchy pages or bypass residency pressure.

Validation: `content.render-package-format` passed, receipt `Build/verification/verify-20261018T011755932Z-7224.json`. Source search found no remaining physical geometry 128/513 hardcoded bounds in RendererD3D12/shaders. Updated native shader compilation and runtime smoke are pending parent integration. Mesh/HLOD recooking is not required by this correction; shader artifacts must be regenerated.
