#ifndef WFMCM_REQUEST_CONTEXT_DATA_H
#define WFMCM_REQUEST_CONTEXT_DATA_H

#include <src/app-common/api/ModelSessionData.h>
#include <src/app-common/api/WfmcmApiComponents.h>

#include <memory>
#include <utility>
#include <variant>
#include <vector>

namespace app::vasara {

/** Serialized DTO bytes (Grid); parsing not wired yet. */
struct RequestContextBytes
{
    std::vector<char> historicalData_;
    std::vector<char> marketData_;
    Date valuationDate_{};
};

/** Reuses existing API handles on the worker (no fill/move on a request handle). */
struct RequestContextHandles
{
    Handle historicalDataHandle_{};
    Handle dateSpecHandle_{};
    Handle modelOptionsHandle_{};
    Handle marketDataHandle_{};
};

/**
 * Tier 2 (per-batch / per-scenario) request context.
 * Immutable after construction (no member may be mutated once one of the
 * value constructors has run). Referenced by vasara calc* APIs.
 *
 * The payload is a std::variant so the two storage modes are mutually
 * exclusive at compile time: code that reads handle fields cannot
 * accidentally see byte-payload state and vice versa. std::monostate is the
 * first alternative so that a default-constructed context is unambiguously
 * EMPTY rather than silently presenting as an empty bytes-mode payload;
 * callers must check hasBytes()/hasHandles() (or empty()) before use.
 *
 * Invariant: modelSessionHandle_ MUST be the handle under which
 * *modelSession_ is registered in the HandleContainer. Both fields describe
 * the same Tier 1 session; the handle is what crosses the Java/native
 * boundary, the shared_ptr is the in-process fast path. The constructors
 * take them as a pair to keep them from drifting apart — never assign them
 * independently.
 *
 * Handle ownership / lifetime: all Handle members here are NON-OWNING. The
 * Java side records every created Handle in an atomic (LIFO) stack and
 * destroys all of them explicitly at teardown; no GC/finalizer path frees a
 * native handle, and this struct's destructor must not either. Because
 * request-level handles are created after the session handle they refer to,
 * LIFO destruction pops them first, so dependent handles are gone before
 * their session handle is destroyed. If the teardown order ever becomes
 * unordered, deleting a session handle (deleteHandle) must tolerate (or
 * reject) request contexts that still reference it.
 */
struct RequestContextData
{
    Handle modelSessionHandle_{};
    std::shared_ptr<const ModelSessionData> modelSession_;
    std::variant<std::monostate, RequestContextBytes, RequestContextHandles> payload_;

    RequestContextData() = default;

    RequestContextData(
        Handle modelSessionHandle,
        std::shared_ptr<const ModelSessionData> modelSession,
        RequestContextBytes bytes)
        : modelSessionHandle_(modelSessionHandle)
        , modelSession_(std::move(modelSession))
        , payload_(std::move(bytes))
    {}

    RequestContextData(
        Handle modelSessionHandle,
        std::shared_ptr<const ModelSessionData> modelSession,
        RequestContextHandles handles)
        : modelSessionHandle_(modelSessionHandle)
        , modelSession_(std::move(modelSession))
        , payload_(std::move(handles))
    {}

    bool empty() const noexcept
    {
        return std::holds_alternative<std::monostate>(payload_);
    }
    bool hasBytes() const noexcept
    {
        return std::holds_alternative<RequestContextBytes>(payload_);
    }
    bool hasHandles() const noexcept
    {
        return std::holds_alternative<RequestContextHandles>(payload_);
    }

    const RequestContextBytes& bytes() const
    {
        return std::get<RequestContextBytes>(payload_); // throws bad_variant_access if wrong mode
    }
    const RequestContextHandles& handles() const
    {
        return std::get<RequestContextHandles>(payload_); // throws bad_variant_access if wrong mode
    }
};

} // namespace app::vasara

#endif // WFMCM_REQUEST_CONTEXT_DATA_H
