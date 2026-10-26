#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc != 2 || strcmp(argv[1], "--verify-shipping-content") != 0) return 2;
    FILE *pack = fopen("Content/content.hsp", "rb");
    if (!pack) return 3;
    fclose(pack);
    FILE *failure = fopen("fail.verify", "rb");
    if (failure)
    {
        fclose(failure);
        return 4;
    }
    return 0;
}
