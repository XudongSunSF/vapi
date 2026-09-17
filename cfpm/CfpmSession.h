/**
 * @file CfpmSession.h
 * @author Wells Fargo MMDC
 * @brief Unified CFPM session struct and builder.
 * @copyright 2024 Wells Fargo MMDC
 *
 * The CPU and GPU backends previously each owned a session type and a builder.
 * The session data lives in
 * `CfpmSessionBase`; the builders only differ by how they reach the model's
 * calculator storage, so the builder is templated on the model type and routes
 * through the `cfpm::calc_*` policy functions.
 */

#ifndef WFMCM_CFPM_SESSION_H
#define WFMCM_CFPM_SESSION_H

#include <map>
#include <memory_resource>
#include <ranges>
#include <set>
#include <string>
#include <vector>

#include <wfmcm/enums.h>
#include <src/core/behavioral/cfpm/CfpmSessionBase.h>
#include <src/core/behavioral/MortgageBehavioralModelSession.h>
#include <src/core/behavioral/cfpm/CfpmCalculatorPolicy.h>
#include <src/core/behavioral/cfpm/detail/CfpmDetail.h>
#include <src/core/DiagnosticSettings.h>

#include <mortgage/utility/containers/context/context.h>
#include <mortgage/utility/containers/lookup/lookup.h>
#include <mortgage/utility/types/constants.h>
#include <ql/time/date.hpp>

#include <src/io/PolyvarReader.h>

namespace wfmcm {

//----------------------------------------------------------------------------
// Session (shared by CPU and GPU)
//----------------------------------------------------------------------------

struct CfpmSession : public CfpmSessionBase {
    using ParamSet = std::set<SessionParamName>;

    /**
     * Return the set of supported session parameters.
     */
    const ParamSet& supportedSpecParams() const final;

    /**
     * Return the set of supported session parameters.
     */
    static const ParamSet& SupportedSpecParams();
};

inline const CfpmSession::ParamSet& CfpmSession::supportedSpecParams() const
{
    return SupportedSpecParams();
}

inline const CfpmSession::ParamSet& CfpmSession::SupportedSpecParams()
{
    // static init is thread-safe under C++11 rules
    static auto session_params = []
    {
        auto params = MortgageBehavioralModelSession::SupportedSpecParams();
        params.insert(
            {
                SessionParamName::Multipliers,
                SessionParamName::PrepayMultiplier,
                SessionParamName::RefinanceMultiplier,
                SessionParamName::TurnoverMultiplier
            }
        );
        return params;
    }();
    return session_params;
}

//----------------------------------------------------------------------------
// Session builder (templated on the model type)
//----------------------------------------------------------------------------

/**
 * CFPM session struct builder.
 *
 * @tparam Model `Cfpm` (CPU) or `CfpmGpuImpl` (GPU); must expose
 *               `calculators()`, `determineRequiredIndices()`.
 *
 * @todo Document more
 */
template <class Model>
struct CfpmSessionBuilder
  : public MortgageBehavioralModelSessionBuilder<CfpmSessionBuilder<Model>, Model, CfpmSession> {
    using Base = MortgageBehavioralModelSessionBuilder<CfpmSessionBuilder<Model>, Model, CfpmSession>;
    using SessionType = CfpmSession;
    // session parameter mapping spec type
    using ParamMap = std::map<SessionParamName, std::string>;

    CfpmSessionBuilder() = default;
    CfpmSessionBuilder& withMultiplier(wfmutil::polyvar&& mult);

private:
    void applyMultiplierKnobs(const detail::MultiplierKnobs& knobs) final;
    void extractFromContext(const wfmutil::context&) final;
    void extractFromSpec(const ParamMap&) final;
    void extendPrimaryHistoricalRates();
    void doBuild() final;
};

template <class Model>
CfpmSessionBuilder<Model>& CfpmSessionBuilder<Model>::withMultiplier(wfmutil::polyvar&& mult)
{
    for (auto&& m : mult) {
        // {FIXME} should element type detection be here?
        // {FIXME} UB if m.name() is the empty string
        if (m.name()[0] == '_')
            continue;
        detail::convert(std::move(m), &session_.multiplier_);
    }
    return *this;
}

template <class Model>
void CfpmSessionBuilder<Model>::applyMultiplierKnobs(const detail::MultiplierKnobs& knobs)
{
    if (knobs.prepayMultiplier_)
        session_.multiplier_.KnobPrepay = knobs.prepayMultiplier_.value();
    if (knobs.refinanceMultiplier_)
        session_.multiplier_.KnobRefinance = knobs.refinanceMultiplier_.value();
    if (knobs.turnoverMultiplier_)
        session_.multiplier_.KnobTurnover = knobs.turnoverMultiplier_.value();
    detail::checkKnobMultipliers(
        3,
        session_.multiplier_.KnobPrepay,
        session_.multiplier_.KnobRefinance,
        session_.multiplier_.KnobTurnover
    );
}

template <class Model>
void CfpmSessionBuilder<Model>::extractFromContext(const wfmutil::context& ctx)
{
    Base::extractFromContext(ctx);
}

template <class Model>
void CfpmSessionBuilder<Model>::extractFromSpec(const ParamMap& params)
{
    Base::extractFromSpec(params);

    // read multiplier from file or string
    if (auto it = params.find(SessionParamName::Multipliers); it != params.end()) {
        auto multipliers = io::PolyvarReader::readFromJson(it->second);
        // Check file format version
        detail::checkJsonFormatVersion(
            multipliers, it->second, detail::MULTIPLIER_FORMAT_VERSION
        );
        withMultiplier(std::move(multipliers));
    }
}

template <class Model>
void CfpmSessionBuilder<Model>::extendPrimaryHistoricalRates()
{
    // use pmms 30 and 15 rates to create pmms 10 and 20 rates
    const auto& hist_rates = session_.histMbsRates_.value()->lookupTs_;
    auto dateKey = hist_rates.key<0>();
    const auto& valueKey = hist_rates.key<1>();

    std::pmr::vector<ResidentialMortgage> mortgages;
    const auto& requiredIndices = model_->determineRequiredIndices(mortgages);

    // check if required primary rates is in historical primary rate file
    for (auto type : requiredIndices.view_of<PrimaryRateType>()) {
        if (valueKey.pos(type) == wf::mortgage::utility::lookup_constant::MISSING_KEY) {
            THROW("Required primary rate: " + to_string(type)
                + " is missing. Please verify the historical primary rates are good.");
        }
    }

    // number of derived rates (10, 20 fixed, should be just 2)
    auto n_derived = detail::cfpm_derived_rates_map().size();
    // get the derived rates from the derived rates map (must map to calculators)
    std::pmr::vector<HistPrimaryRateKey> rates;
    rates.reserve(n_derived);
    for (const auto& [key, value] : detail::cfpm_derived_rates_map()) {
        if (!cfpm::calc_contains(model_->calculators(), key))
            THROW("Model: " + to_string(key) + " is not in the parameter map.");
        rates.emplace_back(value);
    }

    auto dateSize = dateKey.size();
    // lookup indices from the primary rate keys
    auto i30 = valueKey.pos(PrimaryRateType::fhmrate_pmms);
    auto i15 = valueKey.pos(PrimaryRateType::fhcr15_pmms);
    // output vector to back a lookup of fixed 10/20 derived rates
    std::pmr::vector<double> values;
    values.reserve(n_derived * dateSize);
    // add 15/30 year rates to derive the 10 and 20 year rates
    for (std::size_t i = 0; i < dateSize; i++) {
        for (auto model_type : std::views::keys(detail::cfpm_derived_rates_map())) {
            // note: derived rate is computed via the respective calculator
            auto rate = cfpm::calc_derived_rate(
                model_->calculators(), model_type, hist_rates(i, i15), hist_rates(i, i30));
            values.emplace_back(rate);
        }
    }
    // build using builder partial specialization
    session_.derivedHistRates_ = wf::mortgage::utility::lookup_builder<HistRateTsLookup<HistPrimaryRateKey>>{}
        .withKey<0>(std::move(dateKey))
        .withKey<1>(rates)
        .withValue(std::move(values))
        .build();
}

template <class Model>
void CfpmSessionBuilder<Model>::doBuild()
{
    Base::doBuild();

    if (!model_)
        THROW("Cfpm session: missing model.");
    if (!session_.hasHistoricalMbsPrimaryRates())
        THROW("Cfpm session: missing historical mbs primary rates.");
#ifdef _VALIDATE_OPTIONAL_BUILDER_SETTINGS_
    if (!session_.histMbsRates_ && !session_.econScenario_) {
        THROW("Cfpm: Missing MBS rates or economic scenario.");
    }

    if (!session_.unemployment_) {
        THROW("Cfpm: Missing unemployment data");
    }

    if (!session_.hpi_) {
        THROW("Cfpm: Missing HPI data");
    }
#endif  // _VALIDATE_OPTIONAL_BUILDER_SETTINGS_
    extendPrimaryHistoricalRates();

    // calc historical turbo
    const auto& primaryRates = session_.histMbsRates_.value()->lookupTs_;

    for (const auto& turboRate : detail::cfpm_turbo_rate_types()) {
        auto histRateBegin = primaryRates.begin_with_key(_, turboRate);
        auto histRateEnd = primaryRates.end_with_key(_, turboRate);
        auto derivedRateBegin = session_.derivedHistRates_.begin_with_key(_, turboRate);
        auto derivedRateEnd = session_.derivedHistRates_.end_with_key(_, turboRate);
        // select derived only if historical is empty
        auto [rateBegin, rateEnd] = [&]
        {
            return (histRateBegin != histRateEnd) ?
                std::make_pair(histRateBegin, histRateEnd) :
                std::make_pair(derivedRateBegin, derivedRateEnd);
        }();
        // range must be nonzero
        if (rateBegin == rateEnd)
            THROW(
                to_string(turboRate) +
                " is not available, hist turbo cannot be calculated"
            );
        // calculate turbo rates
        std::pmr::vector<double> turboValues;
        turboValues.reserve(std::distance(rateBegin, rateEnd));
        detail::calculateTurboRates(
            rateBegin,
            rateEnd,
            rateBegin,
            cfpm::calc_turbo_param(model_->calculators()),
            turboValues
        );
        session_.histTurbo_[turboRate] = std::move(turboValues);
    }
}

}  // namespace wfmcm

#endif  // WFMCM_CFPM_SESSION_H
