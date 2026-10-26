#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "action.hpp"
#include <hs/asset_manifest/generation_lock.hpp>
#include <hs/core/process_info.hpp>

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#else
#include <hs/core/sha256.hpp>
#include <cerrno>
#include <fcntl.h>
#include <sys/random.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <format>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <system_error>

namespace hs::asset_pipeline
{
std::string DiagnosticPath(const fs::path &path)
{
    try
    {
        const auto utf8 = path.u8string();
        return {reinterpret_cast<const char *>(utf8.data()), utf8.size()};
    }
    catch (const std::system_error &)
    {
        return "<path cannot be encoded as UTF-8>";
    }
}

void ReportFailure(std::string_view producer_id, std::string_view reason,
                   const Json &context, std::string_view error_code)
{
    Json diagnostic = context;
    diagnostic["schema_version"] = 1;
    diagnostic["error_code"] = error_code;
    diagnostic["producer_id"] = producer_id;
    diagnostic["reason"] = reason;
    diagnostic["remediation"] =
        "Check the reported producer inputs and dependencies, correct the failure, and rerun the producer.";
    std::cerr << diagnostic.dump(-1, ' ', false, Json::error_handler_t::replace) << '\n';
}

namespace
{
void Require(bool condition, const std::string &message)
{
    if (!condition)
        throw std::runtime_error("asset action: " + message);
}

#ifdef _WIN32
using UtcTime = SYSTEMTIME;
UtcTime UtcNow() { UtcTime value{}; GetSystemTime(&value); return value; }
auto ProcessId() { return GetCurrentProcessId(); }
auto TickCount() { return GetTickCount64(); }
#else
using UtcTime = std::chrono::system_clock::time_point;
UtcTime UtcNow() { return std::chrono::system_clock::now(); }
auto ProcessId() { return getpid(); }
auto TickCount()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
#endif

struct InvocationClock
{
    UtcTime started_utc = UtcNow();
    std::chrono::steady_clock::time_point started_steady =
        std::chrono::steady_clock::now();
};

const InvocationClock invocation_clock;

std::string UtcTimestamp(const UtcTime &time)
{
#ifdef _WIN32
    return std::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:03}Z",
                       time.wYear, time.wMonth, time.wDay, time.wHour,
                       time.wMinute, time.wSecond, time.wMilliseconds);
#else
    const auto day = std::chrono::floor<std::chrono::days>(time);
    const std::chrono::year_month_day date(day);
    const std::chrono::hh_mm_ss clock(std::chrono::floor<std::chrono::milliseconds>(time - day));
    return std::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:03}Z", int(date.year()), unsigned(date.month()),
                       unsigned(date.day()), clock.hours().count(), clock.minutes().count(),
                       clock.seconds().count(), clock.subseconds().count());
#endif
}

std::string UtcFilename(const UtcTime &time)
{
#ifdef _WIN32
    return std::format("{:04}{:02}{:02}T{:02}{:02}{:02}{:03}Z",
                       time.wYear, time.wMonth, time.wDay, time.wHour,
                       time.wMinute, time.wSecond, time.wMilliseconds);
#else
    const auto day = std::chrono::floor<std::chrono::days>(time);
    const std::chrono::year_month_day date(day);
    const std::chrono::hh_mm_ss clock(std::chrono::floor<std::chrono::milliseconds>(time - day));
    return std::format("{:04}{:02}{:02}T{:02}{:02}{:02}{:03}Z", int(date.year()), unsigned(date.month()),
                       unsigned(date.day()), clock.hours().count(), clock.minutes().count(),
                       clock.seconds().count(), clock.subseconds().count());
#endif
}

#ifdef _WIN32
class Sha256
{
  public:
    Sha256()
    {
        Require(BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_SHA256_ALGORITHM,
                                            nullptr, 0) >= 0,
                "cannot open SHA-256 provider");
        try
        {
            DWORD length = 0;
            DWORD returned = 0;
            Require(BCryptGetProperty(algorithm_, BCRYPT_OBJECT_LENGTH,
                                      reinterpret_cast<PUCHAR>(&length), sizeof(length),
                                      &returned, 0) >= 0 && returned == sizeof(length),
                    "cannot query SHA-256 provider");
            object_.resize(length);
            Require(BCryptCreateHash(algorithm_, &hash_, object_.data(), length,
                                     nullptr, 0, 0) >= 0,
                    "cannot create SHA-256 hash");
        }
        catch (...)
        {
            BCryptCloseAlgorithmProvider(algorithm_, 0);
            throw;
        }
    }
    Sha256(const Sha256 &) = delete;
    Sha256 &operator=(const Sha256 &) = delete;
    ~Sha256()
    {
        if (hash_)
            BCryptDestroyHash(hash_);
        if (algorithm_)
            BCryptCloseAlgorithmProvider(algorithm_, 0);
    }
    void Add(const void *data, std::size_t count)
    {
        const auto *bytes = static_cast<const std::uint8_t *>(data);
        while (count)
        {
            const auto chunk = static_cast<ULONG>(std::min<std::size_t>(count, 1u << 20));
            Require(BCryptHashData(hash_, const_cast<PUCHAR>(bytes), chunk, 0) >= 0,
                    "SHA-256 update failed");
            bytes += chunk;
            count -= chunk;
        }
    }
    void AddInteger(std::uint64_t value)
    {
        std::array<std::uint8_t, 8> bytes{};
        for (std::size_t i = 0; i < bytes.size(); ++i)
            bytes[i] = static_cast<std::uint8_t>(value >> (i * 8));
        Add(bytes.data(), bytes.size());
    }
    void AddString(std::string_view value)
    {
        AddInteger(value.size());
        Add(value.data(), value.size());
    }
    std::string Finish()
    {
        std::array<std::uint8_t, 32> bytes{};
        Require(BCryptFinishHash(hash_, bytes.data(), static_cast<ULONG>(bytes.size()), 0) >= 0,
                "SHA-256 finalization failed");
        constexpr char hex[] = "0123456789abcdef";
        std::string result = "sha256:";
        result.reserve(71);
        for (const auto byte : bytes)
        {
            result += hex[byte >> 4];
            result += hex[byte & 15];
        }
        return result;
    }

  private:
    BCRYPT_ALG_HANDLE algorithm_ = nullptr;
    BCRYPT_HASH_HANDLE hash_ = nullptr;
    std::vector<std::uint8_t> object_;
};

#else
class Sha256
{
  public:
    void Add(const void *data, std::size_t count)
    {
        hash_.Add({static_cast<const std::byte *>(data), count});
    }
    void AddInteger(std::uint64_t value)
    {
        std::array<std::uint8_t, 8> bytes{};
        for (std::size_t i = 0; i < bytes.size(); ++i)
            bytes[i] = static_cast<std::uint8_t>(value >> (i * 8));
        Add(bytes.data(), bytes.size());
    }
    void AddString(std::string_view value)
    {
        AddInteger(value.size());
        Add(value.data(), value.size());
    }
    std::string Finish()
    {
        constexpr char hex[] = "0123456789abcdef";
        std::string result = "sha256:";
        for (const auto byte : hash_.Finish())
        {
            const auto value = std::to_integer<unsigned>(byte);
            result += hex[value >> 4];
            result += hex[value & 15];
        }
        return result;
    }
  private:
    hs::Sha256Stream hash_;
};
#endif

// Windows' legacy MAX_PATH limit applies to filesystem status and CRT file
// opens independently of the path stored in an Action. Use an extended native
// path only at the OS boundary; input identities and depfiles keep their
// original paths.
fs::path NativeFilePath(const fs::path &path)
{
#ifdef _WIN32
    auto value = fs::absolute(path).lexically_normal().make_preferred().native();
    if (value.starts_with(L"\\\\?\\"))
        return fs::path(std::move(value));
    if (value.starts_with(L"\\\\"))
        return fs::path(L"\\\\?\\UNC\\" + value.substr(2));
    return fs::path(L"\\\\?\\" + value);
#else
    return path;
#endif
}

bool IsReparsePoint(const fs::path &path)
{
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(NativeFilePath(path).c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES)
    {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
            return false;
        Require(false, "cannot inspect path: " + path.string());
    }
    return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    std::error_code error;
    const auto status = fs::symlink_status(path, error);
    if (error == std::errc::no_such_file_or_directory)
        return false;
    Require(!error, "cannot inspect path: " + path.string() + ": " + error.message());
    return fs::is_symlink(status);
#endif
}

void RejectReparseAncestors(const fs::path &path)
{
    fs::path prefix;
    for (const auto &part : path)
    {
        prefix /= part;
        Require(!IsReparsePoint(prefix), "reparse point is forbidden: " + prefix.string());
    }
}

bool IsSafeName(std::string_view name)
{
    if (name.empty() || name == "." || name == ".." ||
        name.find_first_of("/\\:") != std::string_view::npos)
        return false;
    const fs::path path(name);
    return path.filename() == path && !path.has_root_path();
}

bool IsActionId(std::string_view value)
{
    return !value.empty() && value.size() <= 128 && value != "." && value != ".." &&
           std::ranges::all_of(value, [](unsigned char ch) {
               return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                      (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.';
           });
}

void ValidateNamespace(std::string_view name)
{
    const fs::path path(name);
    Require(!path.empty() && !path.has_root_path(), "invalid action namespace");
    for (const auto &part : path)
        Require(IsSafeName(part.string()), "unsafe action namespace");
}

std::uint64_t AddSize(std::uint64_t size, std::size_t amount)
{
    Require(size <= std::numeric_limits<std::uint64_t>::max() - amount,
            "file exceeds supported size");
    return size + amount;
}

std::pair<std::string, std::uint64_t> HashFile(const fs::path &path)
{
    RequireRegularFile(path);
    std::ifstream input(NativeFilePath(path), std::ios::binary);
    Require(input.is_open(), "cannot read file: " + path.string());
    Sha256 hash;
    std::array<char, 64 * 1024> buffer{};
    std::uint64_t size = 0;
    while (input)
    {
        input.read(buffer.data(), buffer.size());
        const auto count = input.gcount();
        if (count <= 0)
            break;
        hash.Add(buffer.data(), static_cast<std::size_t>(count));
        size = AddSize(size, static_cast<std::size_t>(count));
    }
    Require(input.eof() && !input.bad(), "cannot read complete file: " + path.string());
    return {hash.Finish(), size};
}

std::string EscapeDepfile(const fs::path &path)
{
    const auto utf8 = path.generic_u8string();
    const std::string generic(reinterpret_cast<const char *>(utf8.data()), utf8.size());
    std::string escaped;
    for (const char ch : generic)
    {
        Require(ch != '\n' && ch != '\r', "newline in depfile path");
        if (ch == ' ' || ch == '#' || ch == ':' || ch == '\\')
            escaped += '\\';
        else if (ch == '$')
            escaped += '$';
        escaped += ch;
    }
    return escaped;
}

fs::path Utf8Path(std::string_view value)
{
    const auto *begin = reinterpret_cast<const char8_t *>(value.data());
    return fs::path(std::u8string(begin, begin + value.size()));
}

bool IsDigest(std::string_view digest)
{
    if (!digest.starts_with("sha256:") || digest.size() != 71)
        return false;
    return std::ranges::all_of(digest.substr(7), [](const char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
    });
}

bool IsProductSetId(std::string_view id)
{
    if (id.size() != 64)
        return false;
    return std::ranges::all_of(id, [](const char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
    });
}

void ValidateActionResult(const Json &result, std::string_view namespace_path,
                          std::map<std::string, std::size_t> &product_indexes)
{
    Require(result.at("schema_version") == 1 &&
            result.at("action_id").is_string(), "upstream ActionResult schema mismatch");
    Require(IsActionId(result.at("action_id").get<std::string>()),
            "upstream ActionResult has an invalid flat action ID");
    if (result.contains("namespace_path"))
        Require(result.at("namespace_path") == std::string(namespace_path),
                "upstream ActionResult namespace mismatch");
    else
        Require(namespace_path == "audio" && result.at("action_id") == "audio",
                "upstream ActionResult namespace is missing");
    const auto id = result.at("product_set_id").get<std::string>();
    Require(IsProductSetId(id), "invalid upstream ProductSetId");
    const auto &products = result.at("products");
    Require(products.is_array(), "invalid upstream products");
    for (std::size_t index = 0; index < products.size(); ++index)
    {
        const auto &product = products.at(index);
        const auto &logical_id = product.at("logical_id");
        if (!logical_id.is_string())
            continue;
        Require(product_indexes.emplace(logical_id.get<std::string>(), index).second,
                "duplicate upstream product logical ID");
    }
}

void VerifyPublished(const fs::path &directory, std::span<const Product> expected)
{
    RequireDirectory(directory);
    std::size_t count = 0;
    for (const auto &entry : fs::directory_iterator(directory))
    {
        RequireRegularFile(entry.path());
        ++count;
    }
    Require(count == expected.size(), "published ProductSet has unexpected files");
    for (const auto &product : expected)
    {
        const auto [digest, size] = HashFile(directory / product.filename);
        Require(digest == product.digest && size == product.size,
                "published ProductSet differs from expected bytes: " + product.filename);
    }
}

void Quarantine(const fs::path &published, const fs::path &build_root,
                std::string_view namespace_path, std::string_view id)
{
    RequireDirectory(published);
    const auto root = build_root / "CorruptProducts" / namespace_path;
    MakeDirectory(root);
    for (unsigned i = 0; i < 1024; ++i)
    {
        const auto target = root / (std::string(id) + "-" +
            std::to_string(ProcessId()) + "-" +
            std::to_string(TickCount()) + "-" + std::to_string(i));
        RejectReparseAncestors(target);
        if (fs::exists(target))
            continue;
        std::error_code error;
#ifdef _WIN32
        fs::rename(published, target, error);
#else
        if (renameat2(AT_FDCWD, published.c_str(), AT_FDCWD, target.c_str(), RENAME_NOREPLACE) != 0)
            error = std::error_code(errno, std::generic_category());
#endif
        if (!error)
            return;
        Require(!fs::exists(published),
                "cannot quarantine corrupt ProductSet: " + error.message());
        Require(false, "ProductSet disappeared during quarantine");
    }
    Require(false, "cannot allocate ProductSet quarantine path");
}

fs::path ReceiptDirectory(const fs::path &result_path)
{
    Require(result_path.filename() == "result.json", "invalid ActionResult receipt path");
    const auto action_directory = result_path.parent_path();
    auto actions = action_directory;
    while (!actions.empty() && actions.filename() != "Actions")
    {
        if (actions.parent_path() == actions)
            break;
        actions = actions.parent_path();
    }
    Require(actions.filename() == "Actions", "ActionResult receipt path is outside Actions");
    const auto namespace_path = action_directory.lexically_relative(actions);
    ValidateNamespace(namespace_path.generic_string());
    return actions.parent_path() / "ActionReceipts" / namespace_path;
}

Json ExecutionReceipt(std::string_view outcome, const Json &current,
                      const Json &previous, const UtcTime &ended_utc,
                      std::chrono::steady_clock::time_point ended_steady)
{
    const bool had_previous = previous.is_object();
    const Json previous_key = had_previous ? previous.at("action_key") : Json(nullptr);
    const Json previous_set = had_previous ? previous.at("product_set_id") : Json(nullptr);
    return {
        {"schema_version", 1},
        {"action_id", current.at("action_id")},
        {"action_key", current.at("action_key")},
        {"product_set_id", current.at("product_set_id")},
        {"outcome", std::string(outcome)},
        {"previous_action_key", previous_key},
        {"previous_product_set_id", previous_set},
        {"started_utc", UtcTimestamp(invocation_clock.started_utc)},
        {"ended_utc", UtcTimestamp(ended_utc)},
        {"elapsed_ms", std::chrono::duration_cast<std::chrono::milliseconds>(
                           ended_steady - invocation_clock.started_steady).count()},
        {"semantic_delta", {
            {"action_key_changed", had_previous ? Json(previous_key != current.at("action_key"))
                                                : Json(nullptr)},
            {"product_set_changed", had_previous ? Json(previous_set != current.at("product_set_id"))
                                                 : Json(nullptr)},
        }},
        {"scheduler_reason", nullptr},
    };
}

void WriteExecutionReceipt(const fs::path &result_path, std::string_view outcome,
                           const Json &current, const Json &previous)
{
    const UtcTime ended_utc = UtcNow();
    const auto ended_steady = std::chrono::steady_clock::now();
    const auto receipt = ExecutionReceipt(outcome, current, previous,
                                          ended_utc, ended_steady).dump(2) + "\n";
    const auto directory = ReceiptDirectory(result_path);
    MakeDirectory(directory);
    constexpr char hex[] = "0123456789abcdef";
    for (unsigned attempt = 0; attempt < 32; ++attempt)
    {
        std::array<unsigned char, 16> nonce{};
#ifdef _WIN32
        Require(BCryptGenRandom(nullptr, nonce.data(), static_cast<ULONG>(nonce.size()),
                                BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0,
                "cannot generate receipt filename");
#else
        std::size_t filled = 0;
        while (filled < nonce.size())
        {
            const auto count = getrandom(nonce.data() + filled, nonce.size() - filled, 0);
            if (count < 0 && errno == EINTR)
                continue;
            Require(count > 0, "cannot generate receipt filename");
            filled += static_cast<std::size_t>(count);
        }
#endif
        std::string suffix;
        suffix.reserve(nonce.size() * 2);
        for (const auto byte : nonce)
        {
            suffix += hex[byte >> 4];
            suffix += hex[byte & 15];
        }
        const auto name = UtcFilename(ended_utc) + "-" +
            std::to_string(ProcessId()) + "-" + suffix + ".json";
        const auto final_path = directory / name;
        const auto temporary = directory / ("." + name + ".tmp");
        RejectReparseAncestors(temporary);
#ifdef _WIN32
        const HANDLE reservation = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0,
                                                nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL,
                                                nullptr);
        if (reservation == INVALID_HANDLE_VALUE)
        {
            const auto error = GetLastError();
            if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS)
                continue;
            Require(false, "cannot reserve receipt file: " + temporary.string());
        }
        CloseHandle(reservation);
#else
        const int reservation = open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
        if (reservation < 0)
        {
            if (errno == EEXIST)
                continue;
            Require(false, "cannot reserve receipt file: " + temporary.string());
        }
        Require(close(reservation) == 0, "cannot close reserved receipt file");
#endif
        try
        {
            WriteBytes(temporary, std::as_bytes(std::span(receipt.data(), receipt.size())));
#ifdef _WIN32
            if (MoveFileExW(temporary.c_str(), final_path.c_str(), MOVEFILE_WRITE_THROUGH))
                return;
            const auto error = GetLastError();
            if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS)
                Require(false, "cannot atomically publish receipt: " + final_path.string());
#else
            if (renameat2(AT_FDCWD, temporary.c_str(), AT_FDCWD, final_path.c_str(), RENAME_NOREPLACE) == 0)
                return;
            Require(errno == EEXIST, "cannot atomically publish receipt: " + final_path.string());
#endif
        }
        catch (...)
        {
            std::error_code error;
            fs::remove(temporary, error);
            throw;
        }
        std::error_code error;
        fs::remove(temporary, error);
    }
    Require(false, "cannot allocate unique receipt filename");
}

void TryWriteExecutionReceipt(const fs::path &result_path, std::string_view outcome,
                              const Json &current, const Json &previous) noexcept
{
    try { WriteExecutionReceipt(result_path, outcome, current, previous); }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "asset action: receipt unavailable: %s\n", error.what());
    }
    catch (...) { std::fprintf(stderr, "asset action: receipt unavailable\n"); }
}
} // namespace

struct Sha256Stream::Impl
{
    Sha256 hash;
};

Sha256Stream::Sha256Stream() : impl_(std::make_unique<Impl>()) {}
Sha256Stream::~Sha256Stream() = default;
Sha256Stream::Sha256Stream(Sha256Stream &&) noexcept = default;
Sha256Stream &Sha256Stream::operator=(Sha256Stream &&) noexcept = default;

void Sha256Stream::Add(std::span<const std::byte> bytes)
{
    Require(impl_ != nullptr, "SHA-256 stream has finished");
    impl_->hash.Add(bytes.data(), bytes.size());
}

std::string Sha256Stream::Finish()
{
    Require(impl_ != nullptr, "SHA-256 stream has finished");
    auto digest = impl_->hash.Finish();
    impl_.reset();
    return digest;
}

void RequireRegularFile(const fs::path &path)
{
    RejectReparseAncestors(path);
    Require(fs::is_regular_file(fs::symlink_status(NativeFilePath(path))),
            "missing regular file: " + path.string());
}

void RequireDirectory(const fs::path &path)
{
    RejectReparseAncestors(path);
    Require(fs::is_directory(fs::symlink_status(NativeFilePath(path))),
            "missing directory: " + path.string());
}

void MakeDirectory(const fs::path &path)
{
    RejectReparseAncestors(path);
    fs::create_directories(path);
    RequireDirectory(path);
}

std::string Sha256File(const fs::path &path) { return HashFile(path).first; }

std::string Sha256Bytes(std::span<const std::byte> bytes)
{
    Sha256 hash;
    hash.Add(bytes.data(), bytes.size());
    return hash.Finish();
}

std::string ReadBytes(const fs::path &path)
{
    RequireRegularFile(path);
    std::ifstream input(NativeFilePath(path), std::ios::binary | std::ios::ate);
    Require(input.is_open(), "cannot read file: " + path.string());
    const auto size = input.tellg();
    Require(size >= 0, "cannot size file: " + path.string());
    std::string bytes(static_cast<std::size_t>(size), '\0');
    input.seekg(0);
    Require(static_cast<bool>(input.read(bytes.data(), static_cast<std::streamsize>(size))),
            "cannot read complete file: " + path.string());
    return bytes;
}

Input HashInput(std::string logical_id, fs::path path)
{
    return {std::move(logical_id), path, Sha256File(path)};
}

Input CopyAndHashInput(std::string logical_id, const fs::path &source,
                       const fs::path &target)
{
    RequireRegularFile(source);
    RejectReparseAncestors(target);
    std::ifstream input(NativeFilePath(source), std::ios::binary);
    std::ofstream output(NativeFilePath(target), std::ios::binary | std::ios::trunc);
    Require(input.is_open() && output.is_open(), "cannot copy input: " + source.string());
    Sha256 hash;
    std::array<char, 64 * 1024> buffer{};
    while (input)
    {
        input.read(buffer.data(), buffer.size());
        const auto count = input.gcount();
        if (count <= 0)
            break;
        output.write(buffer.data(), count);
        Require(output.good(), "cannot write staged file: " + target.string());
        hash.Add(buffer.data(), static_cast<std::size_t>(count));
    }
    Require(input.eof() && !input.bad(), "cannot read input: " + source.string());
    output.close();
    Require(output.good(), "cannot finalize staged file: " + target.string());
    return {std::move(logical_id), source, hash.Finish()};
}

Product HashProduct(std::string logical_id, std::string filename,
                    const fs::path &path)
{
    Require(IsSafeName(filename), "unsafe product filename: " + filename);
    const auto [digest, size] = HashFile(path);
    return {std::move(logical_id), std::move(filename), digest, size, {}};
}

std::string ProductSetId(std::span<const Product> products)
{
    Sha256 hash;
    hash.AddString("project-hs.product-set");
    hash.AddInteger(2);
    hash.AddInteger(products.size());
    for (const auto &product : products)
    {
        hash.AddString(product.logical_id);
        hash.AddString(product.filename);
        hash.AddString(product.digest);
        hash.AddInteger(product.size);
        if (!product.dependencies.empty())
        {
            hash.AddString("product-dependencies");
            hash.AddInteger(product.dependencies.size());
            for (const auto &dependency : product.dependencies)
                hash.AddString(dependency);
        }
    }
    return hash.Finish().substr(7);
}

namespace
{
struct ProducerRecipe
{
    std::string identity;
    std::vector<fs::path> dependencies;
};

const ProducerRecipe &CurrentProducerRecipe()
{
    // The linked executable captures implementation changes without re-reading source dependencies.
    static const ProducerRecipe recipe = [] {
        ProducerRecipe result;
        const auto executable = CurrentExecutablePath();
        result.dependencies = {executable};
        result.identity = Sha256File(executable);
        return result;
    }();
    return recipe;
}
} // namespace

std::string ActionKey(const ActionSpec &spec)
{
    Sha256 hash;
    hash.AddString("project-hs.action-key");
    hash.AddInteger(2);
    hash.AddString(spec.action_id);
    hash.AddString(spec.recipe_id);
    hash.AddInteger(spec.recipe_version);
    hash.AddString(CurrentProducerRecipe().identity);
    hash.AddInteger(spec.inputs.size());
    for (const auto &input : spec.inputs)
    {
        hash.AddString(input.logical_id);
        hash.AddString(input.digest);
    }
    if (!spec.upstream.empty())
    {
        hash.AddInteger(spec.upstream.size());
        for (const auto &upstream : spec.upstream)
        {
            hash.AddString(upstream.logical_id);
            hash.AddString(upstream.digest);
            hash.AddInteger(upstream.size);
        }
    }
    return hash.Finish();
}

ActionResultSnapshot::ActionResultSnapshot(const fs::path &build_root,
                                           std::string_view namespace_path)
    : build_root_(build_root),
      result_path_(build_root / "Actions" / namespace_path / "result.json"),
      namespace_path_(namespace_path)
{
    ValidateNamespace(namespace_path_);
    result_ = Json::parse(ReadBytes(result_path_));
    ValidateActionResult(result_, namespace_path_, product_indexes_);
}

std::string_view ActionResultSnapshot::ActionId() const
{
    return result_.at("action_id").get_ref<const std::string &>();
}

std::string_view ActionResultSnapshot::ActionKeyValue() const
{
    return result_.at("action_key").get_ref<const std::string &>();
}

std::size_t ActionResultSnapshot::ProductCount() const
{
    return result_.at("products").size();
}

bool ActionResultSnapshot::HasProduct(std::string_view product_id) const
{
    return product_indexes_.contains(std::string(product_id));
}

const Json &ActionResultSnapshot::ProductMetadata(std::string_view product_id) const
{
    const auto found = product_indexes_.find(std::string(product_id));
    Require(found != product_indexes_.end(),
            "upstream product is missing from ActionResult: " + std::string(product_id));
    return result_.at("products").at(found->second);
}
std::vector<std::string_view> ActionResultSnapshot::ProductIds() const
{
    std::vector<std::string_view> ids;
    ids.reserve(product_indexes_.size());
    for (const auto &[id, index] : product_indexes_) { (void)index; ids.push_back(id); }
    return ids;
}
void ActionResultSnapshot::CopyProductBytesTo(std::string_view product_id, std::ostream &output,
                                             UpstreamProduct &dependency, std::span<char> scratch) const
{
    fs::path path;
    ResolveProduct(product_id, dependency, path);
    std::vector<char> local;
    if (scratch.empty()) { local.resize(1024 * 1024); scratch = local; }
    CopyFileBytesTo(path, output, dependency.size, dependency.digest, scratch);
}

void CopyFileBytesTo(const fs::path &path, std::ostream &output, std::uint64_t expected_size,
                     std::string_view expected_digest, std::span<char> scratch)
{
    Require(!scratch.empty() && scratch.size() <= static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max()),
            "invalid streaming copy scratch extent");
    RequireRegularFile(path);
    std::ifstream input(NativeFilePath(path), std::ios::binary);
    Require(input.is_open(), "cannot read product: " + path.string());
    Sha256 hash;
    std::uint64_t size{};
    while (input)
    {
        input.read(scratch.data(), static_cast<std::streamsize>(scratch.size()));
        const auto count = input.gcount();
        if (count <= 0) break;
        size = AddSize(size, static_cast<std::size_t>(count));
        Require(size <= expected_size, "upstream product exceeds declared size");
        hash.Add(scratch.data(), static_cast<std::size_t>(count));
        output.write(scratch.data(), count);
        Require(static_cast<bool>(output), "cannot write streamed product");
    }
    Require(input.eof() && !input.bad() && size == expected_size && hash.Finish() == expected_digest,
            "upstream product is missing or corrupt: " + path.string());
}

void AtomicPublishFile(const fs::path &temporary, const fs::path &destination)
{
    RequireRegularFile(temporary);
    RejectReparseAncestors(destination);
#ifdef _WIN32
    Require(MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0,
            "cannot atomically publish file: " + destination.string());
#else
    fs::rename(temporary, destination);
#endif
}

void ActionResultSnapshot::ResolveProduct(std::string_view product_id,
                                       UpstreamProduct &dependency,
                                       fs::path &product_path) const
{
    const auto &product = ProductMetadata(product_id);
    const auto id = result_.at("product_set_id").get<std::string>();
    const auto locator = product.at("locator").get<std::string>();
    const auto expected = (fs::path("Products") / namespace_path_ / id /
                           fs::path(locator).filename()).generic_string();
    Require(locator == expected, "upstream product locator mismatch");
    product_path = build_root_ / fs::path(locator);
    dependency = {std::string(product_id), product.at("digest").get<std::string>(),
                  product.at("size").get<std::uint64_t>(), result_path_};
    Require(IsDigest(dependency.digest), "invalid upstream product digest");
}

void ActionResultSnapshot::ReadProduct(std::string_view product_id,
                                      UpstreamProduct &dependency,
                                      fs::path &product_path) const
{
    ResolveProduct(product_id, dependency, product_path);
    const auto [digest, size] = HashFile(product_path);
    Require(digest == dependency.digest && size == dependency.size,
            "upstream product is missing or corrupt: " + std::string(product_id));
}

std::string ActionResultSnapshot::ReadProductBytes(std::string_view product_id,
                                                  UpstreamProduct &dependency) const
{
    fs::path path;
    ResolveProduct(product_id, dependency, path);
    auto bytes = ReadBytes(path);
    Require(bytes.size() == dependency.size &&
                Sha256Bytes(std::as_bytes(std::span(bytes.data(), bytes.size()))) == dependency.digest,
            "upstream product is missing or corrupt: " + std::string(product_id));
    return bytes;
}

Json ReadActionResult(const fs::path &build_root, std::string_view namespace_path,
                      std::string_view product_id, UpstreamProduct &dependency,
                      fs::path &product_path)
{
    ActionResultSnapshot snapshot(build_root, namespace_path);
    snapshot.ReadProduct(product_id, dependency, product_path);
    return std::move(snapshot.result_);
}

void WriteBytes(const fs::path &path, std::span<const std::byte> bytes)
{
    RejectReparseAncestors(path);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    Require(output.is_open(), "cannot write staged file: " + path.string());
    output.write(reinterpret_cast<const char *>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    output.close();
    Require(output.good(), "cannot finalize staged file: " + path.string());
}

void AtomicWriteIfDifferent(const fs::path &path, std::string_view bytes)
{
    RejectReparseAncestors(path);
    if (fs::exists(path) && ReadBytes(path) == bytes)
        return;
    const auto temporary = path.parent_path() /
        ("." + path.filename().string() + "." +
         std::to_string(ProcessId()) + "." +
         std::to_string(TickCount()) + ".tmp");
    RejectReparseAncestors(temporary);
    try
    {
        WriteBytes(temporary, std::as_bytes(std::span(bytes.data(), bytes.size())));
#ifdef _WIN32
        Require(MoveFileExW(temporary.c_str(), path.c_str(),
                            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0,
                "cannot atomically publish metadata: " + path.string());
#else
        // Same-filesystem rename gives atomic visibility, not power-loss durability.
        fs::rename(temporary, path);
#endif
    }
    catch (...)
    {
        std::error_code error;
        fs::remove(temporary, error);
        throw;
    }
}

void TouchActionResult(const fs::path &path)
{
    // Preserve the stable output mtime so Ninja restat can prune downstream work.
    RequireRegularFile(path);
    try
    {
        const auto current = Json::parse(ReadBytes(path));
        TryWriteExecutionReceipt(path, "current", current, current);
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "asset action: receipt unavailable: %s\n", error.what());
    }
}

fs::path CreateStage(const fs::path &actions)
{
    MakeDirectory(actions);
    for (unsigned i = 0; i < 1024; ++i)
    {
        const auto path = actions /
            (".stage-" + std::to_string(ProcessId()) + "-" +
             std::to_string(TickCount()) + "-" + std::to_string(i));
        RejectReparseAncestors(path);
        std::error_code error;
        if (fs::create_directory(path, error))
            return path;
        Require(!error || error == std::errc::file_exists,
                "cannot create staging directory: " + error.message());
    }
    Require(false, "cannot allocate staging directory");
    return {};
}

void RemoveStage(const fs::path &stage)
{
    if (fs::exists(stage))
    {
        RejectReparseAncestors(stage);
        std::error_code error;
        fs::remove_all(stage, error);
        Require(!error, "cannot remove staging directory: " + error.message());
    }
}

void Publish(const fs::path &build_root, const ActionSpec &spec,
             const fs::path &stage, std::vector<Product> products)
{
    Require(IsActionId(spec.action_id), "invalid flat action ID");
    ValidateNamespace(spec.namespace_path);
    Require(build_root.is_absolute(), "build root must be absolute");
    RequireDirectory(stage);
    hs::GenerationFileLock mutation(build_root / ".generation-mutation.lock",
        hs::GenerationFileLock::Mode::Exclusive, true);
    std::ranges::sort(products, {}, &Product::logical_id);
    std::set<std::string> filenames;
    for (std::size_t index = 0; index < products.size(); ++index)
    {
        auto &product = products[index];
        Require(IsSafeName(product.filename) && IsDigest(product.digest),
                "invalid product metadata");
        Require(filenames.insert(product.filename).second,
                "duplicate product filename: " + product.filename);
        Require(index == 0 || products[index - 1].logical_id != product.logical_id,
                "duplicate product logical ID: " + product.logical_id);
        std::ranges::sort(product.dependencies);
        Require(std::ranges::all_of(product.dependencies,
                                    [&](const std::string &dependency) {
                                        return !dependency.empty() &&
                                               dependency != product.logical_id;
                                    }) &&
                    std::adjacent_find(product.dependencies.begin(),
                                       product.dependencies.end()) == product.dependencies.end(),
                "invalid product dependencies: " + product.logical_id);
    }
    std::vector<Input> inputs = spec.inputs;
    std::ranges::sort(inputs, {}, &Input::logical_id);
    auto upstream = spec.upstream;
    std::ranges::sort(upstream, {}, &UpstreamProduct::logical_id);
    ActionSpec canonical = spec;
    canonical.inputs = inputs;
    canonical.upstream = upstream;
    const auto key = ActionKey(canonical);
    const auto id = ProductSetId(products);
    const auto actions = build_root / "Actions" / spec.namespace_path;
    const auto products_root = build_root / "Products" / spec.namespace_path;
    MakeDirectory(actions);
    MakeDirectory(products_root);
    const auto published = products_root / id;
    const auto result_path = actions / "result.json";
    Json previous = nullptr;
    if (fs::exists(result_path))
    {
        const auto old = Json::parse(ReadBytes(result_path));
        previous = old;
        Require(old.at("action_key") != key || old.at("product_set_id") == id,
                "same ActionKey produced a different ProductSetId");
        if (old.at("product_set_id") == id)
        {
            const auto &old_products = old.at("products");
            Require(old_products.is_array() && old_products.size() == products.size(),
                    "same ProductSetId has different product metadata");
            for (std::size_t index = 0; index < products.size(); ++index)
            {
                const auto &product = products[index];
                const auto locator = (fs::path("Products") / spec.namespace_path / id /
                                      product.filename).generic_string();
                const auto &entry = old_products[index];
                Require(entry.at("logical_id") == product.logical_id &&
                            entry.at("locator") == locator &&
                            entry.at("digest") == product.digest &&
                            entry.at("size") == product.size &&
                            (entry.contains("dependencies") ?
                                 entry.at("dependencies") : Json::array()) ==
                                Json(product.dependencies),
                        "same ProductSetId has different product metadata");
            }
        }
    }
    // Schema2 readers resolve into ProductSets; retain exclusive manifest leases
    // through repair so their healthy sibling paths cannot disappear mid-read.
    std::vector<hs::GenerationFileLock> legacy_repair_leases;
    if (fs::exists(published))
    {
        try { VerifyPublished(published, products); }
        catch (const std::exception &)
        {
            const auto generations = build_root / "Generations";
            if (fs::exists(generations))
            {
                RequireDirectory(generations);
                for (const auto &entry : fs::directory_iterator(generations))
                {
                    if (!IsProductSetId(entry.path().filename().string())) continue;
                    RequireDirectory(entry.path());
                    const auto manifest_path = entry.path() / "generation.json";
                    if (!fs::exists(manifest_path)) continue; // Already retired by GC/repair.
                    const auto manifest = Json::parse(ReadBytes(manifest_path));
                    Require(manifest.at("schema_version").is_number_integer(),
                            "invalid Generation schema during ProductSet repair");
                    if (manifest.at("schema_version") != 2) continue;
                    Require(manifest.at("products").is_array() && !manifest.at("products").empty(),
                            "invalid Generation products during ProductSet repair");
                    for (const auto &product : manifest.at("products"))
                    {
                        const auto text = product.at("locator").get<std::string>();
                        const fs::path locator{std::u8string(text.begin(), text.end())};
                        if (!fs::equivalent(build_root / locator.parent_path(), published)) continue;
                        legacy_repair_leases.emplace_back(manifest_path,
                            hs::GenerationFileLock::Mode::TryExclusive);
                        Require(legacy_repair_leases.back().OwnsLock(),
                                "generation-in-use: cannot repair ProductSet used by an active schema2 reader");
                        break;
                    }
                }
            }
            Quarantine(published, build_root, spec.namespace_path, id);
        }
    }
    if (!fs::exists(published))
    {
        std::error_code error;
#ifdef _WIN32
        fs::rename(stage, published, error);
#else
        if (renameat2(AT_FDCWD, stage.c_str(), AT_FDCWD, published.c_str(), RENAME_NOREPLACE) != 0)
            error = std::error_code(errno, std::generic_category());
#endif
        if (error)
        {
            Require(fs::exists(published), "cannot publish ProductSet: " + error.message());
            VerifyPublished(published, products);
        }
    }
    std::string depfile = EscapeDepfile(result_path) + ":";
    for (const auto &input : inputs)
        depfile += " " + EscapeDepfile(input.path);
    for (const auto &entry : upstream)
        depfile += " " + EscapeDepfile(entry.result_path);
    for (const auto &path : CurrentProducerRecipe().dependencies)
        depfile += " " + EscapeDepfile(path);
    depfile += '\n';
    AtomicWriteIfDifferent(actions / "result.d", depfile);

    Json result = {
        {"schema_version", 1},
        {"action_id", spec.action_id},
        {"action_key", key},
        {"recipe_id", spec.recipe_id},
        {"recipe_version", spec.recipe_version},
        {"inputs", Json::array()},
        {"upstream_product_dependencies", Json::array()},
        {"product_set_id", id},
        {"products", Json::array()},
    };
    if (spec.include_namespace_field)
        result["namespace_path"] = spec.namespace_path;
    result["recipe_identity"] = CurrentProducerRecipe().identity;
    for (const auto &input : inputs)
        result["inputs"].push_back({{"logical_id", input.logical_id},
                                     {"digest", input.digest}});
    for (const auto &entry : upstream)
        result["upstream_product_dependencies"].push_back(
            {{"logical_id", entry.logical_id}, {"digest", entry.digest},
             {"size", entry.size}});
    for (const auto &product : products)
    {
        const auto locator = (fs::path("Products") / spec.namespace_path / id /
                              product.filename).generic_string();
        Json entry = {{"logical_id", product.logical_id},
                      {"locator", locator},
                      {"digest", product.digest},
                      {"size", product.size}};
        if (!product.dependencies.empty())
            entry["dependencies"] = product.dependencies;
        result["products"].push_back(std::move(entry));
    }
    AtomicWriteIfDifferent(result_path, result.dump(2) + "\n");
    TryWriteExecutionReceipt(result_path, "produced", result, previous);
}

} // namespace hs::asset_pipeline
