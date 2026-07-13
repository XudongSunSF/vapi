#ifndef WFMCM_MODEL_SESSION_DATA_H
#define WFMCM_MODEL_SESSION_DATA_H

#include <src/app-common/api/WfmcmApiComponents.h>

#include <memory>
#include <utility>
#include <vector>

namespace app::vasara {

/**
 * Tier 1 (service-level) model configuration.
 *
 * Populated inside createModelSession: byte payloads at construction, then
 * modelMapSpec_ once the behavioral map spec has been parsed and registered.
 * After createModelSession returns, the instance is frozen -- no member may
 * be mutated -- and may be cached per worker / evaluator and read
 * concurrently (contexts hold it via shared_ptr<const ModelSessionData>).
 *
 * Handle ownership: the behavioral model map spec handle is created natively
 * inside createModelSession and is never returned to Java, so it is NOT in
 * the Java-side handle-teardown stack. It is therefore RAII-owned here via a
 * shared_ptr whose deleter (installed by createModelSession) removes it from
 * the HandleContainer when the last owner is destroyed -- i.e. when the
 * session handle is deleted after all request-context copies (Java's LIFO
 * teardown deletes contexts first). Copies of this struct (one per request
 * context) share that ownership; copying remains cheap-correct, though it
 * duplicates the byte payloads.
 *
 * NOTE: this type is stored in HandleData's std::any, which requires a
 * copy-constructible type -- do not make it move-only.
 */
struct ModelSessionData
{
    std::vector<char> modelParams_;
    std::vector<char> modelMapCsv_;
    /**
     * Shared owner of the behavioral model map spec handle; empty when the
     * session has no behavioral map. Deleter installed by createModelSession.
     */
    std::shared_ptr<const Handle> modelMapSpec_;

    /** Spec handle value, or the null handle (internal_ == 0) when absent. */
    Handle modelMapSpecHandle() const noexcept
    {
        return modelMapSpec_ ? *modelMapSpec_ : Handle{};
    }

    ModelSessionData() = default;

    ModelSessionData(std::vector<char> modelParams, std::vector<char> modelMapCsv)
        : modelParams_(std::move(modelParams))
        , modelMapCsv_(std::move(modelMapCsv))
    {}
};

} // namespace app::vasara

#endif // WFMCM_MODEL_SESSION_DATA_H
