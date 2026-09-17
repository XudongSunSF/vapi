// Standard library headers must precede project headers here: <execution>
// pulls libstdc++'s parallel STL, which uses the reserved identifier _T2 that
// mortgage/utility/namespaces.h defines as a macro; including it first avoids
// the macro clash on GCC/libstdc++.
#include <chrono>
#include <concepts>
#include <execution>
#include <iterator>
#include <functional>
#include <tuple>
#include <type_traits>
#include <utility>

#include <src/core/behavioral/cfpm/gpu/Cfpm.h>
#include <mortgage/utility/containers/lookup/lookup_matrix_view.h>
#include <utils/pinned_memory_resource.hpp>
#include <src/core/behavioral/cfpm/gpu/CfpmInstrumentSession.h>
#include <src/core/behavioral/cfpm/gpu/CfpmRatePathSession.h>
#include <src/core/behavioral/cfpm/gpu/CfpmSession.h>
#include <src/core/behavioral/cfpm/detail/CfpmDetail.h>
#include <src/core/behavioral/detail/BehavioralModelUtils.h>
#include <src/io/CfpmReader.h>
#include <src/io/CohortDefaultValuesReader.h>
#include <src/io/ServicerSpeedReader.h>
#include <wfmcm/inst_session_mortgage_iterator.h>

#include <iostream>

namespace ql = QuantLib;
using namespace std;

namespace {
size_t mapCfpmPrimaryRateTypeToNum(PrimaryRateType rateType) {
    switch (rateType) {
        case PrimaryRateType::conv_fixed10_pmms: return 0;
        case PrimaryRateType::fhcr15_pmms:       return 1;
        case PrimaryRateType::conv_fixed20_pmms: return 2;
        case PrimaryRateType::fhmrate_pmms:      return 3;
        default:
            THROW("Unable to find corresponding PrimaryRateType of " + to_string(rateType));
    }
}
}

namespace wfmcm {

namespace {

auto make_rate_data(
    std::size_t n_hist,
    const HistRateTsLookup<HistPrimaryRateKey>& raw_hist,
    const HistRateTsLookup<HistPrimaryRateKey>& derived_hist,
    const IRatePathManager& raw_proj,
    const CfpmRatePathSession::ScenMap& derived_proj)
{
    struct RateData {
        std::pmr::vector<std::pmr::vector<double>> hist_rates_;
        std::pmr::vector<double> proj_rates_flat_;  // Changed to flat vector
        std::pmr::vector<double> turbo_rates_flat_; // Added for turbo rates
        size_t num_scenarios_;
        size_t num_rate_types_;
        size_t num_proj_periods_;
        
        RateData(size_t numRateTypes, size_t numScenarios, size_t numProjPeriods)
            : num_scenarios_(numScenarios)
            , num_rate_types_(numRateTypes)
            , num_proj_periods_(numProjPeriods)
        {
            hist_rates_.resize(numRateTypes);
            // Pre-allocate flat vectors with correct size
            proj_rates_flat_.resize(numScenarios * numRateTypes * numProjPeriods);
            turbo_rates_flat_.resize(numScenarios * numRateTypes * numProjPeriods);
        }
    };
    
    auto numRateTypes = detail::cfpm_raw_rate_types().size() + detail::cfpm_derived_rate_types().size();
    
    // Get dimensions from the first rate type projection data
    size_t numScenarios = 0;
    size_t numProjPeriods = 0;
    
    // Determine dimensions from raw projected rates
    const auto& raw_proj_mgr = raw_proj.imanager<PrimaryRateType>();
    auto first_rate = detail::cfpm_raw_rate_types().begin();
    if (first_rate != detail::cfpm_raw_rate_types().end()) {
        auto proj_view = raw_proj_mgr.get_if(*first_rate);
        if (proj_view) {
            numScenarios = proj_view.rows();
            numProjPeriods = proj_view.columns();
        }
    }
    
    // hard-coded 256 scenarios
    numScenarios = 256;
    RateData result(numRateTypes, numScenarios, numProjPeriods);
    
    // Extract raw historical rates (PMMS 15, 30)
    for (auto rate : detail::cfpm_raw_rate_types()) {
        std::pmr::vector<double> hist_rates;
        hist_rates.resize(n_hist);
        
        const auto& time_key = raw_hist.template key<0>();
        auto time_size = std::min(n_hist, time_key.size());

        auto hist = wfmutil::
            make_matrix_view(raw_hist, wfmutil::col_select(n_hist), rate);
        
        for (size_t t = 0; t < time_size; ++t) {
            hist_rates[t] = hist(0, t);
        }
        
        auto rate_idx = mapCfpmPrimaryRateTypeToNum(rate);
        result.hist_rates_[rate_idx] = std::move(hist_rates);
        
        // Extract projected data directly to flat vector
        const auto& raw_proj_mgr = raw_proj.imanager<PrimaryRateType>();
        auto proj_view = raw_proj_mgr.get_if(rate);
        if (proj_view) {
            // Copy directly to flat vector in the correct layout (scenario, rate, time)
            for (size_t s = 0; s < numScenarios; ++s) {
                for (size_t t = 0; t < numProjPeriods; ++t) {
                    size_t flat_idx = s * numRateTypes * numProjPeriods + rate_idx * numProjPeriods + t;
                    result.proj_rates_flat_[flat_idx] = proj_view(0, t);
                }
            }
        }
    }
    
    // Extract derived historical rates (conv fixed 10, 20)
    for (auto rate : detail::cfpm_derived_rate_types()) {
        std::pmr::vector<double> hist_rates;
        hist_rates.resize(n_hist);
        
        const auto& time_key = derived_hist.template key<0>();
        auto time_size = std::min(n_hist, time_key.size());
        
        auto hist = wfmutil::
                    make_matrix_view(derived_hist, wfmutil::col_select(n_hist), rate);
        for (size_t t = 0; t < time_size; ++t) {
            hist_rates[t] = hist(0, t);
        }
        
        auto rate_idx = mapCfpmPrimaryRateTypeToNum(rate);
        result.hist_rates_[rate_idx] = std::move(hist_rates);
        
        // Extract projected data directly to flat vector
        auto proj_iter = derived_proj.find(rate);
        if (proj_iter != derived_proj.end()) {
            const auto& proj_3d = proj_iter->second;
            // Copy directly to flat vector in the correct layout (scenario, rate, time)
            for (size_t s = 0; s < numScenarios && s < proj_3d.size(); ++s) {
                for (size_t t = 0; t < numProjPeriods && t < proj_3d[s].size(); ++t) {
                    size_t flat_idx = s * numRateTypes * numProjPeriods + rate_idx * numProjPeriods + t;
                    result.proj_rates_flat_[flat_idx] = proj_3d[0][t];
                }
            }
        }
    }
    
    return result;
}
    
auto make_turbo_data(
    std::size_t n_hist,
    const std::map<PrimaryRateType, wfmcm::RatePath>& hist,
    const std::pmr::vector<PrimaryRateType>& primaryRates,
    const std::map<PrimaryRateType, wfmutil::matrix<double>>& proj)
{
    struct TurboData {
        std::pmr::vector<std::pmr::vector<double>> hist_rates_;
        std::pmr::vector<double> proj_rates_flat_;  // Changed to flat vector
        size_t num_scenarios_;
        size_t num_rate_types_;
        size_t num_proj_periods_;
        
        TurboData(size_t numRateTypes, size_t numScenarios, size_t numProjPeriods)
            : num_scenarios_(numScenarios)
            , num_rate_types_(numRateTypes)
            , num_proj_periods_(numProjPeriods)
        {
            hist_rates_.resize(numRateTypes);
            proj_rates_flat_.resize(numScenarios * numRateTypes * numProjPeriods);
        }
    };
    
    // Get dimensions from the first available projection data
    size_t numScenarios = 0;
    size_t numProjPeriods = 0;
    
    auto first_proj = proj.begin();
    if (first_proj != proj.end()) {
        numScenarios = first_proj->second.size();
        numProjPeriods = first_proj->second[0].size();
    }

    // hard-coded 256 scenarios
    numScenarios = 256;
    TurboData result(primaryRates.size(), numScenarios, numProjPeriods);
    
    for (auto rate : primaryRates) {
        // Historical data - truncated to n_hist
        std::pmr::vector<double> hist_rates;
        hist_rates.resize(n_hist);
        
        auto hist_iter = hist.find(rate);
        if (hist_iter != hist.end()) {
            const auto& hist_rate_data = hist_iter->second;
            size_t hist_size = std::min(n_hist, hist_rate_data.size());
            for (size_t i = 0; i < hist_size; ++i) {
                hist_rates[i] = hist_rate_data[i];
            }
        }
        
        auto rate_idx = mapCfpmPrimaryRateTypeToNum(rate);
        result.hist_rates_[rate_idx] = std::move(hist_rates);
        
        // Projected data - copy directly to flat vector
        auto proj_iter = proj.find(rate);
        if (proj_iter != proj.end()) {
            const auto& proj_matrix = proj_iter->second;
            // Copy directly to flat vector in the correct layout (scenario, rate, time)
            for (size_t s = 0; s < numScenarios; ++s) {
                for (size_t t = 0; t < numProjPeriods; ++t) {
                    size_t flat_idx = s * result.num_rate_types_ * numProjPeriods + rate_idx * numProjPeriods + t;
                    result.proj_rates_flat_[flat_idx] = proj_matrix[0][t];
                }
            }
        }
    }
    
    return result;
}

}  // namespace

auto CfpmGpuImpl::project(
    CalcResults&& results,
    const SessionType& session,
    const InstrumentSessionType::View& instrumentSession,
    const IRatePathManager& rates,
    const context& context) const -> CalcResults
{
    const auto& rateMgr = rates.imanager<PrimaryRateType>();
    // empty output if no rates or context
    if (rateMgr.empty() || instrumentSession.empty())
        return {};
    // assume first rates view defines counts of  paths and rates
    //size_t numPaths = rateMgr.begin()->second.rows();
    size_t numPaths = 256;
    size_t numRates = rateMgr.begin()->second.columns();
    // thread-local context name scenario storage
    app::storeScenarioId(context.name());

    // {FIXME} better way to get requiredIndices?
    // {FIXME} need to determine if turbo param is fixed or may change later

    // note: don't actually need the vector but signature required one. so we
    // just pass a default-constructed vector and call it a day
    auto requiredIndices = determineRequiredIndices({});
    // debug context key. must be string (for implicit conversion reasons)
    std::string key{app::retrieveModelDebugContextId("cfpm_ratepathSession_key")};
    // create rate path session shared pointer. note that if there is no debug
    auto rps = CfpmRatePathSessionBuilder<gpu::CfpmCalculator>{}
                .withHistRates(session.hist_rates_lookup())
                .withDerivedHistRates(session.derivedHistRates())
                .withProjScenarios(rates)
                .withAsOfDate(session.asOf())	//rate cut off date
                .withRequiredIndices(requiredIndices)
                .withDerivedRatesParam(calculators_, detail::cfpm_derived_rates_map())
                .withProjectionLength(session.projectionLength())
                .withTurboParam(calculators_.Turbo_)
                .build();
    // number of time steps between first historical and first projected. this
    // gives the number of historical rates that are used. this of course
    // assumes that there is sufficient history for this to be positive
    auto n_hist = (rps.firstProjDate() - rps.firstHistDate()).count();
    auto rateData = make_rate_data(
        n_hist,
        session.hist_rates_lookup(),
        session.derivedHistRates(),
        rates,
        rps.derivedProjScenarios()
    );

    auto turboRateData = make_turbo_data(
        n_hist,
        session.historicalTurbo(),
        detail::cfpm_turbo_rate_types(),
        rps.projTurbo()
    );

    auto start_time = std::chrono::steady_clock::now();
    auto pinned_mr = wfmcm::cuda::pinned_memory_resource::instance();
    // move rate data to cuda::cfpm::Rates
    
    cuda::cfpm::Rates gpuRateInputs(
        cuda::monthsSinceBase<size_t>(rps.firstHistDate()),
        cuda::monthsSinceBase<size_t>(rps.lastHistDate()),
        cuda::monthsSinceBase<size_t>(rps.firstProjDate()),
        cuda::monthsSinceBase<size_t>(rps.lastProjDate()),
        numPaths,
        rateData.num_rate_types_,
        std::move(rateData.hist_rates_),
        std::move(rateData.proj_rates_flat_),      // Pass flat vector directly
        std::move(turboRateData.hist_rates_),
        std::move(turboRateData.proj_rates_flat_),  // Pass flat vector directly
        pinned_mr
    );
    
    // auto inst_start_time = std::chrono::steady_clock::now();
    auto gpuInstInputs = createGpuInstrumentInputs(instrumentSession);
    // auto inst_end_time = std::chrono::steady_clock::now();
    // auto inst_duration = std::chrono::duration_cast<std::chrono::milliseconds>(inst_end_time - inst_start_time);
    // std::cout << "Converting cpu instrument objects to vectors sent to gpu took " << inst_duration.count() << " ms" << std::endl;
    
    auto gpuCfpmMultiplier = createGpuCfpmMultiplier(session);
    cuda::cfpm::TimeSeries gpuBDaysAdjRatio(
        cuda::monthsSinceBase<size_t>(businessDaysAdjRatio_.startDate()),
        cuda::monthsSinceBase<size_t>(businessDaysAdjRatio_.endDate()),
        businessDaysAdjRatio_.spacing(),
        businessDaysAdjRatio_.data()
    ); 

    // cudaResults is a vector of cuda::cfpm::Results with prepay and curtailment
    // the vector size is the number of batches of rate paths
    // each prepay has dimension of (numInsts, numPaths, maxProjLength)
    auto gpu_start_time = std::chrono::steady_clock::now();
    auto cudaBatchResults = cuda::cfpm::project(
        *(calculators_.prepay_),
        gpuRateInputs,
        *gpuInstInputs,
        gpuCfpmMultiplier,
        gpuBDaysAdjRatio); 

    auto gpu_end_time = std::chrono::steady_clock::now();
    auto gpu_duration = std::chrono::duration_cast<std::chrono::milliseconds>(gpu_end_time - gpu_start_time);
    std::cout << "Gpu data transfer + calculation + result transfer back took " << gpu_duration.count() << " ms" << std::endl;
    auto result_start_time = std::chrono::steady_clock::now();

    auto numInstruments = gpuInstInputs->numInstruments;
    const auto& totalProjLength = gpuInstInputs->totalProjLength;
    const auto& instStartIdx = gpuInstInputs->projStartingPos;
    const auto& instProjLength = gpuInstInputs->projLength;

    using dmatrix = wfmutil::matrix<double>;
    std::vector<dmatrix> prepayMats(numInstruments, dmatrix(numPaths));
    std::vector<dmatrix> curtailMats(numInstruments, dmatrix(numPaths));

    std::vector<size_t> instrument_indices(numInstruments);
    std::iota(instrument_indices.begin(), instrument_indices.end(), 0);

    for (const auto& batchResult : cudaBatchResults) {
        const auto& prepayResultsGpu = batchResult.prepay;
        const auto& curtailResultsGpu = batchResult.curtailment;
        // Use parallel execution to populate the pre-allocated matrices.
        std::for_each(std::execution::par, instrument_indices.begin(), instrument_indices.end(), [&](size_t i) {
            const size_t projLen = instProjLength[i];
            if (projLen == 0) {
                return; // Nothing to process for this instrument
            }

            // Get references to the pre-allocated matrices for this instrument.
            auto& prepayMat = prepayMats[i];
            auto& curtailMat = curtailMats[i];

            // Calculate the starting point of this instrument's data in the flat source array.
            const size_t inst_data_start_offset = instStartIdx[i] * batchResult.numPaths;

            for (size_t p = 0; p < batchResult.numPaths; ++p) {
                // The data for each path is contiguous in the source array.
                const size_t path_start_offset = inst_data_start_offset + (p * projLen);
                
                // Get iterators to the source data for this path.
                auto prepay_src_begin = prepayResultsGpu.begin() + path_start_offset;
                auto prepay_src_end = prepay_src_begin + projLen;

                auto curtail_src_begin = curtailResultsGpu.begin() + path_start_offset;
                auto curtail_src_end = curtail_src_begin + projLen;

                // Use 'assign' to populate the pre-allocated row vectors.
                // Since the rows are empty, this will perform one allocation per row,
                // but it happens in a thread-safe manner on its own matrix object.
                prepayMat[p].assign(prepay_src_begin, prepay_src_end);
                curtailMat[p].assign(curtail_src_begin, curtail_src_end);
            }
        });
    }

    // Move the populated matrices into the final results structure (serially).
    for (size_t i = 0; i < numInstruments; ++i) {
        std::map<MortgageBehavioralModelOutputKey, dmatrix> output_map;
        output_map.emplace(MortgageBehavioralModelOutputKey::Prepay, std::move(prepayMats[i]));
        output_map.emplace(MortgageBehavioralModelOutputKey::Curtail, std::move(curtailMats[i]));
        
        results[i] = CalcResult(instrumentSession[i].instrumentId(), std::move(output_map));
    }

    auto end_time = std::chrono::steady_clock::now();
    auto result_duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - gpu_end_time);
    std::cout << "Populating final results took " << result_duration.count() << " ms" << std::endl;
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    std::cout << "Cfpm model gpu gpu data transfer + calculation + results population for a portfolio of size "
              << instrumentSession.size() <<  " and " << numPaths << " rate paths took : "
              << duration.count() << "ms" << std::endl;
    return results;
}

std::unique_ptr<cuda::cfpm::InstrumentInputs>
CfpmGpuImpl::createGpuInstrumentInputs(const InstrumentSessionType::View& instrumentSession) const
{
    auto pinned_mr = wfmcm::cuda::pinned_memory_resource::instance();
    auto instInputsPtr = std::make_unique<cuda::cfpm::InstrumentInputs>(
        instrumentSession.size(),
        gpu_.maxAmortLength_,
        gpu_.maxProjLength_,
        gpu_.totalAmortLength_,
        gpu_.totalProjLength_,
        gpu_.multTotalSmmLen_,
        gpu_.multRefiSmmLen_,
        gpu_.multHtSmmLen_,
        gpu_.multCoSmmLen_,
        gpu_.multCtSmmLen_,
        pinned_mr
    );
    // auto mapCfpmSubModelTypeToNum = [](MortgageBehavioralSubModelType mdlType) {
    //     switch (mdlType) {
    //         case MortgageBehavioralSubModelType::Cfpm_F10: return 0;
    //         case MortgageBehavioralSubModelType::Cfpm_F15: return 1;
    //         case MortgageBehavioralSubModelType::Cfpm_F20: return 2;
    //         case MortgageBehavioralSubModelType::Cfpm_F30: return 3;
    //         default: THROW("Unsupported CFPM sub model type");
    //     }
    // };

    auto mapCfpmPrimaryRateTypeToNum = [](PrimaryRateType rateType) {
        switch (rateType) {
            case PrimaryRateType::conv_fixed10_pmms: return 0;
            case PrimaryRateType::fhcr15_pmms:       return 1;
            case PrimaryRateType::conv_fixed20_pmms: return 2;
            case PrimaryRateType::fhmrate_pmms:      return 3;
            default:
                THROW("Unable to find corresponding PrimaryRateType of " + to_string(rateType));
        }
    };

    for (size_t i = 0; i < instrumentSession.size(); ++i) {
        const auto& inst = instrumentSession[i];
        const auto& subMdlType = inst.subModelType();
        const auto subMdlIdx = calculators_.modelMappedIndex(subMdlType);
        instInputsPtr->prepayParamIndex.emplace_back(subMdlIdx);
        instInputsPtr->amortStartingPos.emplace_back(instInputsPtr->loanSizeSeries.size());
        instInputsPtr->amortStartDate.emplace_back(cuda::monthsSinceBase<size_t>(inst.amortDatesSeries()[0]));
        instInputsPtr->amortLength.emplace_back(inst.amortLength());
        instInputsPtr->projStartingPos.emplace_back(instInputsPtr->projHpa1YSeries.size());
        instInputsPtr->projLength.emplace_back(inst.amortLength() - inst.wala());
        // instInputsPtr->rateProjIndex.emplace_back();
        instInputsPtr->primeRateIndex.emplace_back(mapCfpmPrimaryRateTypeToNum(inst.primaryRate()));
        instInputsPtr->origYear.emplace_back(cuda::yearsSinceBase<size_t>(inst.origDate().year()));
        instInputsPtr->origDate.emplace_back(cuda::monthsSinceBase<size_t>(inst.origDate()));
        instInputsPtr->wala.emplace_back(static_cast<size_t>(inst.wala()));
        instInputsPtr->wac.emplace_back(inst.wac());
        instInputsPtr->fico.emplace_back(inst.fico());
        instInputsPtr->oltv.emplace_back(inst.oltv());
        instInputsPtr->oals.emplace_back(inst.oals());
        instInputsPtr->refiState.emplace_back(calculators_.statefunc_.RefinanceState[subMdlIdx](inst.state()));
        instInputsPtr->curtailState.emplace_back(calculators_.statefunc_.CurtailmentState[subMdlIdx](inst.state()));
        instInputsPtr->turnoverState.emplace_back(calculators_.statefunc_.TurnoverState[subMdlIdx](inst.state()));
        instInputsPtr->prepayElbowState.emplace_back(calculators_.statefunc_.ElbowState[subMdlIdx](inst.state()));
        instInputsPtr->sato.emplace_back(inst.sato());
        instInputsPtr->wacDiscFactor.emplace_back(inst.wacDiscFactor());
        std::pmr::vector<double> purpose(cuda::NumPurposeTypes);
        for (const auto& [k, v] : inst.purpose()) {
            auto idx = static_cast<size_t>(k);
            if (idx < cuda::NumPurposeTypes) // ignore unknown
                purpose[idx] = v;
        }
        instInputsPtr->purpose.emplace_back(std::move(purpose));
        std::pmr::vector<double> property(cuda::NumPropertyTypes);
        for (const auto& [k, v] : inst.property()) {
            auto idx = static_cast<size_t>(k);
            if (idx < cuda::NumPropertyTypes)
                property[idx] = v;
        }
        instInputsPtr->property.emplace_back(std::move(property));
        std::pmr::vector<double> occupancy(cuda::NumOccupancyTypes);
        for (const auto& [k, v] : inst.occupancy()) {
            auto idx = static_cast<size_t>(k);
            if (idx < cuda::NumOccupancyTypes)
                occupancy[idx] = v;
        }
        instInputsPtr->occupancy.emplace_back(std::move(occupancy));
        instInputsPtr->propertyInspectionWaiver.emplace_back(inst.piw());
        std::pmr::vector<double> rfSpeed(cuda::NumServiceSpeedTypes);
        for (const auto& [k, v] : inst.rfSpeed()) {
            auto idx = static_cast<size_t>(k);
            if (idx < cuda::NumServiceSpeedTypes)
                rfSpeed[idx] = v;
        }
        instInputsPtr->rfSpeed.emplace_back(std::move(rfSpeed));
        std::pmr::vector<double> htSpeed(cuda::NumServiceSpeedTypes);
        for (const auto& [k, v] : inst.htSpeed()) {
            auto idx = static_cast<size_t>(k);
            if (idx < cuda::NumServiceSpeedTypes)
                htSpeed[idx] = v;
        }
        instInputsPtr->htSpeed.emplace_back(std::move(htSpeed));
        std::pmr::vector<double> channel(cuda::NumOriginationChannels);
        for (const auto& [k, v] : inst.channel()) {
            auto idx = static_cast<size_t>(k);
            if (idx < cuda::NumOriginationChannels)
                channel[idx] = v;
        }
        instInputsPtr->channel.emplace_back(std::move(channel));
        
        const auto& loanSizeSeries = inst.loanSizeSeries();
        std::copy(loanSizeSeries.begin(), loanSizeSeries.end(), std::back_inserter(instInputsPtr->loanSizeSeries));
        const auto& factorRatioSeries = inst.factorRatioSeries();
        std::copy(factorRatioSeries.begin(), factorRatioSeries.end(), std::back_inserter(instInputsPtr->factorRatioSeries));
        const auto& ltvSeries = inst.ltvSeries();
        std::copy(ltvSeries.begin(), ltvSeries.end(), std::back_inserter(instInputsPtr->ltvSeries));
        const auto& wamSeries = inst.wamSeries();
        std::copy(wamSeries.begin(), wamSeries.end(), std::back_inserter(instInputsPtr->wamSeries));
        const auto& walaSeries = inst.walaSeries();
        std::copy(walaSeries.begin(), walaSeries.end(), std::back_inserter(instInputsPtr->walaSeries));
        const auto& cumHpaSeries = inst.cumHpaSeries();
        std::copy(cumHpaSeries.begin(), cumHpaSeries.end(), std::back_inserter(instInputsPtr->cumHpaSeries));
        const auto& projHpa1YSeries = inst.projHpa1ySeries();
        std::copy(projHpa1YSeries.begin(), projHpa1YSeries.end(), std::back_inserter(instInputsPtr->projHpa1YSeries));
        const auto& projHpa2YSeries = inst.projHpa2ySeries();
        std::copy(projHpa2YSeries.begin(), projHpa2YSeries.end(), std::back_inserter(instInputsPtr->projHpa2YSeries));
        const auto& projHpa3YSeries = inst.projHpa3ySeries();
        std::copy(projHpa3YSeries.begin(), projHpa3YSeries.end(), std::back_inserter(instInputsPtr->projHpa3YSeries));
        const auto& projHpa5YSeries = inst.projHpa5ySeries();
        std::copy(projHpa5YSeries.begin(), projHpa5YSeries.end(), std::back_inserter(instInputsPtr->projHpa5YSeries));
        
        const auto& totalSmmMult = inst.totalSmmMult();
        std::copy(totalSmmMult.first.begin(), totalSmmMult.first.end(), std::back_inserter(instInputsPtr->multTotalSmmX));
        std::copy(totalSmmMult.second.begin(), totalSmmMult.second.end(), std::back_inserter(instInputsPtr->multTotalSmmY));

        const auto& refiSmmMult = inst.refinanceSmmMult();
        std::copy(refiSmmMult.first.begin(), refiSmmMult.first.end(), std::back_inserter(instInputsPtr->multRefiSmmX));
        std::copy(refiSmmMult.second.begin(), refiSmmMult.second.end(), std::back_inserter(instInputsPtr->multRefiSmmY));

        const auto& htSmmMult = inst.turnoverSmmMult();
        std::copy(htSmmMult.first.begin(), htSmmMult.first.end(), std::back_inserter(instInputsPtr->multHtSmmX));
        std::copy(htSmmMult.second.begin(), htSmmMult.second.end(), std::back_inserter(instInputsPtr->multHtSmmY));

        const auto& coSmmMult = inst.cashoutSmmMult();
        std::copy(coSmmMult.first.begin(), coSmmMult.first.end(), std::back_inserter(instInputsPtr->multCoSmmX));
        std::copy(coSmmMult.second.begin(), coSmmMult.second.end(), std::back_inserter(instInputsPtr->multCoSmmY));

        const auto& ctSmmMult = inst.curtailmentSmmMult();
        std::copy(ctSmmMult.first.begin(), ctSmmMult.first.end(), std::back_inserter(instInputsPtr->multCtSmmX));
        std::copy(ctSmmMult.second.begin(), ctSmmMult.second.end(), std::back_inserter(instInputsPtr->multCtSmmY));

        instInputsPtr->multTotalSmmStartPos.emplace_back(instInputsPtr->multTotalSmmX.size());
        instInputsPtr->multRefiSmmStartPos.emplace_back(instInputsPtr->multRefiSmmX.size());
        instInputsPtr->multHtSmmStartPos.emplace_back(instInputsPtr->multHtSmmX.size());
        instInputsPtr->multCoSmmStartPos.emplace_back(instInputsPtr->multCoSmmX.size());
        instInputsPtr->multCtSmmStartPos.emplace_back(instInputsPtr->multCtSmmX.size());
    }

    return instInputsPtr;
}

cuda::cfpm::CfpmMultiplier CfpmGpuImpl::createGpuCfpmMultiplier(const SessionType& session) const
{
    const auto& multiplier = session.multiplier();
    cuda::cfpm::CfpmMultiplier gpuMultiplier(
        multiplier.KnobRefinance,
        multiplier.KnobTurnover,
        multiplier.KnobPrepay,
        multiplier.ExtraElbowShift.first(),
        multiplier.ExtraElbowShift.second(),
        multiplier.TotalSmm.first(),
        multiplier.TotalSmm.second(),
        multiplier.RefinanceSmm.first(),
        multiplier.RefinanceSmm.second(),
        multiplier.TurnoverSmm.first(),
        multiplier.TurnoverSmm.second(),
        multiplier.CashoutSmm.first(),
        multiplier.CashoutSmm.second(),
        multiplier.CurtailmentSmm.first(),
        multiplier.CurtailmentSmm.second(),
        multiplier.BurnoutCumulativeSpeed.first(),
        multiplier.BurnoutCumulativeSpeed.second(),
        multiplier.TurnoverBurnoutCumulativeSpeed.first(),
        multiplier.TurnoverBurnoutCumulativeSpeed.second(),
        multiplier.RefinanceMediaEffect.first(),
        multiplier.RefinanceMediaEffect.second(),
        multiplier.TurnoverLockinEffect.first(),
        multiplier.TurnoverLockinEffect.second(),
        multiplier.RefinanceRamping.first(),
        multiplier.RefinanceRamping.second(),
        multiplier.TurnoverRamping.first(),
        multiplier.TurnoverRamping.second(),
        multiplier.CashoutRamping.first(),
        multiplier.CashoutRamping.second()
    );

    return gpuMultiplier;
}

}  // namespace wfmcm

}  // namespace wfmcm