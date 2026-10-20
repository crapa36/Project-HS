#include <iostream>
#include <string_view>

int RunVfxDecalTests();
int RunVfxDistortionTests();
int RunVfxLightTests();
int RunVfxMeshTests();
int RunVfxUpgradeDispatchTests();

int main(int argc, char **argv)
{
    if (argc == 2)
    {
        const std::string_view name = argv[1];
        if (name == "decal") return RunVfxDecalTests();
        if (name == "distortion") return RunVfxDistortionTests();
        if (name == "light") return RunVfxLightTests();
        if (name == "mesh") return RunVfxMeshTests();
        if (name == "upgrade_dispatch") return RunVfxUpgradeDispatchTests();
    }
    std::cerr << "Expected one VFX case: decal|distortion|light|mesh|upgrade_dispatch\n";
    return 2;
}
