# Brixelizer native noise source

Exact source copy from FidelityFX SDK 1.1.4 commit
`c6efa6bf7f2027b3ec94f28578bb5965eabb9e55`,
`samples/thirdparty/samplercpp/samplerBlueNoiseErrorDistribution_128x128_OptimizedFor_2d2d2d2d_8spp.cpp`.
The original source notice and FidelityFX license are retained.

The content producer evaluates the original sampler at each 128 by 128 pixel,
sample indices 0 through 15, dimensions 0 and 1. RGBA16F stores its exact
`(0.5 + value) / 256` values in R/G, with B zero and A one. These are two
optimized sampling dimensions from the actual Sobol/ranking/scrambling tables.
They are spatial blue noise samples; no temporal STBN guarantee is asserted.

This file is an authoritative content input. Runtime reads only the sixteen
selected immutable texture products, never the source tables or SDK cache.
