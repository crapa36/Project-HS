#include <hs/core/render_snapshot.hpp>

namespace hs
{

RenderSnapshotStorage::RenderSnapshotStorage(std::size_t instance_capacity,
                                             std::size_t pose_capacity,
                                             std::size_t light_capacity,
                                             std::size_t ui_capacity)
{
    instances_.reserve(instance_capacity);
    poses_.reserve(pose_capacity);
    lights_.reserve(light_capacity);
    ui_.reserve(ui_capacity);
}

void RenderSnapshotStorage::Clear() noexcept
{
    instances_.clear();
    poses_.clear();
    lights_.clear();
    ui_.clear();
}

bool RenderSnapshotStorage::AddInstance(const RenderInstance &instance)
{
    if (instances_.size() == instances_.capacity())
    {
        return false;
    }
    instances_.push_back(instance);
    return true;
}

bool RenderSnapshotStorage::AddPose(const AnimationPoseRef &pose)
{
    if (poses_.size() == poses_.capacity())
    {
        return false;
    }
    poses_.push_back(pose);
    return true;
}

bool RenderSnapshotStorage::AddLight(const LightView &light)
{
    if (lights_.size() == lights_.capacity())
    {
        return false;
    }
    lights_.push_back(light);
    return true;
}

bool RenderSnapshotStorage::AddUi(const UiModel &ui)
{
    if (ui_.size() == ui_.capacity())
    {
        return false;
    }
    ui_.push_back(ui);
    return true;
}

RenderSnapshot RenderSnapshotStorage::View() const noexcept
{
    return {header, instances_, poses_, lights_, ui_, camera};
}

} // namespace hs
