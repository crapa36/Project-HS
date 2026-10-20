#pragma once

#include <filesystem>
#include <memory>

namespace hs
{
// Cooperating readers/GC lock an existing manifest without modifying it.
// Mutation locks use one stable file at the build root, never an atomic pointer.
// The OS releases locks on process death; no PID/TTL cleanup or write access to
// an installed Generation is needed. Direct edits and power-loss durability
// remain outside this cooperative contract.
class GenerationFileLock
{
public:
    enum class Mode { Shared, Exclusive, TryExclusive };
    GenerationFileLock() noexcept;
    GenerationFileLock(const std::filesystem::path &path, Mode mode, bool create = false);
    ~GenerationFileLock();
    GenerationFileLock(GenerationFileLock &&) noexcept;
    GenerationFileLock &operator=(GenerationFileLock &&) noexcept;
    GenerationFileLock(const GenerationFileLock &) = delete;
    GenerationFileLock &operator=(const GenerationFileLock &) = delete;
    [[nodiscard]] bool OwnsLock() const noexcept;
private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace hs
