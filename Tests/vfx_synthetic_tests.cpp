#include <iostream>
#include <string_view>

int RunVfxDecalTests();
int RunVfxDistortionTests();
int RunVfxLightTests();
int RunVfxMeshTests();
int RunVfxEntitySurfaceTests();
int RunVfxUpgradeDispatchTests();
int RunVfxLegacyTests();

int main(int argc, char **argv)
{
    if (argc == 2)
    {
        const std::string_view name = argv[1];
        if (name == "decal") return RunVfxDecalTests();
        if (name == "distortion") return RunVfxDistortionTests();
        if (name == "light") return RunVfxLightTests();
        if (name == "mesh") return RunVfxMeshTests();
        if (name == "entity_surface") return RunVfxEntitySurfaceTests();
        if (name == "upgrade_dispatch") return RunVfxUpgradeDispatchTests();
        if (name == "legacy") return RunVfxLegacyTests();
    }
    std::cerr << "Expected one VFX case: decal|distortion|light|mesh|entity_surface|upgrade_dispatch|legacy\n";
    return 2;
}
