#include <iostream>
#include <string_view>

int RunVfxFlashTests();
int RunVfxBallisticTests();
int RunVfxMoteTests();
int RunVfxGroundTests();
int RunVfxFresnelTests();
int RunVfxRibbonTests();

int main(int argc, char **argv)
{
    if (argc == 2)
    {
        const std::string_view name = argv[1];
        if (name == "flash") return RunVfxFlashTests();
        if (name == "ballistic") return RunVfxBallisticTests();
        if (name == "mote") return RunVfxMoteTests();
        if (name == "ground") return RunVfxGroundTests();
        if (name == "fresnel") return RunVfxFresnelTests();
        if (name == "ribbon") return RunVfxRibbonTests();
    }
    std::cerr << "Expected one VFX case: flash|ballistic|mote|ground|fresnel|ribbon\n";
    return 2;
}
