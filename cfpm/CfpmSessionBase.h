/**
 * @file CfpmSessionBase.h
 * @author Wells Fargo MMDC
 * @brief Base class for CFPM session containing shared data used by both CPU and GPU
 * @copyright 2024 Wells Fargo MMDC
 */

#pragma once

#include <map>
#include <memory_resource>

#include <src/core/behavioral/MortgageBehavioralModelSession.h>
#include <src/core/behavioral/cfpm/detail/CfpmDetail.h>

namespace wfmcm {

// Forward declarations for friend access
struct CfpmSession;
template <class Model> struct CfpmSessionBuilder;

/**
 * Base session struct for CFPM model containing shared data.
 * 
 * This struct holds the common session data used by both CPU and GPU implementations.
 * The derived class (CfpmSession) provides the public interface, while the builder
 * (CfpmSessionBuilder<Model>) has protected access to populate the data.
 */
struct CfpmSessionBase : public MortgageBehavioralModelSession
{
    // Shared read interface used by both CPU and GPU sessions.

    /**
     * Return the modifier struct.
     */
    const auto& multiplier() const noexcept { return multiplier_; }

    /**
     * Return the historical derived rates lookup.
     */
    const auto& derivedHistRates() const noexcept { return derivedHistRates_; }

    /**
     * Return the mapping of historical turbo rates.
     */
    const auto& historicalTurbo() const noexcept { return histTurbo_; }

protected:
    // Shared session data accessible to both CPU and GPU builders
    detail::CfpmMultiplier multiplier_;
    HistRateTsLookup<HistPrimaryRateKey> derivedHistRates_;
    std::map<PrimaryRateType, std::pmr::vector<double>> histTurbo_;

    // Grant the session type and its builder access to protected members
    friend struct CfpmSession;
    template <class Model>
    friend struct CfpmSessionBuilder;
};

}  // namespace wfmcm
