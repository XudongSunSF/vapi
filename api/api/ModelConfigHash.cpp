#include <src/app-common/api/ModelConfigHash.h>
#include <src/app-common/api/ModelConfigData.h>
#include <src/app-common/HashMix.h>

#include <functional>
#include <string_view>
#include <vector>

namespace app::vasara {

namespace {

std::size_t hashBytes(const std::vector<char>& bytes) noexcept
{
    // Guard the empty case: vector::data() may be nullptr, and forming a
    // string_view from a null pointer is not well-defined even with size 0.
    if (bytes.empty()) {
        return 0;
    }
    return std::hash<std::string_view>{}(
        std::string_view(bytes.data(), bytes.size()));
}

} // namespace

std::size_t hashModelConfig(const ModelConfigData& config) noexcept
{
    std::size_t h = hashBytes(config.modelParams_);
    h = mixHash(h, hashBytes(config.modelMapCsv_));
    return h;
}

} // namespace app::vasara
