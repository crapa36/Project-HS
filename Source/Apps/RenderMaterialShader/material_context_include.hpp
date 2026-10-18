#pragma once
// Included inside each producer namespace after its Require/SemanticInput helpers.
std::uint32_t AddSurfaceReflectionInputs(ap::ActionSpec &spec, const fs::path &source_root)
{
    std::uint32_t mask{};
    const auto add = [&](std::string_view name) {
        const auto path = source_root / ("ContentSource/Materials/" + std::string(name) + ".json");
        const auto reflection = rd::AnalyzeClosure(hs::material_content::ParseMaterialDefinition(name, ap::ReadBytes(path)).graph);
        mask |= reflection.varying_mask;
        const auto bytes = rd::EncodeMaterialSurfaceLayout({reflection.capability_mask, reflection.varying_mask});
        spec.inputs.push_back(SemanticInput("material/" + std::string(name) + "/surface-reflection", path, bytes));
    };
    for (const auto name : rd::kAuthoredMaterialNames) add(name);
    for (const auto name : rd::kHeroMaterialNames) add(name);
    return mask;
}

std::string HeroContextInclude(ap::ActionSpec &spec, const fs::path &source_root)
{
    std::string generated = "#include \"material_surface.hlsli\"\n";
    for (std::size_t ordinal = 0; ordinal < rd::kHeroMaterialNames.size(); ++ordinal)
    {
        const auto name = rd::kHeroMaterialNames[ordinal];
        const auto path = source_root / ("ContentSource/Materials/" + std::string(name) + ".json");
        const auto graph = hs::material_content::ParseMaterialDefinition(name, ap::ReadBytes(path)).graph;
        const auto functions = rd::EmitHeroMaterialContextHlsl(graph,
            "HsHeroEvaluateSurface_" + std::to_string(ordinal), "HsHeroEvaluateOpacity_" + std::to_string(ordinal));
        generated += functions;
        spec.inputs.push_back(SemanticInput("material/" + std::string(name) + "/hero-context-ir", path,
            std::as_bytes(std::span(functions.data(), functions.size()))));
    }
    generated += "HsSurfaceClosure HsHeroEvaluateSurface(HsMaterialContext ctx, uint ordinal)\n{\n    switch (ordinal)\n    {\n";
    for (std::size_t ordinal = 0; ordinal < rd::kHeroMaterialNames.size(); ++ordinal)
        generated += "    case " + std::to_string(ordinal) + "u: return HsHeroEvaluateSurface_" + std::to_string(ordinal) + "(ctx);\n";
    generated += "    default: return (HsSurfaceClosure)0;\n    }\n}\n";
    generated += "float HsHeroEvaluateOpacity(HsMaterialContext ctx, uint ordinal)\n{\n    switch (ordinal)\n    {\n";
    for (std::size_t ordinal = 0; ordinal < rd::kHeroMaterialNames.size(); ++ordinal)
        generated += "    case " + std::to_string(ordinal) + "u: return HsHeroEvaluateOpacity_" + std::to_string(ordinal) + "(ctx);\n";
    return generated + "    default: return 0.0;\n    }\n}\n";
}



std::string DeferredContextInclude(ap::ActionSpec &spec, const fs::path &source_root)
{
    std::string generated = "#include \"material_surface.hlsli\"\n";
    for (std::size_t ordinal = 0; ordinal < rd::kAuthoredMaterialNames.size(); ++ordinal)
    {
        const std::string name(rd::kAuthoredMaterialNames[ordinal]);
        const auto path = source_root / ("ContentSource/Materials/" + name + ".json");
        const auto definition = hs::material_content::ParseMaterialDefinition(name, ap::ReadBytes(path));
        const auto reflection = rd::AnalyzeClosure(definition.graph);
        (void)rd::EncodeMaterialSurfaceLayout({reflection.capability_mask, reflection.varying_mask});
        const auto function = rd::EmitUniformClosureHlsl(definition.graph,
            "HsMaterialApplyUniformLayers_" + std::to_string(ordinal), reflection.varying_mask);
        generated += function;
        spec.inputs.push_back(SemanticInput("material/" + name + "/uniform-closure-ir", path,
            std::as_bytes(std::span(function.data(), function.size()))));
    }
    generated += "HsSurfaceClosure HsMaterialApplyDeferredLayers(HsSurfaceClosure base, "
                 "HsMaterialContext ctx, uint ordinal)\n{\n    switch (ordinal)\n    {\n";
    for (std::size_t ordinal = 0; ordinal < rd::kAuthoredMaterialNames.size(); ++ordinal)
        generated += "    case " + std::to_string(ordinal) +
            "u: return HsMaterialApplyUniformLayers_" + std::to_string(ordinal) + "(base, ctx);\n";
    generated += "    default: return base;\n    }\n}\n";
    for (std::size_t ordinal = 0; ordinal < rd::kHeroMaterialNames.size(); ++ordinal)
    {
        const auto name = rd::kHeroMaterialNames[ordinal];
        const auto path = source_root / ("ContentSource/Materials/" + std::string(name) + ".json");
        const auto graph = hs::material_content::ParseMaterialDefinition(name, ap::ReadBytes(path)).graph;
        const auto reflection = rd::AnalyzeClosure(graph);
        const auto function = rd::EmitUniformClosureHlsl(graph,
            "HsHeroApplyUniformLayers_" + std::to_string(ordinal), reflection.varying_mask);
        generated += function;
        spec.inputs.push_back(SemanticInput("material/" + std::string(name) + "/uniform-closure-ir", path,
            std::as_bytes(std::span(function.data(), function.size()))));
    }
    generated += "HsSurfaceClosure HsHeroApplyDeferredLayers(HsSurfaceClosure base, HsMaterialContext ctx, uint ordinal)\n{\n    switch (ordinal)\n    {\n";
    for (std::size_t ordinal = 0; ordinal < rd::kHeroMaterialNames.size(); ++ordinal)
        generated += "    case " + std::to_string(ordinal) + "u: return HsHeroApplyUniformLayers_" + std::to_string(ordinal) + "(base, ctx);\n";
    generated += "    default: return base;\n    }\n}\n";
    return generated;
}
