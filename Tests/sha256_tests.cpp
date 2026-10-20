#include <hs/core/sha256.hpp>

#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

std::string Hex(const std::array<std::byte, 32> &digest)
{
    constexpr char digits[] = "0123456789abcdef";
    std::string value;
    for (const auto byte : digest)
    {
        const auto n = std::to_integer<unsigned>(byte);
        value += digits[n >> 4];
        value += digits[n & 15];
    }
    return value;
}

int main()
{
    try
    {
        // Independent SHA-256 known results for byte[i] = i % 251.
        const std::pair<std::size_t, const char *> vectors[]{
        {0, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
        {1, "6e340b9cffb37a989ca544e6bb780a2c78901d3fb33738768511a30617afa01d"},
        {55, "463eb28e72f82e0a96c0a4cc53690c571281131f672aa229e0d45ae59b598b59"},
        {56, "da2ae4d6b36748f2a318f23e7ab1dfdf45acdc9d049bd80e59de82a60895f562"},
        {63, "29af2686fd53374a36b0846694cc342177e428d1647515f078784d69cdb9e488"},
        {64, "fdeab9acf3710362bd2658cdc9a29e8f9c757fcf9811603a8c447cd1d9151108"},
        {65, "4bfd2c8b6f1eec7a2afeb48b934ee4b2694182027e6d0fc075074f2fabb31781"},
        {127, "92ca0fa6651ee2f97b884b7246a562fa71250fedefe5ebf270d31c546bfea976"},
        {128, "471fb943aa23c511f6f72f8d1652d9c880cfa392ad80503120547703e56a2be5"},
        {129, "5099c6a56203f9687f7d33f4bfdf576d31dc91f6b695ecea38b2770c87631135"},
        {1000000, "2c030d49ec131bfbbb446ad21e7a2f12cdb4f2f4f3fda3ac709dd2e68a4646c7"}};
        for (const auto &[size, expected] : vectors)
        {
            std::vector<std::byte> bytes(size);
            for (std::size_t i = 0; i < size; ++i)
                bytes[i] = std::byte(i % 251);
            if (Hex(hs::Sha256(bytes)) != expected)
                throw std::runtime_error("SHA-256 known-vector mismatch");
            for (const auto chunk : {1u, 7u, 55u, 64u, 65u, 4096u})
            {
                hs::Sha256Stream stream;
                stream.Add({});
                for (std::size_t offset = 0; offset < size; offset += chunk)
                    stream.Add(std::span(bytes).subspan(offset, std::min<std::size_t>(chunk, size - offset)));
                if (Hex(stream.Finish()) != expected)
                    throw std::runtime_error("SHA-256 chunked digest mismatch");
                bool rejected = false;
                try { stream.Add({}); }
                catch (const std::logic_error &) { rejected = true; }
                if (!rejected)
                    throw std::runtime_error("finished SHA-256 stream accepted more input");
                rejected = false;
                try { (void)stream.Finish(); }
                catch (const std::logic_error &) { rejected = true; }
                if (!rejected)
                    throw std::runtime_error("SHA-256 stream finalized twice");
            }
        }
        const std::string abc = "abc";
        if (Hex(hs::Sha256(std::as_bytes(std::span(abc)))) !=
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")
            throw std::runtime_error("SHA-256 abc vector mismatch");
        std::cout << "SHA-256 known vectors, padding boundaries and streaming equivalence passed\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
