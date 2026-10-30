#pragma section(".hsimage", read)
#include <fstream>
#include <string_view>

struct Header { unsigned char bytes[64]; };
constexpr Header MakeHeader()
{
    Header header{};
    header.bytes[0] = 0x48;
    header.bytes[1] = 0x53;
    header.bytes[48] = 64;
    header.bytes[56] = 64;
    return header;
}
__declspec(allocate(".hsimage")) extern const Header shipping_image_header = MakeHeader();

int main(int argc, char **argv)
{
    if (argc != 4 || std::string_view(argv[1]) != "--write-pack") return 1;
    Header header = shipping_image_header;
    if (std::string_view(argv[3]) == "mismatch") ++header.bytes[0];
    std::ofstream output(argv[2], std::ios::binary);
    output.write(reinterpret_cast<const char *>(&header), sizeof(header));
    return output ? 0 : 2;
}
