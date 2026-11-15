#include <cstdio>
#include <exception>

extern void TestShippingImage();

int main()
{
    try
    {
        TestShippingImage();
        return 0;
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
