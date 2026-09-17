/**
 * @file CfpmModelBase.h
 * @author Wells Fargo MMDC
 * @brief Shared CRTP base and builder for the CPU and GPU CFPM models.
 * @copyright 2025 Wells Fargo MMDC
 *
 * The CPU (`Cfpm`) and GPU (`CfpmGpuImpl`) models share almost everything
 * except how they *project* instruments. This header hosts the shared state,
 * the shared session/instrument construction, validation, spec-parameter
 * handling, and the shared builder. The two backends only provide their own
 * `project()` (plus, for GPU, the CUDA input marshalling helpers).
 */

#ifndef WFMCM_CFPM_MODEL_BASE_H
#define WFMCM_CFPM_MODEL_BASE_H

#include <chrono>
#include <concepts>
#include <cstddef>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <src/core/behavioral/cfpm/CfpmTraits.h>
#include <src/core/behavioral/cfpm/CfpmCalculatorPolicy.h>
#include <src/core/behavioral/cfpm/CfpmSession.h>
#include <src/core/behavioral/cfpm/detail/CfpmParameter.h>
#include <src/core/behavioral/cfpm/detail/CfpmDetail.h>
#include <src/core/behavioral/detail/BehavioralModelUtils.h>
#include <src/core/behavioral/MortgageBehavioralModelImpl.h>
#include <src/core/DiagnosticSettings.h>
#include <src/core/mortgage_enums_internal.h>
#include <src/core/MortgageCalculationExCore.h>
#include <src/core/states.h>
#include <src/core/utilities/date_util.h>
#include <wfmcm/classIds.h>
#include <wfmcm/IMortgageBehavioralModelInstrumentSession.h>
#include <wfmcm/mortgage_enums.h>
#include <wfmcm/ResidentialMortgage.h>
#include <wfmcm/inst_session_mortgage_iterator.h>

#include <mortgage/utility/containers/lookup/lookup.h>
#include <mortgage/utility/exceptions/exceptions.h>
#include <mortgage/utility/strings/to_string.h>
#include <mortgage/utility/time/time_series.h>
#include <mortgage/utility/time/time_series_key.h>

#include <ql/time/date.hpp>

#include <src/io/CfpmReader.h>
#include <src/io/CohortDefaultValuesReader.h>
#include <src/io/ServicerSpeedReader.h>
#include <src/io/TimeSeriesReader.h>

namespace wfmcm {
namespace cfpm {

/**
 * Perform conversion actions on given model parameters in the model spec.
 *
 * If the parameter is not found an exception is thrown. If the action returns
 * void then the returned value in the tuple is `std::monostate`.
 */
template <typename... Actions>
requires (std::is_invocable_v<Actions, std::string> && ...)
auto convert_param_contents(
    const std::map<ModelParamName, std::string>& spec,
    std::pair<ModelParamName, Actions>... tasks)
{
    // at least one task required
    static_assert(sizeof...(Actions), "at least one action required");
    // create tuple from actions
    return std::make_tuple(
        [&tasks, &spec]
        {
            const auto& [param, action] = tasks;
            // search for required parameter in map
            auto it = spec.find(param);
            if (it == spec.end())
                THROW(to_string(param) + " is missing.");
            // if found, perform action
            if constexpr (std::is_same_v<void, decltype(action(it->second))>) {
                action(it->second);
                return std::monostate{};
            }
            else
                return action(it->second);
        }()
        ...
    );
}

}  // namespace cfpm

//----------------------------------------------------------------------------
// Shared model base (CRTP)
//----------------------------------------------------------------------------

template <class Model, class Traits> struct CfpmModelBuilder;

/**
 * @tparam Derived the concrete backend model (`Cfpm` or `CfpmGpuImpl`).
 * @tparam Traits  `model_traits<cpu_tag>` or `model_traits<gpu_tag>`.
 */
template <class Derived, class Traits>
struct CfpmModelBase {
    using Tag = typename Traits::tag;
    using BuilderType = CfpmModelBuilder<Derived, Traits>;
    using SessionType = typename Traits::session_type;
    using SessionTypePtr = std::unique_ptr<SessionType>;
    using SessionBuilderType = CfpmSessionBuilder<Derived>;
    using ExType = typename Traits::instrument_ex;
    using ExBuilderType = typename Traits::ex_builder;
    using InstrumentSessionType = InstrumentSession<ExType>;
    using InstrumentSessionTypePtr = std::unique_ptr<InstrumentSessionType>;
    using InstrumentSessionBuilderType = InstrumentSessionBuilder<InstrumentSessionType>;
    using CalculatorType = typename Traits::calculator_type;
    using CalcStorage = typename Traits::calc_storage;
    using CohortMap = std::map<
        MortgageProductType,
        std::map<std::chrono::year, ResidentialMortgage>
    >;
    using ParamSet = std::set<ModelParamName>;

    // Unified calculator-storage accessor (see `cfpm::calc_contains` etc.).
    const CalcStorage& calculators() const noexcept { return calculators_; }

    // Read-only state accessors used by the shared instrument builder.
    const CohortMap& cohortDefaults() const noexcept { return cohortDefaults_; }
    const std::map<std::string, ServicerSpeedType>& rfServicerSpeed() const noexcept { return rfServicerSpeed_; }
    const std::map<std::string, ServicerSpeedType>& htServicerSpeed() const noexcept { return htServicerSpeed_; }

    SessionTypePtr createSession(
        QuantLib::Date valuationDate,
        const wfmutil::context& ctx) const;

    SessionTypePtr createSession(
        QuantLib::Date valuationDate,
        const std::map<wfmcm::SessionParamName, std::string>& sessionSpec,
        const wfmutil::context& ctx) const;

    InstrumentSessionTypePtr createInstrumentSession(
        const SessionType& session,
        ResidentialMortgageRange mortgages,
        const wfmutil::context& context,
        const std::chrono::year_month& factorDate) const;

    InstrumentSessionTypePtr createInstrumentSession(
        const SessionType& session,
        const IMortgageBehavioralModelInstrumentSessionView& other,
        const wfmutil::context& context,
        const std::chrono::year_month& factorDate) const;

    const ParamSet& supportedSpecParams() const;

    static const ParamSet& SupportedSpecParams();

    static IndexSet determineRequiredIndices(ResidentialMortgageRange mortgages = {});

    static ResidentialMortgage::ErrorPack
    validateMortgage(const ResidentialMortgage& mort);

protected:
    CalcStorage calculators_;
    detail::TurboParam turboParam_;
    CohortMap cohortDefaults_;
    typename Traits::business_days_ts businessDaysAdjRatio_;
    std::map<std::string, ServicerSpeedType> rfServicerSpeed_;
    std::map<std::string, ServicerSpeedType> htServicerSpeed_;
    [[no_unique_address]] GpuOnlyStorage<Tag> gpu_;

    template <class, class>
    friend struct CfpmModelBuilder;
};

//----------------------------------------------------------------------------
// Shared model builder
//----------------------------------------------------------------------------

template <class Model, class Traits>
struct CfpmModelBuilder : MortgageBehavioralModelBuilder<CfpmModelBuilder<Model, Traits>, Model>
{
    using Base = MortgageBehavioralModelBuilder<CfpmModelBuilder<Model, Traits>, Model>;
    // lookup type indexed by time and string, e.g. for business days
    using NamedTsLookup = HistRateTsLookup<std::string>;
    // submodel parameter mapping type
    using ParamMap = std::map<MortgageBehavioralSubModelType, detail::CfpmParameter>;

    // model parameters
    CfpmModelBuilder& withParameters(ParamMap&& modelParams);

    CfpmModelBuilder& withCohortDefaults(typename Model::CohortMap&& cohortDefaults);

    // business days
    CfpmModelBuilder& withBusinessDaysAdjRatio(
        NamedTsLookup&& buzDays, NamedTsLookup&& avgBuzDays) noexcept;

    // servicer speed
    CfpmModelBuilder& withServicerSpeed(
        std::map<std::string, std::string>&& rfServicerSpeed,
        std::map<std::string, std::string>&& htServicerSpeed);

private:
    void extractFromContext(const wfmutil::context&) final;
    void extractFromSpec(const std::map<ModelParamName, std::string>&) final;
    void doBuild() final;

    std::map<std::string, std::string> rfServicerSpeed_;
    std::map<std::string, std::string> htServicerSpeed_;
    NamedTsLookup buzDays_;
    NamedTsLookup avgBuzDays_;
};

//----------------------------------------------------------------------------
// CfpmModelBase method definitions
//----------------------------------------------------------------------------

template <class Derived, class Traits>
auto CfpmModelBase<Derived, Traits>::createSession(
    QuantLib::Date valuationDate,
    const wfmutil::context& ctx) const -> SessionTypePtr
{
    return std::make_unique<SessionType>(SessionBuilderType()
        .withModel(static_cast<const Derived&>(*this))
        .withAsOf(valuationDate)
        .withContext(ctx)
        .build());
}

template <class Derived, class Traits>
auto CfpmModelBase<Derived, Traits>::createSession(
    QuantLib::Date valuationDate,
    const std::map<wfmcm::SessionParamName, std::string>& sessionSpec,
    const wfmutil::context& ctx) const -> SessionTypePtr
{
    return std::make_unique<SessionType>(SessionBuilderType{}
        .withModel(static_cast<const Derived&>(*this))
        .withAsOf(valuationDate)
        .withContext(ctx)
        .withSpec(sessionSpec)
        .build());
}

namespace cfpm {

/**
 * Create a CFPM instrument session builder from a range of mortgages.
 *
 * @note This could be made more generic to handle other models later.
 */
template <class Derived, class Traits, std::forward_iterator It>
requires (std::same_as<std::iter_value_t<It>, ResidentialMortgage>)
auto make_instrument_session(
    const CfpmModelBase<Derived, Traits>& model,
    const typename CfpmModelBase<Derived, Traits>::SessionType& session,
    const It (&its)[2],
    const wfmutil::context& ctx,
    const std::chrono::year_month& factor_date,
    GpuOnlyStorage<typename Traits::tag>& gpu)
{
    // beginning and ending mortgage iterators
    auto m_begin = its[0];
    auto m_end = its[1];
    // number of mortgages (cast signed to unsigned)
    auto n_inst = static_cast<std::size_t>(std::distance(m_begin, m_end));
    // new builder
    typename CfpmModelBase<Derived, Traits>::InstrumentSessionBuilderType builder{n_inst};
    // iterate through mortgages
    for (auto m_it = m_begin; m_it != m_end; m_it++) {
        // attempt instrument build
        try {
            // build instrument + add to session builder
            auto exCore = typename CfpmModelBase<Derived, Traits>::ExBuilderType{}
                .withModel(static_cast<const Derived&>(model))
                .withSession(session)
                .withMortgage(*m_it)
                .withContext(ctx)
                .withFactorDate((*m_it).FactorDate)
                .build();

            // GPU: track packed CUDA buffer sizes while instruments are built
            if constexpr (Traits::is_gpu) {
                auto amortLen = exCore.amortLength();
                auto wala = exCore.wala();
                auto projLen = amortLen - wala;
                gpu.maxAmortLength_ = std::max(gpu.maxAmortLength_, amortLen);
                gpu.maxProjLength_ = std::max(gpu.maxProjLength_, projLen);
                gpu.totalAmortLength_ += amortLen;
                gpu.totalProjLength_ += projLen;
                gpu.multTotalSmmLen_ += exCore.totalSmmMult().first.size();
                gpu.multRefiSmmLen_ += exCore.refinanceSmmMult().first.size();
                gpu.multHtSmmLen_ += exCore.turnoverSmmMult().first.size();
                gpu.multCoSmmLen_ += exCore.cashoutSmmMult().first.size();
                gpu.multCtSmmLen_ += exCore.curtailmentSmmMult().first.size();
            }

            builder.append(std::move(exCore));
        }
        catch (...) {
            THROW_NESTED(
                "InstrumentEx build failed for asset #" +
                std::to_string(std::distance(m_begin, m_it)) +
                " (ID: '" + m_it->Id + "')"
            );
        }
    }
    // new instrument session
    return std::make_unique<decltype(builder.build())>(builder.build());
}

}  // namespace cfpm

template <class Derived, class Traits>
auto CfpmModelBase<Derived, Traits>::createInstrumentSession(
    const SessionType& session,
    ResidentialMortgageRange mortgages,
    const wfmutil::context& ctx,
    const std::chrono::year_month& factorDate) const -> InstrumentSessionTypePtr
{
    return cfpm::make_instrument_session(
        static_cast<const Derived&>(*this),
        session,
        {std::begin(mortgages), std::end(mortgages)},
        ctx,
        factorDate,
        gpu_
    );
}

template <class Derived, class Traits>
auto CfpmModelBase<Derived, Traits>::createInstrumentSession(
    const SessionType& session,
    const IMortgageBehavioralModelInstrumentSessionView& other,
    const wfmutil::context& ctx,
    const std::chrono::year_month& factorDate) const -> InstrumentSessionTypePtr
{
    return cfpm::make_instrument_session(
        static_cast<const Derived&>(*this),
        session,
        {std::begin(other), std::end(other)},
        ctx,
        factorDate,
        gpu_
    );
}

template <class Derived, class Traits>
const typename CfpmModelBase<Derived, Traits>::ParamSet&
CfpmModelBase<Derived, Traits>::supportedSpecParams() const
{
    return SupportedSpecParams();
}

template <class Derived, class Traits>
const typename CfpmModelBase<Derived, Traits>::ParamSet&
CfpmModelBase<Derived, Traits>::SupportedSpecParams()
{
    static ParamSet params{
        ModelParamName::ModelParams,
        ModelParamName::ConformingLimits,
        ModelParamName::CohortDefaultValues,
        ModelParamName::BusinessDays,
        ModelParamName::AverageBusinessDays,
        ModelParamName::ServicerSpeed
    };
    return params;
}

template <class Derived, class Traits>
IndexSet CfpmModelBase<Derived, Traits>::determineRequiredIndices(ResidentialMortgageRange /*mortgages*/)
{
    IndexSet indices;
    indices.insert({PrimaryRateType::fhmrate_pmms, PrimaryRateType::fhcr15_pmms});
    return indices;
}

template <class Derived, class Traits>
ResidentialMortgage::ErrorPack
CfpmModelBase<Derived, Traits>::validateMortgage(const ResidentialMortgage& mort)
{
    ResidentialMortgage::ErrorPack ep;
    auto& errors = ep.errors_;
    auto err_map = ResidentialMortgage::check_percentages(
        mort.Property,
        "Property",
        {
            PropertyType::Condo,
            PropertyType::CoOp,
            PropertyType::ManufacturedHome,
            PropertyType::TownHouse,
            PropertyType::PUD,
            PropertyType::Unknown
        }
    );
    errors.insert(errors.end(), err_map.errors_.begin(), err_map.errors_.end());
    err_map = ResidentialMortgage::check_percentages(mort.ServicerDistribution, "Servicer Map");
    errors.insert(errors.end(), err_map.errors_.begin(), err_map.errors_.end());
    return ep;
}

//----------------------------------------------------------------------------
// CfpmModelBuilder method definitions
//----------------------------------------------------------------------------

template <class Model, class Traits>
CfpmModelBuilder<Model, Traits>&
CfpmModelBuilder<Model, Traits>::withParameters(ParamMap&& modelParams)
{
    // set of supported CFPM model subtypes
    const auto& model_types = detail::CfpmSupportedSubModelTypes();
    // param count must match number of submodel types
    if (modelParams.size() != model_types.size())
        THROW("The parameter file does not contain all the supported CFPM models");

    if constexpr (Traits::is_gpu) {
        try {
            model_.calculators_.assign(std::move(modelParams));
        }
        catch (...) {
            THROW_NESTED(
                "Unable to create the calculator of sub model; please verify the parameter file is correct."
            );
        }
    }
    else {
        // insert calculators based on the parameters
        for (auto&& [type, param] : modelParams) {
            // model type must in the param map
            if (!model_types.contains(type))
                THROW(
                    "The CFPM model " + to_string(type) +
                    " is not included in the parameter file."
                );
            // otherwise attempt to insert the calculator
            try {
                model_.calculators_
                    .insert({type, detail::CfpmCalculator(std::move(param))});
            }
            catch (...) {
                THROW_NESTED(
                    "Unable to create the calculator of sub model: " +
                    to_string(type) +
                    " , please verify the parameter file is correct."
                );
            }
        }
    }
    return *this;
}

template <class Model, class Traits>
CfpmModelBuilder<Model, Traits>&
CfpmModelBuilder<Model, Traits>::withCohortDefaults(typename Model::CohortMap&& cohortDefaults)
{
    model_.cohortDefaults_ = std::move(cohortDefaults);
    return *this;
}

template <class Model, class Traits>
CfpmModelBuilder<Model, Traits>&
CfpmModelBuilder<Model, Traits>::withBusinessDaysAdjRatio(
    NamedTsLookup&& buzDays, NamedTsLookup&& avgBuzDays) noexcept
{
    // note: lookup has a compiler-defined move ctor
    buzDays_ = std::move(buzDays);
    avgBuzDays_ = std::move(avgBuzDays);
    return *this;
}

template <class Model, class Traits>
CfpmModelBuilder<Model, Traits>&
CfpmModelBuilder<Model, Traits>::withServicerSpeed(
    std::map<std::string, std::string>&& rfServicerSpeed,
    std::map<std::string, std::string>&& htServicerSpeed)
{
    rfServicerSpeed_ = std::move(rfServicerSpeed);
    htServicerSpeed_ = std::move(htServicerSpeed);
    return *this;
}

template <class Model, class Traits>
void CfpmModelBuilder<Model, Traits>::extractFromSpec(
    const std::map<ModelParamName, std::string>& modelSpec)
{
    // perform spec validation with supported parameters
    Base::extractFromSpec(modelSpec);
    // converter for business days data
    auto buz_converter = [](const std::string& content)
    {
        return io::BehavioralModelTableReader<std::string>::
            readFromText(content, io::BehavioralModelTableConverter::StringToString);
    };
    // convert parameter contents
    auto res = cfpm::convert_param_contents(
        modelSpec,
        std::make_pair(ModelParamName::ModelParams, io::CfpmReader::readFromCsv<>),
        std::make_pair(ModelParamName::CohortDefaultValues, io::CohortDefaultValuesReader::readFromCsv<>),
        std::make_pair(ModelParamName::BusinessDays, buz_converter),
        std::make_pair(ModelParamName::AverageBusinessDays, buz_converter),
        std::make_pair(ModelParamName::ServicerSpeed, io::ServicerSpeedReader::readFromCsv<>)
    );
    // break into individual names (note: lvalue references)
    auto& [params, cohort_defaults, bus_days, avg_bus_days, serv_speeds] = res;
    // servicer speeds
    auto& [rf_serv_speed, ht_serv_speed] = serv_speeds;
    // set state using extracted objects
    withParameters(std::move(params));
    withCohortDefaults(std::move(cohort_defaults));
    withBusinessDaysAdjRatio(std::move(bus_days), std::move(avg_bus_days));
    withServicerSpeed(std::move(rf_serv_speed), std::move(ht_serv_speed));
}

template <class Model, class Traits>
void CfpmModelBuilder<Model, Traits>::extractFromContext(const wfmutil::context& ctx)
{
    Base::extractFromContext(ctx);
}

template <class Model, class Traits>
void CfpmModelBuilder<Model, Traits>::doBuild()
{
    Base::doBuild();

    if constexpr (Traits::is_gpu) {
        if (model_.calculators_.models_.empty())
            THROW("Cfpm: calculator map is empty.");
    }
    else {
        if (model_.calculators_.empty())
            THROW("Cfpm: calculator map is empty.");
    }

    // convert servicer map into std::map<std::string, ServicerSpeedType>
    detail::convertServicerSpeedMap(std::move(rfServicerSpeed_), model_.rfServicerSpeed_);
    detail::convertServicerSpeedMap(std::move(htServicerSpeed_), model_.htServicerSpeed_);

    // assign turbo parameters
    if constexpr (Traits::is_gpu) {
        model_.turboParam_ = cfpm::calc_turbo_param(model_.calculators_);
    }
    else {
        // these must be the same for all models
        bool hasAssignTurboParam = false;
        for (auto& [key, value] : model_.calculators_) {
            if (!hasAssignTurboParam) {
                // note: useless std::move as TurboParam is POD; this is just a copy
                model_.turboParam_ = std::move(value.Prepay_.Turbo_);
                hasAssignTurboParam = true;
            }
            else if (model_.turboParam_ != value.Prepay_.Turbo_) {
                THROW("Cfpm turbo parameters are the not same of different submodels.");
            }
        }
    }

    // calculate businessDaysAdjRatio_
    std::pmr::vector<double> data;
    const auto& avgBDKey = *avgBuzDays_.key<1>().begin();
    const auto& buzKey = *buzDays_.key<1>().begin();
    auto startDate = std::max(
        buzDays_.key<0>().startDate(),
        first_month_of_year(avgBuzDays_.key<0>().startDate())
    );
    auto endDate = std::min(
        buzDays_.key<0>().endDate(),
        last_month_of_year(avgBuzDays_.key<0>().endDate())
    );

    std::size_t dataSize = detail::calculateUnsignedMonthDifference(endDate, startDate) + 1;
    data.reserve(dataSize);

    for (auto date = startDate; date <= endDate;) {
        data.emplace_back(
            buzDays_.value(date, buzKey) /
            avgBuzDays_.value(first_month_of_year(date), avgBDKey)
        );
        date += std::chrono::months(1);
    }

    if constexpr (Traits::is_gpu) {
        model_.businessDaysAdjRatio_.start_ = startDate;
        model_.businessDaysAdjRatio_.end_ = endDate;
        model_.businessDaysAdjRatio_.values_ = std::move(data);
    }
    else {
        model_.businessDaysAdjRatio_ = {startDate, std::move(data)};
    }
}

}  // namespace wfmcm

#endif  // WFMCM_CFPM_MODEL_BASE_H
