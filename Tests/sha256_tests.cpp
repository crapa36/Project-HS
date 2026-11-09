#include <hs/core/sha256.hpp>

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

static std::string Hex(const std::array<std::byte, 32> &digest)
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

void TestSha256()
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
    {255, "857df204175f077a9986709897f00ee0bcc0449585248e4b42498337e9329999"},
    {256, "5bc31b283cef0072274e97d74916552954c935794536cab632641e5ea071379d"},
    {257, "a2c6cad2ffa699b14538231fef914f45d30440389e6f8d79091efba836165a2b"},
    {511, "96781741ba50e7280e4d5eab7cf2cfed6b0eeb98d27d235611e689322b6b6d0f"},
    {512, "d86e386278a71782a283f96aae4f4e7437471abef71136bd2811f98245488d89"},
    {513, "c2525d534358baf2020a2560015aa2adbd23d94eeec6455c24092aafdd8cf1b5"},
    {1000000, "2c030d49ec131bfbbb446ad21e7a2f12cdb4f2f4f3fda3ac709dd2e68a4646c7"}};
    for (const auto &[size, expected] : vectors)
    {
        std::vector<std::byte> bytes(size);
        for (std::size_t i = 0; i < size; ++i)
            bytes[i] = std::byte(i % 251);
        if (Hex(hs::Sha256(bytes)) != expected)
            throw std::runtime_error("SHA-256 known-vector mismatch");
        for (const auto chunk : {1u, 7u, 55u, 64u, 65u, 255u, 256u, 257u, 4096u})
        {
            hs::Sha256Stream stream;
            stream.Add({});
            for (std::size_t offset = 0; offset < size; offset += chunk)
                stream.Add(std::span(bytes).subspan(offset, std::min<std::size_t>(chunk, size - offset)));
            if (Hex(stream.Finish()) != expected)
                throw std::runtime_error("SHA-256 chunked digest mismatch");

        }
    }
    {
        hs::Sha256Stream finished;
        (void)finished.Finish();
        auto copied = finished;
        bool rejected_add{}, rejected_finish{}, rejected_copy{};
        try { finished.Add({}); } catch (const std::logic_error &) { rejected_add = true; }
        try { (void)finished.Finish(); } catch (const std::logic_error &) { rejected_finish = true; }
        try { (void)copied.Finish(); } catch (const std::logic_error &) { rejected_copy = true; }
        if (!rejected_add || !rejected_finish || !rejected_copy)
            throw std::runtime_error("finished SHA-256 stream accepted an operation");
    }
    {
        std::vector<std::byte> bytes(1025);
        for (std::size_t i = 0; i < bytes.size(); ++i)
            bytes[i] = std::byte(i % 251);
        const std::array initial{std::byte{0xaa}};
        const std::array moved_from_input{std::byte{0x42}};
        for (const auto prefix_size : {1u, 255u, 256u, 257u, 511u, 513u})
        {
            const auto expected = hs::Sha256(bytes);
            const auto prefix = std::span(bytes).first(prefix_size);
            const auto suffix = std::span(bytes).subspan(prefix_size);
            const auto add_prefix = [&](hs::Sha256Stream &stream) {
                for (std::size_t offset = 0; offset < prefix.size(); offset += 7)
                    stream.Add(prefix.subspan(offset, std::min<std::size_t>(7, prefix.size() - offset)));
            };

            hs::Sha256Stream original;
            add_prefix(original);
            auto copy_constructed = original;
            hs::Sha256Stream copy_assigned;
            copy_assigned.Add(initial);
            copy_assigned = original;
            original.Add(suffix);
            copy_constructed.Add(suffix);
            copy_assigned.Add(suffix);
            if (original.Finish() != expected || copy_constructed.Finish() != expected ||
                copy_assigned.Finish() != expected)
                throw std::runtime_error("SHA-256 copied pending state differs");

            hs::Sha256Stream move_source;
            add_prefix(move_source);
            auto move_constructed = std::move(move_source);
            move_constructed.Add(suffix);
            if (move_constructed.Finish() != expected)
                throw std::runtime_error("SHA-256 moved pending state differs");
            move_source.Add(moved_from_input);
            if (move_source.Finish() != hs::Sha256(moved_from_input))
                throw std::runtime_error("moved-from SHA-256 stream retained pending input");

            hs::Sha256Stream move_assignment_source;
            add_prefix(move_assignment_source);
            hs::Sha256Stream move_assigned;
            move_assigned.Add(initial);
            move_assigned = std::move(move_assignment_source);
            move_assigned.Add(suffix);
            if (move_assigned.Finish() != expected)
                throw std::runtime_error("SHA-256 move-assigned pending state differs");
            move_assignment_source.Add(moved_from_input);
            if (move_assignment_source.Finish() != hs::Sha256(moved_from_input))
                throw std::runtime_error("move-assigned-from SHA-256 stream retained pending input");
        }
    }
    const std::string abc = "abc";
    if (Hex(hs::Sha256(std::as_bytes(std::span(abc)))) !=
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")
        throw std::runtime_error("SHA-256 abc vector mismatch");
}
