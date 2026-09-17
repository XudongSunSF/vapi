// #ifdef USE_GPU
#include <src/core/behavioral/cfpm/gpu/CfpmCalculator.h>

#include <src/core/behavioral/cfpm/detail/CfpmParameter.h>
#include <src/core/behavioral/detail/BehavioralModelUtils.h>

#include <mortgage/utility/strings/from_string.h>
#include <utils/pinned_memory_resource.hpp>

#include <chrono>

namespace wfmcm::gpu {

CfpmCalculator::CfpmCalculator()
{
    // Get the singleton instance of the pinned memory resource.
    auto pinned_mr = wfmcm::cuda::pinned_memory_resource::instance();
    // Initialize the prepay_ member with a new Prepay object that uses the
    // pinned memory resource for all its internal vector allocations.
    prepay_ = std::make_unique<wfmcm::cuda::cfpm::Prepay>(pinned_mr);
}

double CfpmCalculator::derived_rate(MortgageBehavioralSubModelType mdl, double pmms15, double pmms30) const
{
    auto it = derivedRateParams_.find(mdl);
    if (it == derivedRateParams_.end()) {
        THROW("unsupport CFPM model types when computing derived rates")
    }
    return it->second.DerivedRatePmms15Coef * pmms15 +
           it->second.DerivedRatePmms30Coef * pmms30 +
           it->second.DerivedRateSpread;

}

void CfpmCalculator::assign(std::map<MortgageBehavioralSubModelType, detail::CfpmParameter>&& params)
{
    using wfmcm::detail::build_weighted_avg;
    
    // will use two passes to fill PrepayData
    // first pass is used to determine the sizes so no reallocation and move when 
    // concatenating vectors
    const auto numModels = params.size();

    const size_t numSeasons = 12;
    // number of elements in enum classes
    // OccupancyType, PropertyType, PurposeType
    const size_t numOccupancyTypes = 3;
    const size_t numPropertyTypes = 7;
    const size_t numPurposeTypes = 6;
    const size_t seasonLen = numModels * numSeasons;
    const size_t occupancyLen = numModels * numOccupancyTypes;
    const size_t propertyLen = numModels * numPropertyTypes;
    const size_t purposeLen = numModels * numPurposeTypes;

    // total length of each variable
    size_t coHpa1y{0}, coCumHpa{0}, coAgeDecay{0}, coAge{0}, coFico{0};
    size_t coCltv{0}, coCals{0}, coValuationYear{0}, coPeriodMultiplier{0};
    
    size_t ctWam{0}, ctAge{0}, ctFico{0}, ctCals{0}, ctFactorRatio{0};
    size_t ctCltv{0}, ctPurchaseAge{0}, ctPurchaseAgeWacAdj{0};
    size_t ctPeriodMultiplier{0};

    size_t htAgeDecay{0}, htHpa2y{0}, htLockin{0}, htLockinCals{0};
    size_t htHighLtvAgeMult{0}, htCltv{0}, htLockinDecay{0}, htFico{0};
    size_t htSato{0}, htCals{0}, htLockinDecayWala{0};
    size_t htLockinDecayCumLockin{0}, htPeriodMultiplier{0};
    size_t htPurchaseAge{0};

    size_t refiNontpoAge{0}, refiCorrespondentAge{0};
    size_t refiBrokerAge{0}, refiFico{0}, refiCltv{0};
    size_t refiHpa3y{0}, refiTurbo{0}, refiScurve{0};
    size_t refiBurnout{0}, refiAgingDecay{0}, refiSato{0};
    size_t refiHighCltvPenalty{0}, refiPeriodMultiplier{0};
    size_t refiCalsAge{0}, refiVintageAdj{0};

    size_t elbowAls{0}, elbowFico{0}, elbowCltv{0};
    size_t elbowOccupancyCltv_SecondHome{0};
    size_t elbowOccupancyCltv_Investor_OldLlpa{0};
    size_t elbowSato{0}, elbowFicoWalaCure{0};
    size_t elbowFicoCltv{0};

    size_t calsAdjMultLen{0};

    auto it = params.begin();
    Turbo_ = std::move(it->second.Prepay_.Turbo_);

    size_t mdlIdx = 0;
    for (const auto& [mdl, param] : params) {

        models_[mdl] = mdlIdx++;
        const auto& prepay = param.Prepay_;
        const auto& co = prepay.Cashout_;
        const auto& ct = prepay.Curtailment_;
        const auto& ht = prepay.Turnover_;
        const auto& refi = prepay.Refinance_;
        
        coHpa1y += co.Hpa1y.X.size();
        coCumHpa += co.CumHpa.X.size();
        coAgeDecay += co.AgeDecay.X.size();
        coAge += co.Age.X.size();
        coFico += co.Fico.X.size();
        coCltv += co.Cltv.X.size();
        coCals += co.Cals.X.size();
        coValuationYear += co.ValuationYear.X.size();
        coPeriodMultiplier += co.PeriodMultiplier.X.size();

        ctWam += ct.Wam.X.size();
        ctAge += ct.Age.X.size();
        ctFico += ct.Fico.X.size();
        ctCals += ct.Cals.X.size();
        ctFactorRatio += ct.FactorRatio.X.size();
        ctCltv += ct.Cltv.X.size();
        ctPurchaseAge += ct.PurchaseAge.X.size();
        ctPurchaseAgeWacAdj += ct.PurchaseAgeWacAdj.isValid() ? ct.PurchaseAgeWacAdj.X.size() : 1;
        ctPeriodMultiplier += ct.PeriodMultiplier.X.size();

        htAgeDecay += ht.AgeDecay.X.size();
        htHpa2y += ht.Hpa2y.X.size();
        htLockin += ht.Lockin.X.size();
        htLockinCals += ht.LockinCals.X.size();
        htHighLtvAgeMult += ht.HighLtvAgeMult.X.size();
        htCltv += ht.Cltv.X.size();
        htLockinDecay += ht.LockinDecay.X.size();
        htFico += ht.Fico.X.size();
        htSato += ht.Sato.X.size();
        htCals += ht.Cals.X.size();
        htLockinDecayWala += ht.LockinDecayWala.isValid() ? ht.LockinDecayWala.X.size() : 1;
        htLockinDecayCumLockin += ht.LockinDecayCumLockin.isValid() ? ht.LockinDecayCumLockin.X.size() : 1;
        htPeriodMultiplier += ht.PeriodMultiplier.X.size();
        htPurchaseAge += ht.PurchaseAge.X.size();

        refiNontpoAge += refi.NontpoAge.X.size();
        refiCorrespondentAge += refi.CorrespondentAge.X.size();
        refiBrokerAge += refi.BrokerAge.X.size();
        refiFico += refi.Fico.X.size();
        refiCltv += refi.Cltv.X.size();
        refiHpa3y += refi.Hpa3y.X.size();
        refiTurbo += refi.Turbo.X.size();
        refiScurve += refi.Scurve.X.size();
        refiBurnout += refi.Burnout.X.size();
        refiAgingDecay += refi.AgingDecay.X.size();
        refiSato += refi.Sato.X.size();
        refiHighCltvPenalty += refi.HighCltvPenalty.X.size();
        refiPeriodMultiplier += refi.PeriodMultiplier.X.size();
        refiCalsAge += refi.CalsAge.X.size();
        refiVintageAdj += refi.VintageAdj.X.size();

        elbowAls += prepay.ElbowAls.X.size();
        elbowFico += prepay.ElbowFico.X.size();
        elbowCltv += prepay.ElbowCltv.X.size();
        elbowOccupancyCltv_SecondHome += prepay.ElbowOccupancyCltv_SecondHome.X.size();
        elbowOccupancyCltv_Investor_OldLlpa += prepay.ElbowOccupancyCltv_Investor_OldLlpa.X.size();
        elbowSato += prepay.ElbowSato.X.size();
        elbowFicoWalaCure += prepay.ElbowFicoWalaCure.X.size();
        elbowFicoCltv += prepay.ElbowFicoCltv.X.size();

        calsAdjMultLen += prepay.CalsAdjMult.X.size();
    }

    // allocate vector size
    auto& co_ = prepay_->cashout;
    auto& ct_ = prepay_->curtailment;
    auto& ht_ = prepay_->turnover;
    auto& refi_ = prepay_->refinance;
    auto& elbow_ = prepay_->elbow;

    co_.Hpa1y_x.reserve(coHpa1y);
    co_.Hpa1y_y.reserve(coHpa1y);
    co_.Hpa1y_start_indices.reserve(numModels + 1);

    co_.CumHpa_x.reserve(coCumHpa);
    co_.CumHpa_y.reserve(coCumHpa);
    co_.CumHpa_start_indices.reserve(numModels + 1);

    co_.AgeDecay_x.reserve(coAgeDecay);
    co_.AgeDecay_y.reserve(coAgeDecay);
    co_.AgeDecay_start_indices.reserve(numModels + 1);

    co_.Age_x.reserve(coAge);
    co_.Age_y.reserve(coAge);
    co_.Age_start_indices.reserve(numModels + 1);

    co_.Fico_x.reserve(coFico);
    co_.Fico_y.reserve(coFico);
    co_.Fico_start_indices.reserve(numModels + 1);

    co_.Cltv_x.reserve(coCltv);
    co_.Cltv_y.reserve(coCltv);
    co_.Cltv_start_indices.reserve(numModels + 1);

    co_.Lockin_x.reserve(0); // You need to compute and use the correct size for Lockin if available
    co_.Lockin_y.reserve(0);
    co_.Lockin_start_indices.reserve(numModels + 1);

    co_.Cals_x.reserve(coCals);
    co_.Cals_y.reserve(coCals);
    co_.Cals_start_indices.reserve(numModels + 1);

    co_.ValuationYear_x.reserve(coValuationYear);
    co_.ValuationYear_y.reserve(coValuationYear);
    co_.ValuationYear_start_indices.reserve(numModels + 1);

    co_.PeriodMultiplier_x.reserve(coPeriodMultiplier);
    co_.PeriodMultiplier_y.reserve(coPeriodMultiplier);
    co_.PeriodMultiplier_start_indices.reserve(numModels + 1);

    co_.ServicerFast.reserve(numModels);
    co_.ServicerSlow.reserve(numModels);
    co_.LockinAlpha.reserve(numModels);
    co_.Intercept.reserve(numModels);
    co_.ModelMultiplier.reserve(numModels);

    ct_.Wam_x.reserve(ctWam);
    ct_.Wam_y.reserve(ctWam);
    ct_.Wam_start_indices.reserve(numModels + 1);

    ct_.Age_x.reserve(ctAge);
    ct_.Age_y.reserve(ctAge);
    ct_.Age_start_indices.reserve(numModels + 1);

    ct_.Fico_x.reserve(ctFico);
    ct_.Fico_y.reserve(ctFico);
    ct_.Fico_start_indices.reserve(numModels + 1);

    ct_.Cals_x.reserve(ctCals);
    ct_.Cals_y.reserve(ctCals);
    ct_.Cals_start_indices.reserve(numModels + 1);

    ct_.FactorRatio_x.reserve(ctFactorRatio);
    ct_.FactorRatio_y.reserve(ctFactorRatio);
    ct_.FactorRatio_start_indices.reserve(numModels + 1);

    ct_.Cltv_x.reserve(ctCltv);
    ct_.Cltv_y.reserve(ctCltv);
    ct_.Cltv_start_indices.reserve(numModels + 1);

    ct_.PurchaseAge_x.reserve(ctPurchaseAge);
    ct_.PurchaseAge_y.reserve(ctPurchaseAge);
    ct_.PurchaseAge_start_indices.reserve(numModels + 1);

    ct_.PurchaseAgeWacAdj_x.reserve(ctPurchaseAgeWacAdj);
    ct_.PurchaseAgeWacAdj_y.reserve(ctPurchaseAgeWacAdj);
    ct_.PurchaseAgeWacAdj_start_indices.reserve(numModels + 1);

    ct_.PeriodMultiplier_x.reserve(ctPeriodMultiplier);
    ct_.PeriodMultiplier_y.reserve(ctPeriodMultiplier);
    ct_.PeriodMultiplier_start_indices.reserve(numModels + 1);

    ct_.Intercept.reserve(numModels);
    ct_.ModelMultiplier.reserve(numModels);
    ct_.RegimeDate.reserve(numModels);
    ct_.RegimeMult.reserve(numModels); 
    ct_.RegimeRamping.reserve(numModels);

    ht_.AgeDecay_x.reserve(htAgeDecay);
    ht_.AgeDecay_y.reserve(htAgeDecay);
    ht_.AgeDecay_start_indices.reserve(numModels + 1);

    ht_.Hpa2y_x.reserve(htHpa2y);
    ht_.Hpa2y_y.reserve(htHpa2y);
    ht_.Hpa2y_start_indices.reserve(numModels + 1);

    ht_.Lockin_x.reserve(htLockin);
    ht_.Lockin_y.reserve(htLockin);
    ht_.Lockin_start_indices.reserve(numModels + 1);

    ht_.LockinCals_x.reserve(htLockinCals);
    ht_.LockinCals_y.reserve(htLockinCals);
    ht_.LockinCals_start_indices.reserve(numModels + 1);

    ht_.HighLtvAgeMult_x.reserve(htHighLtvAgeMult);
    ht_.HighLtvAgeMult_y.reserve(htHighLtvAgeMult);
    ht_.HighLtvAgeMult_start_indices.reserve(numModels + 1);

    ht_.Cltv_x.reserve(htCltv);
    ht_.Cltv_y.reserve(htCltv);
    ht_.Cltv_start_indices.reserve(numModels + 1);

    ht_.LockinDecay_x.reserve(htLockinDecay);
    ht_.LockinDecay_y.reserve(htLockinDecay);
    ht_.LockinDecay_start_indices.reserve(numModels + 1);

    ht_.Fico_x.reserve(htFico);
    ht_.Fico_y.reserve(htFico);
    ht_.Fico_start_indices.reserve(numModels + 1);

    ht_.Sato_x.reserve(htSato);
    ht_.Sato_y.reserve(htSato);
    ht_.Sato_start_indices.reserve(numModels + 1);

    ht_.Cals_x.reserve(htCals);
    ht_.Cals_y.reserve(htCals);
    ht_.Cals_start_indices.reserve(numModels + 1);

    ht_.LockinDecayWala_x.reserve(htLockinDecayWala);
    ht_.LockinDecayWala_y.reserve(htLockinDecayWala);
    ht_.LockinDecayWala_start_indices.reserve(numModels + 1);

    ht_.LockinDecayCumLockin_x.reserve(htLockinDecayCumLockin);
    ht_.LockinDecayCumLockin_y.reserve(htLockinDecayCumLockin);
    ht_.LockinDecayCumLockin_start_indices.reserve(numModels + 1);

    ht_.PeriodMultiplier_x.reserve(htPeriodMultiplier);
    ht_.PeriodMultiplier_y.reserve(htPeriodMultiplier);
    ht_.PeriodMultiplier_start_indices.reserve(numModels + 1);

    ht_.PurchaseAge_x.reserve(htPurchaseAge);
    ht_.PurchaseAge_y.reserve(htPurchaseAge);
    ht_.PurchaseAge_z.reserve(htPurchaseAge);
    ht_.PurchaseAge_start_indices.reserve(numModels + 1);

    ht_.Seasoning.resize(seasonLen);
    ht_.Seasoning_start_indices.reserve(numModels + 1);
    ht_.Property.resize(propertyLen);
    ht_.Property_start_indices.reserve(numModels + 1);

    ht_.LockinAlpha.reserve(numModels);
    ht_.HighOltvCut.reserve(numModels);
    ht_.HighCltvCut.reserve(numModels);
    ht_.Intercept.reserve(numModels);
    ht_.ServicerFast.reserve(numModels);
    ht_.ServicerSlow.reserve(numModels);
    ht_.ModelMultiplier.reserve(numModels);
    ht_.EitCeiling.reserve(numModels);

    refi_.NontpoAge_x.reserve(refiNontpoAge);
    refi_.NontpoAge_y.reserve(refiNontpoAge);
    refi_.NontpoAge_start_indices.reserve(numModels + 1);

    refi_.CorrespondentAge_x.reserve(refiCorrespondentAge);
    refi_.CorrespondentAge_y.reserve(refiCorrespondentAge);
    refi_.CorrespondentAge_start_indices.reserve(numModels + 1);

    refi_.BrokerAge_x.reserve(refiBrokerAge);
    refi_.BrokerAge_y.reserve(refiBrokerAge);
    refi_.BrokerAge_start_indices.reserve(numModels + 1);

    refi_.Fico_x.reserve(refiFico);
    refi_.Fico_y.reserve(refiFico);
    refi_.Fico_start_indices.reserve(numModels + 1);

    refi_.Cltv_x.reserve(refiCltv);
    refi_.Cltv_y.reserve(refiCltv);
    refi_.Cltv_start_indices.reserve(numModels + 1);

    refi_.Hpa3y_x.reserve(refiHpa3y);
    refi_.Hpa3y_y.reserve(refiHpa3y);
    refi_.Hpa3y_start_indices.reserve(numModels + 1);

    refi_.Turbo_x.reserve(refiTurbo);
    refi_.Turbo_y.reserve(refiTurbo);
    refi_.Turbo_start_indices.reserve(numModels + 1);

    refi_.Scurve_x.reserve(refiScurve);
    refi_.Scurve_y.reserve(refiScurve);
    refi_.Scurve_start_indices.reserve(numModels + 1);

    refi_.Burnout_x.reserve(refiBurnout);
    refi_.Burnout_y.reserve(refiBurnout);
    refi_.Burnout_start_indices.reserve(numModels + 1);

    refi_.AgingDecay_x.reserve(refiAgingDecay);
    refi_.AgingDecay_y.reserve(refiAgingDecay);
    refi_.AgingDecay_start_indices.reserve(numModels + 1);

    refi_.Sato_x.reserve(refiSato);
    refi_.Sato_y.reserve(refiSato);
    refi_.Sato_start_indices.reserve(numModels + 1);

    refi_.HighCltvPenalty_x.reserve(refiHighCltvPenalty);
    refi_.HighCltvPenalty_y.reserve(refiHighCltvPenalty);
    refi_.HighCltvPenalty_start_indices.reserve(numModels + 1);

    refi_.PeriodMultiplier_x.reserve(refiPeriodMultiplier);
    refi_.PeriodMultiplier_y.reserve(refiPeriodMultiplier);
    refi_.PeriodMultiplier_start_indices.reserve(numModels + 1);

    refi_.CalsAge_x.reserve(refiCalsAge);
    refi_.CalsAge_y.reserve(refiCalsAge);
    refi_.CalsAge_z.reserve(refiCalsAge);
    refi_.CalsAge_start_indices.reserve(numModels + 1);

    refi_.VintageAdj_x.reserve(refiVintageAdj);
    refi_.VintageAdj_y.reserve(refiVintageAdj);
    refi_.VintageAdj_start_indices.reserve(numModels + 1);

    refi_.PropertyInspectionWaiver.reserve(numModels);
    refi_.EitCeiling.reserve(numModels);
    refi_.EitThreshold.reserve(numModels);
    refi_.Intercept.reserve(numModels);
    refi_.Lag1Weight.reserve(numModels);
    refi_.ServicerFast.reserve(numModels);
    refi_.ServicerSlow.reserve(numModels);
    refi_.ModelMultiplier.reserve(numModels);

    refi_.ValuationDateAdjCut.reserve(numModels);
    refi_.HighCltvPenaltyBegin.reserve(numModels);
    refi_.HighCltvPenaltyEnd.reserve(numModels);

    refi_.Occupancy.resize(occupancyLen);
    refi_.Occupancy_start_indices.reserve(numModels + 1);

    refi_.Property.resize(propertyLen);
    refi_.Property_start_indices.reserve(numModels + 1);

    elbow_.Als_x.reserve(elbowAls);
    elbow_.Als_y.reserve(elbowAls);
    elbow_.Als_start_indices.reserve(numModels + 1);

    elbow_.Fico_x.reserve(elbowFico);
    elbow_.Fico_y.reserve(elbowFico);
    elbow_.Fico_start_indices.reserve(numModels + 1);

    elbow_.Cltv_x.reserve(elbowCltv);
    elbow_.Cltv_y.reserve(elbowCltv);
    elbow_.Cltv_start_indices.reserve(numModels + 1);

    elbow_.OccupancyCltv_SecondHome_x.reserve(elbowOccupancyCltv_SecondHome);
    elbow_.OccupancyCltv_SecondHome_y.reserve(elbowOccupancyCltv_SecondHome);
    elbow_.OccupancyCltv_SecondHome_start_indices.reserve(numModels + 1);

    elbow_.OccupancyCltv_Investor_OldLlpa_x.reserve(elbowOccupancyCltv_Investor_OldLlpa);
    elbow_.OccupancyCltv_Investor_OldLlpa_y.reserve(elbowOccupancyCltv_Investor_OldLlpa);
    elbow_.OccupancyCltv_Investor_OldLlpa_start_indices.reserve(numModels + 1);

    elbow_.OccupancyCltv_Investor_NewLlpa_x.reserve(0);
    elbow_.OccupancyCltv_Investor_NewLlpa_y.reserve(0);
    elbow_.OccupancyCltv_Investor_NewLlpa_start_indices.reserve(numModels + 1);

    elbow_.Sato_x.reserve(elbowSato);
    elbow_.Sato_y.reserve(elbowSato);
    elbow_.Sato_start_indices.reserve(numModels + 1);

    elbow_.FicoWalaCure_x.reserve(elbowFicoWalaCure);
    elbow_.FicoWalaCure_y.reserve(elbowFicoWalaCure);
    elbow_.FicoWalaCure_z.reserve(elbowFicoWalaCure);
    elbow_.FicoWalaCure_start_indices.reserve(numModels + 1);

    elbow_.FicoCltv_x.reserve(elbowFicoCltv);
    elbow_.FicoCltv_y.reserve(elbowFicoCltv);
    elbow_.FicoCltv_z.reserve(elbowFicoCltv);
    elbow_.FicoCltv_start_indices.reserve(numModels + 1);

    elbow_.FicoCureCut.reserve(numModels);
    elbow_.Adj.reserve(numModels);
    elbow_.RefinanceAdj.reserve(numModels);
    elbow_.SatoRamp.reserve(numModels);
    elbow_.PropertyAdj_Condo_HighCltv.reserve(numModels);
    elbow_.Intercept.reserve(numModels);
    elbow_.HighCltvCut.reserve(numModels);

    elbow_.AdjBegin.reserve(numModels);
    elbow_.AdjEnd.reserve(numModels);
    elbow_.RefinanceAdjBegin.reserve(numModels);
    elbow_.RefinanceAdjEnd.reserve(numModels);
    elbow_.LlpaCut.reserve(numModels);
    elbow_.SecondHomeCltvCut.reserve(numModels);

    elbow_.Occupancy.resize(occupancyLen);
    elbow_.Occupancy_start_indices.reserve(numModels + 1);

    elbow_.Property.resize(propertyLen);
    elbow_.Property_start_indices.reserve(numModels + 1);

    elbow_.Purpose.resize(purposeLen);
    elbow_.Purpose_start_indices.reserve(numModels + 1);

    prepay_->CalsAdjMult_x.reserve(calsAdjMultLen);
    prepay_->CalsAdjMult_y.reserve(calsAdjMultLen);
    prepay_->CalsAdjMult_z.reserve(calsAdjMultLen);
    prepay_->CalsAdjMult_start_indices.reserve(numModels + 1);

    prepay_->BlendRateCut.reserve(numModels);
    prepay_->WamCorrection.reserve(numModels);

    statefunc_.CurtailmentState.reserve(numModels);
    statefunc_. TurnoverState.reserve(numModels);
    statefunc_. RefinanceState.reserve(numModels);
    statefunc_. ElbowState.reserve(numModels);

    mdlIdx = 0;
    for (auto& [mdl, param] : params) {
        const auto& prepay = param.Prepay_;
        const auto& co = prepay.Cashout_;
        const auto& ct = prepay.Curtailment_;
        const auto& ht = prepay.Turnover_;
        const auto& refi = prepay.Refinance_;

        // move co.Hpa1y.X to co_.Hpa1y_x
        co_.Hpa1y_start_indices.push_back(co_.Hpa1y_x.size());
        std::move(co.Hpa1y.X.begin(), co.Hpa1y.X.end(), std::back_inserter(co_.Hpa1y_x));
        std::move(co.Hpa1y.Y.begin(), co.Hpa1y.Y.end(), std::back_inserter(co_.Hpa1y_y));

        co_.CumHpa_start_indices.push_back(co_.CumHpa_x.size());
        std::move(co.CumHpa.X.begin(), co.CumHpa.X.end(), std::back_inserter(co_.CumHpa_x));
        std::move(co.CumHpa.Y.begin(), co.CumHpa.Y.end(), std::back_inserter(co_.CumHpa_y));

        co_.AgeDecay_start_indices.push_back(co_.AgeDecay_x.size());
        std::move(co.AgeDecay.X.begin(), co.AgeDecay.X.end(), std::back_inserter(co_.AgeDecay_x));
        std::move(co.AgeDecay.Y.begin(), co.AgeDecay.Y.end(), std::back_inserter(co_.AgeDecay_y));

        co_.Age_start_indices.push_back(co_.Age_x.size());
        std::move(co.Age.X.begin(), co.Age.X.end(), std::back_inserter(co_.Age_x));
        std::move(co.Age.Y.begin(), co.Age.Y.end(), std::back_inserter(co_.Age_y));

        co_.Fico_start_indices.push_back(co_.Fico_x.size());
        std::move(co.Fico.X.begin(), co.Fico.X.end(), std::back_inserter(co_.Fico_x));
        std::move(co.Fico.Y.begin(), co.Fico.Y.end(), std::back_inserter(co_.Fico_y));

        co_.Cltv_start_indices.push_back(co_.Cltv_x.size());
        std::move(co.Cltv.X.begin(), co.Cltv.X.end(), std::back_inserter(co_.Cltv_x));
        std::move(co.Cltv.Y.begin(), co.Cltv.Y.end(), std::back_inserter(co_.Cltv_y));

        co_.Lockin_start_indices.push_back(co_.Lockin_x.size());
        std::move(co.Lockin.X.begin(), co.Lockin.X.end(), std::back_inserter(co_.Lockin_x));
        std::move(co.Lockin.Y.begin(), co.Lockin.Y.end(), std::back_inserter(co_.Lockin_y));

        co_.Cals_start_indices.push_back(co_.Cals_x.size());
        std::move(co.Cals.X.begin(), co.Cals.X.end(), std::back_inserter(co_.Cals_x));
        std::move(co.Cals.Y.begin(), co.Cals.Y.end(), std::back_inserter(co_.Cals_y));

        co_.ValuationYear_start_indices.push_back(co_.ValuationYear_x.size());
        std::transform(
            co.ValuationYear.X.begin(), co.ValuationYear.X.end(),
            std::back_inserter(co_.ValuationYear_x),
            [](auto y) { return cuda::yearsSinceBase<double>(y); }
        );
        std::move(co.ValuationYear.Y.begin(), co.ValuationYear.Y.end(), std::back_inserter(co_.ValuationYear_y));

        co_.PeriodMultiplier_start_indices.push_back(co_.PeriodMultiplier_x.size());
        std::transform(
            co.PeriodMultiplier.X.begin(), co.PeriodMultiplier.X.end(),
            std::back_inserter(co_.PeriodMultiplier_x),
            [](auto y) { return cuda::monthsSinceBase<double>(y); }
        );
        std::move(co.PeriodMultiplier.Y.begin(), co.PeriodMultiplier.Y.end(), std::back_inserter(co_.PeriodMultiplier_y));

        co_.ServicerFast.push_back(co.ServicerFast);
        co_.ServicerSlow.push_back(co.ServicerSlow);
        co_.LockinAlpha.push_back(co.LockinAlpha);
        co_.Intercept.push_back(co.Intercept);
        co_.ModelMultiplier.push_back(co.ModelMultiplier);

        ct_.Wam_start_indices.push_back(ct_.Wam_x.size());
        std::move(ct.Wam.X.begin(), ct.Wam.X.end(), std::back_inserter(ct_.Wam_x));
        std::move(ct.Wam.Y.begin(), ct.Wam.Y.end(), std::back_inserter(ct_.Wam_y));

        ct_.Age_start_indices.push_back(ct_.Age_x.size());
        std::move(ct.Age.X.begin(), ct.Age.X.end(), std::back_inserter(ct_.Age_x));
        std::move(ct.Age.Y.begin(), ct.Age.Y.end(), std::back_inserter(ct_.Age_y));

        ct_.Fico_start_indices.push_back(ct_.Fico_x.size());
        std::move(ct.Fico.X.begin(), ct.Fico.X.end(), std::back_inserter(ct_.Fico_x));
        std::move(ct.Fico.Y.begin(), ct.Fico.Y.end(), std::back_inserter(ct_.Fico_y));

        ct_.Cals_start_indices.push_back(ct_.Cals_x.size());
        std::move(ct.Cals.X.begin(), ct.Cals.X.end(), std::back_inserter(ct_.Cals_x));
        std::move(ct.Cals.Y.begin(), ct.Cals.Y.end(), std::back_inserter(ct_.Cals_y));

        ct_.FactorRatio_start_indices.push_back(ct_.FactorRatio_x.size());
        std::move(ct.FactorRatio.X.begin(), ct.FactorRatio.X.end(), std::back_inserter(ct_.FactorRatio_x));
        std::move(ct.FactorRatio.Y.begin(), ct.FactorRatio.Y.end(), std::back_inserter(ct_.FactorRatio_y));

        ct_.Cltv_start_indices.push_back(ct_.Cltv_x.size());
        std::move(ct.Cltv.X.begin(), ct.Cltv.X.end(), std::back_inserter(ct_.Cltv_x));
        std::move(ct.Cltv.Y.begin(), ct.Cltv.Y.end(), std::back_inserter(ct_.Cltv_y));

        ct_.PurchaseAge_start_indices.push_back(ct_.PurchaseAge_x.size());
        std::move(ct.PurchaseAge.X.begin(), ct.PurchaseAge.X.end(), std::back_inserter(ct_.PurchaseAge_x));
        std::move(ct.PurchaseAge.Y.begin(), ct.PurchaseAge.Y.end(), std::back_inserter(ct_.PurchaseAge_y));

        ct_.PurchaseAgeWacAdj_start_indices.push_back(ct_.PurchaseAgeWacAdj_x.size());
        if (ct.PurchaseAgeWacAdj.isValid()) {
            std::move(ct.PurchaseAgeWacAdj.X.begin(), ct.PurchaseAgeWacAdj.X.end(), std::back_inserter(ct_.PurchaseAgeWacAdj_x));
            std::move(ct.PurchaseAgeWacAdj.Y.begin(), ct.PurchaseAgeWacAdj.Y.end(), std::back_inserter(ct_.PurchaseAgeWacAdj_y));
        }
        else {
            ct_.PurchaseAgeWacAdj_x.push_back(0.0);
            ct_.PurchaseAgeWacAdj_y.push_back(1.0);
        }
        ct_.PeriodMultiplier_start_indices.push_back(ct_.PeriodMultiplier_x.size());
        std::transform(
            ct.PeriodMultiplier.X.begin(), ct.PeriodMultiplier.X.end(),
            std::back_inserter(ct_.PeriodMultiplier_x),
            [](auto y) { return cuda::monthsSinceBase<double>(y); }
            
        );
        std::move(ct.PeriodMultiplier.Y.begin(), ct.PeriodMultiplier.Y.end(), std::back_inserter(ct_.PeriodMultiplier_y));
        
        ct_.Intercept.push_back(ct.Intercept);
        ct_.ModelMultiplier.push_back(ct.ModelMultiplier);
        ct_.RegimeDate.push_back(cuda::monthsSinceBase<double>(ct.RegimeDate));
        ct_.RegimeMult.push_back(ct.RegimeMult);
        ct_.RegimeRamping.push_back(ct.RegimeRamping);
        statefunc_.CurtailmentState.push_back(build_weighted_avg(param.Prepay_.Curtailment_.State, 1.));

        ht_.AgeDecay_start_indices.push_back(ht_.AgeDecay_x.size());
        std::move(ht.AgeDecay.X.begin(), ht.AgeDecay.X.end(), std::back_inserter(ht_.AgeDecay_x));
        std::move(ht.AgeDecay.Y.begin(), ht.AgeDecay.Y.end(), std::back_inserter(ht_.AgeDecay_y));

        ht_.Hpa2y_start_indices.push_back(ht_.Hpa2y_x.size());
        std::move(ht.Hpa2y.X.begin(), ht.Hpa2y.X.end(), std::back_inserter(ht_.Hpa2y_x));
        std::move(ht.Hpa2y.Y.begin(), ht.Hpa2y.Y.end(), std::back_inserter(ht_.Hpa2y_y));

        ht_.Lockin_start_indices.push_back(ht_.Lockin_x.size());
        std::move(ht.Lockin.X.begin(), ht.Lockin.X.end(), std::back_inserter(ht_.Lockin_x));
        std::move(ht.Lockin.Y.begin(), ht.Lockin.Y.end(), std::back_inserter(ht_.Lockin_y));

        ht_.LockinCals_start_indices.push_back(ht_.LockinCals_x.size());
        std::move(ht.LockinCals.X.begin(), ht.LockinCals.X.end(), std::back_inserter(ht_.LockinCals_x));
        std::move(ht.LockinCals.Y.begin(), ht.LockinCals.Y.end(), std::back_inserter(ht_.LockinCals_y));

        ht_.HighLtvAgeMult_start_indices.push_back(ht_.HighLtvAgeMult_x.size());
        std::move(ht.HighLtvAgeMult.X.begin(), ht.HighLtvAgeMult.X.end(), std::back_inserter(ht_.HighLtvAgeMult_x));
        std::move(ht.HighLtvAgeMult.Y.begin(), ht.HighLtvAgeMult.Y.end(), std::back_inserter(ht_.HighLtvAgeMult_y));

        ht_.Cltv_start_indices.push_back(ht_.Cltv_x.size());
        std::move(ht.Cltv.X.begin(), ht.Cltv.X.end(), std::back_inserter(ht_.Cltv_x));
        std::move(ht.Cltv.Y.begin(), ht.Cltv.Y.end(), std::back_inserter(ht_.Cltv_y));

        ht_.LockinDecay_start_indices.push_back(ht_.LockinDecay_x.size());
        std::move(ht.LockinDecay.X.begin(), ht.LockinDecay.X.end(), std::back_inserter(ht_.LockinDecay_x));
        std::move(ht.LockinDecay.Y.begin(), ht.LockinDecay.Y.end(), std::back_inserter(ht_.LockinDecay_y));

        ht_.Fico_start_indices.push_back(ht_.Fico_x.size());
        std::move(ht.Fico.X.begin(), ht.Fico.X.end(), std::back_inserter(ht_.Fico_x));
        std::move(ht.Fico.Y.begin(), ht.Fico.Y.end(), std::back_inserter(ht_.Fico_y));

        ht_.Sato_start_indices.push_back(ht_.Sato_x.size());
        std::move(ht.Sato.X.begin(), ht.Sato.X.end(), std::back_inserter(ht_.Sato_x));
        std::move(ht.Sato.Y.begin(), ht.Sato.Y.end(), std::back_inserter(ht_.Sato_y));

        ht_.Cals_start_indices.push_back(ht_.Cals_x.size());
        std::move(ht.Cals.X.begin(), ht.Cals.X.end(), std::back_inserter(ht_.Cals_x));
        std::move(ht.Cals.Y.begin(), ht.Cals.Y.end(), std::back_inserter(ht_.Cals_y));

        ht_.PurchaseAge_start_indices.push_back(ht_.PurchaseAge_x.size());
        std::move(ht.PurchaseAge.X.begin(), ht.PurchaseAge.X.end(), std::back_inserter(ht_.PurchaseAge_x));
        std::move(ht.PurchaseAge.Y.begin(), ht.PurchaseAge.Y.end(), std::back_inserter(ht_.PurchaseAge_y));
        std::move(ht.PurchaseAge.Z.begin(), ht.PurchaseAge.Z.end(), std::back_inserter(ht_.PurchaseAge_z));

        ht_.LockinDecayWala_start_indices.push_back(ht_.LockinDecayWala_x.size());
        if (ht.LockinDecayWala.isValid()) {
            std::move(ht.LockinDecayWala.X.begin(), ht.LockinDecayWala.X.end(), std::back_inserter(ht_.LockinDecayWala_x));
            std::move(ht.LockinDecayWala.Y.begin(), ht.LockinDecayWala.Y.end(), std::back_inserter(ht_.LockinDecayWala_y));
        }
        else {
            ht_.LockinDecayWala_x.push_back(0.0);
            ht_.LockinDecayWala_y.push_back(1.0);
        }
        ht_.LockinDecayCumLockin_start_indices.push_back(ht_.LockinDecayCumLockin_x.size());
        if (ht.LockinDecayCumLockin.isValid()) {
            std::move(ht.LockinDecayCumLockin.X.begin(), ht.LockinDecayCumLockin.X.end(), std::back_inserter(ht_.LockinDecayCumLockin_x));
            std::move(ht.LockinDecayCumLockin.Y.begin(), ht.LockinDecayCumLockin.Y.end(), std::back_inserter(ht_.LockinDecayCumLockin_y));
        }
        else {
            ht_.LockinDecayCumLockin_x.push_back(0.0);
            ht_.LockinDecayCumLockin_y.push_back(1.0);
        }
        ht_.PeriodMultiplier_start_indices.push_back(ht_.PeriodMultiplier_x.size());
        std::transform(
            ht.PeriodMultiplier.X.begin(), ht.PeriodMultiplier.X.end(),
            std::back_inserter(ht_.PeriodMultiplier_x),
            [](auto y) { return cuda::monthsSinceBase<double>(y); }
        );
        std::move(ht.PeriodMultiplier.Y.begin(), ht.PeriodMultiplier.Y.end(), std::back_inserter(ht_.PeriodMultiplier_y));

        const size_t seasonIdx = mdlIdx * 12;
        ht_.Seasoning_start_indices.push_back(seasonIdx);
        for (const auto& [k, v] : ht.Seasoning) {
            auto month = static_cast<size_t>(static_cast<unsigned>(k));
            ht_.Seasoning[seasonIdx + month - 1] = v;
        }
        const size_t propertyIdx = mdlIdx * cuda::NumPropertyTypes;
        ht_.Property_start_indices.push_back(propertyIdx);
        for (const auto& [k, v] : ht.Property) {
            auto idx = static_cast<size_t>(k);
            if (idx < cuda::NumPropertyTypes) // ignore unknown
                ht_.Property[propertyIdx + idx] = v;
        }

        ht_.LockinAlpha.push_back(ht.LockinAlpha);
        ht_.HighOltvCut.push_back(ht.HighOltvCut);
        ht_.HighCltvCut.push_back(ht.HighCltvCut);
        ht_.Intercept.push_back(ht.Intercept);
        ht_.ServicerFast.push_back(ht.ServicerFast);
        ht_.ServicerSlow.push_back(ht.ServicerSlow);
        ht_.ModelMultiplier.push_back(ht.ModelMultiplier);
        ht_.EitCeiling.push_back(is_unset_value(ht.EitCeiling) ? std::numeric_limits<double>::max() : ht.EitCeiling);
        statefunc_.TurnoverState.push_back(build_weighted_avg(ht.State, 1.));

        refi_.NontpoAge_start_indices.push_back(refi_.NontpoAge_x.size());
        std::move(refi.NontpoAge.X.begin(), refi.NontpoAge.X.end(), std::back_inserter(refi_.NontpoAge_x));
        std::move(refi.NontpoAge.Y.begin(), refi.NontpoAge.Y.end(), std::back_inserter(refi_.NontpoAge_y));

        refi_.CorrespondentAge_start_indices.push_back(refi_.CorrespondentAge_x.size());
        std::move(refi.CorrespondentAge.X.begin(), refi.CorrespondentAge.X.end(), std::back_inserter(refi_.CorrespondentAge_x));
        std::move(refi.CorrespondentAge.Y.begin(), refi.CorrespondentAge.Y.end(), std::back_inserter(refi_.CorrespondentAge_y));

        refi_.BrokerAge_start_indices.push_back(refi_.BrokerAge_x.size());
        std::move(refi.BrokerAge.X.begin(), refi.BrokerAge.X.end(), std::back_inserter(refi_.BrokerAge_x));
        std::move(refi.BrokerAge.Y.begin(), refi.BrokerAge.Y.end(), std::back_inserter(refi_.BrokerAge_y));

        refi_.Fico_start_indices.push_back(refi_.Fico_x.size());
        std::move(refi.Fico.X.begin(), refi.Fico.X.end(), std::back_inserter(refi_.Fico_x));
        std::move(refi.Fico.Y.begin(), refi.Fico.Y.end(), std::back_inserter(refi_.Fico_y));

        refi_.Cltv_start_indices.push_back(refi_.Cltv_x.size());
        std::move(refi.Cltv.X.begin(), refi.Cltv.X.end(), std::back_inserter(refi_.Cltv_x));
        std::move(refi.Cltv.Y.begin(), refi.Cltv.Y.end(), std::back_inserter(refi_.Cltv_y));

        refi_.Hpa3y_start_indices.push_back(refi_.Hpa3y_x.size());
        std::move(refi.Hpa3y.X.begin(), refi.Hpa3y.X.end(), std::back_inserter(refi_.Hpa3y_x));
        std::move(refi.Hpa3y.Y.begin(), refi.Hpa3y.Y.end(), std::back_inserter(refi_.Hpa3y_y));

        refi_.Turbo_start_indices.push_back(refi_.Turbo_x.size());
        std::move(refi.Turbo.X.begin(), refi.Turbo.X.end(), std::back_inserter(refi_.Turbo_x));
        std::move(refi.Turbo.Y.begin(), refi.Turbo.Y.end(), std::back_inserter(refi_.Turbo_y));

        refi_.Scurve_start_indices.push_back(refi_.Scurve_x.size());
        std::move(refi.Scurve.X.begin(), refi.Scurve.X.end(), std::back_inserter(refi_.Scurve_x));
        std::move(refi.Scurve.Y.begin(), refi.Scurve.Y.end(), std::back_inserter(refi_.Scurve_y));

        refi_.Burnout_start_indices.push_back(refi_.Burnout_x.size());
        std::move(refi.Burnout.X.begin(), refi.Burnout.X.end(), std::back_inserter(refi_.Burnout_x));
        std::move(refi.Burnout.Y.begin(), refi.Burnout.Y.end(), std::back_inserter(refi_.Burnout_y));

        refi_.AgingDecay_start_indices.push_back(refi_.AgingDecay_x.size());
        std::move(refi.AgingDecay.X.begin(), refi.AgingDecay.X.end(), std::back_inserter(refi_.AgingDecay_x));
        std::move(refi.AgingDecay.Y.begin(), refi.AgingDecay.Y.end(), std::back_inserter(refi_.AgingDecay_y));

        refi_.Sato_start_indices.push_back(refi_.Sato_x.size());
        std::move(refi.Sato.X.begin(), refi.Sato.X.end(), std::back_inserter(refi_.Sato_x));
        std::move(refi.Sato.Y.begin(), refi.Sato.Y.end(), std::back_inserter(refi_.Sato_y));

        refi_.HighCltvPenalty_start_indices.push_back(refi_.HighCltvPenalty_x.size());
        std::move(refi.HighCltvPenalty.X.begin(), refi.HighCltvPenalty.X.end(), std::back_inserter(refi_.HighCltvPenalty_x));
        std::move(refi.HighCltvPenalty.Y.begin(), refi.HighCltvPenalty.Y.end(), std::back_inserter(refi_.HighCltvPenalty_y));

        refi_.PeriodMultiplier_start_indices.push_back(refi_.PeriodMultiplier_x.size());
        std::transform(
            refi.PeriodMultiplier.X.begin(), refi.PeriodMultiplier.X.end(),
            std::back_inserter(refi_.PeriodMultiplier_x),
            [](auto y) { return cuda::monthsSinceBase<double>(y); }
        );
        std::move(refi.PeriodMultiplier.Y.begin(), refi.PeriodMultiplier.Y.end(), std::back_inserter(refi_.PeriodMultiplier_y));

        refi_.CalsAge_start_indices.push_back(refi_.CalsAge_x.size());
        std::move(refi.CalsAge.X.begin(), refi.CalsAge.X.end(), std::back_inserter(refi_.CalsAge_x));
        std::move(refi.CalsAge.Y.begin(), refi.CalsAge.Y.end(), std::back_inserter(refi_.CalsAge_y));
        std::move(refi.CalsAge.Z.begin(), refi.CalsAge.Z.end(), std::back_inserter(refi_.CalsAge_z));

        refi_.VintageAdj_start_indices.push_back(refi_.VintageAdj_x.size());
        std::transform(
            refi.VintageAdj.X.begin(), refi.VintageAdj.X.end(),
            std::back_inserter(refi_.VintageAdj_x),
            [](auto y) { return cuda::yearsSinceBase<double>(y); }
        );
        std::move(refi.VintageAdj.Y.begin(), refi.VintageAdj.Y.end(), std::back_inserter(refi_.VintageAdj_y));

        refi_.PropertyInspectionWaiver.push_back(refi.PropertyInspectionWaiver);
        refi_.EitCeiling.push_back(refi.EitCeiling);
        refi_.EitThreshold.push_back(refi.EitThreshold);
        refi_.Intercept.push_back(refi.Intercept);
        refi_.Lag1Weight.push_back(refi.Lag1Weight);
        refi_.ServicerFast.push_back(refi.ServicerFast);
        refi_.ServicerSlow.push_back(refi.ServicerSlow);
        refi_.ModelMultiplier.push_back(refi.ModelMultiplier);

        refi_.ValuationDateAdjCut.push_back(cuda::monthsSinceBase<size_t>(refi.ValuationDateAdjCut));
        refi_.HighCltvPenaltyBegin.push_back(cuda::monthsSinceBase<size_t>(refi.HighCltvPenaltyBegin));
        refi_.HighCltvPenaltyEnd.push_back(cuda::monthsSinceBase<size_t>(refi.HighCltvPenaltyEnd));

        const size_t refiOccupancyIdx = mdlIdx * cuda::NumOccupancyTypes;
        refi_.Occupancy_start_indices.push_back(refiOccupancyIdx);
        for (const auto& [k, v] : refi.Occupancy) {
            auto idx = static_cast<size_t>(k);
            if (idx < cuda::NumOccupancyTypes)
                refi_.Occupancy[refiOccupancyIdx + idx] = v;
        }

        const size_t refiPropertyIdx = mdlIdx * cuda::NumPropertyTypes;
        refi_.Property_start_indices.push_back(refiPropertyIdx);
        for (const auto& [k, v] : refi.Property) {
            auto idx = static_cast<size_t>(k);
            if (idx < cuda::NumPropertyTypes)
                refi_.Property[refiPropertyIdx + idx] = v;
        }

        statefunc_.RefinanceState.push_back(build_weighted_avg(refi.State, 1.));

        elbow_.Als_start_indices.push_back(elbow_.Als_x.size());
        std::move(prepay.ElbowAls.X.begin(), prepay.ElbowAls.X.end(), std::back_inserter(elbow_.Als_x));
        std::move(prepay.ElbowAls.Y.begin(), prepay.ElbowAls.Y.end(), std::back_inserter(elbow_.Als_y));

        elbow_.Fico_start_indices.push_back(elbow_.Fico_x.size());
        std::move(prepay.ElbowFico.X.begin(), prepay.ElbowFico.X.end(), std::back_inserter(elbow_.Fico_x));
        std::move(prepay.ElbowFico.Y.begin(), prepay.ElbowFico.Y.end(), std::back_inserter(elbow_.Fico_y));

        elbow_.Cltv_start_indices.push_back(elbow_.Cltv_x.size());
        std::move(prepay.ElbowCltv.X.begin(), prepay.ElbowCltv.X.end(), std::back_inserter(elbow_.Cltv_x));
        std::move(prepay.ElbowCltv.Y.begin(), prepay.ElbowCltv.Y.end(), std::back_inserter(elbow_.Cltv_y));

        elbow_.OccupancyCltv_SecondHome_start_indices.push_back(elbow_.OccupancyCltv_SecondHome_x.size());
        std::move(prepay.ElbowOccupancyCltv_SecondHome.X.begin(), prepay.ElbowOccupancyCltv_SecondHome.X.end(), std::back_inserter(elbow_.OccupancyCltv_SecondHome_x));
        std::move(prepay.ElbowOccupancyCltv_SecondHome.Y.begin(), prepay.ElbowOccupancyCltv_SecondHome.Y.end(), std::back_inserter(elbow_.OccupancyCltv_SecondHome_y));

        elbow_.OccupancyCltv_Investor_OldLlpa_start_indices.push_back(elbow_.OccupancyCltv_Investor_OldLlpa_x.size());
        std::move(prepay.ElbowOccupancyCltv_Investor_OldLlpa.X.begin(), prepay.ElbowOccupancyCltv_Investor_OldLlpa.X.end(), std::back_inserter(elbow_.OccupancyCltv_Investor_OldLlpa_x));
        std::move(prepay.ElbowOccupancyCltv_Investor_OldLlpa.Y.begin(), prepay.ElbowOccupancyCltv_Investor_OldLlpa.Y.end(), std::back_inserter(elbow_.OccupancyCltv_Investor_OldLlpa_y));

        elbow_.OccupancyCltv_Investor_NewLlpa_start_indices.push_back(elbow_.OccupancyCltv_Investor_NewLlpa_x.size());
        std::move(prepay.ElbowOccupancyCltv_Investor_NewLlpa.X.begin(), prepay.ElbowOccupancyCltv_Investor_NewLlpa.X.end(), std::back_inserter(elbow_.OccupancyCltv_Investor_NewLlpa_x));
        std::move(prepay.ElbowOccupancyCltv_Investor_NewLlpa.Y.begin(), prepay.ElbowOccupancyCltv_Investor_NewLlpa.Y.end(), std::back_inserter(elbow_.OccupancyCltv_Investor_NewLlpa_y));

        elbow_.Sato_start_indices.push_back(elbow_.Sato_x.size());
        std::move(prepay.ElbowSato.X.begin(), prepay.ElbowSato.X.end(), std::back_inserter(elbow_.Sato_x));
        std::move(prepay.ElbowSato.Y.begin(), prepay.ElbowSato.Y.end(), std::back_inserter(elbow_.Sato_y));

        elbow_.FicoWalaCure_start_indices.push_back(elbow_.FicoWalaCure_x.size());
        std::move(prepay.ElbowFicoWalaCure.X.begin(), prepay.ElbowFicoWalaCure.X.end(), std::back_inserter(elbow_.FicoWalaCure_x));
        std::move(prepay.ElbowFicoWalaCure.Y.begin(), prepay.ElbowFicoWalaCure.Y.end(), std::back_inserter(elbow_.FicoWalaCure_y));
        std::move(prepay.ElbowFicoWalaCure.Z.begin(), prepay.ElbowFicoWalaCure.Z.end(), std::back_inserter(elbow_.FicoWalaCure_z));

        elbow_.FicoCltv_start_indices.push_back(elbow_.FicoCltv_x.size());
        std::move(prepay.ElbowFicoCltv.X.begin(), prepay.ElbowFicoCltv.X.end(), std::back_inserter(elbow_.FicoCltv_x));
        std::move(prepay.ElbowFicoCltv.Y.begin(), prepay.ElbowFicoCltv.Y.end(), std::back_inserter(elbow_.FicoCltv_y));
        std::move(prepay.ElbowFicoCltv.Z.begin(), prepay.ElbowFicoCltv.Z.end(), std::back_inserter(elbow_.FicoCltv_z));

        // Scalars and 1D vectors
        elbow_.FicoCureCut.push_back(prepay.ElbowFicoCureCut);
        elbow_.Adj.push_back(prepay.ElbowAdj);
        elbow_.RefinanceAdj.push_back(prepay.ElbowRefinanceAdj);
        elbow_.SatoRamp.push_back(prepay.ElbowSatoRamp);
        elbow_.PropertyAdj_Condo_HighCltv.push_back(prepay.ElbowPropertyAdj_Condo_HighCltv);
        elbow_.Intercept.push_back(prepay.ElbowIntercept);
        elbow_.HighCltvCut.push_back(prepay.ElbowHighCltvCut);

        elbow_.AdjBegin.push_back(cuda::monthsSinceBase<size_t>(prepay.ElbowAdjBegin));
        elbow_.AdjEnd.push_back(cuda::monthsSinceBase<size_t>(prepay.ElbowAdjEnd));
        elbow_.RefinanceAdjBegin.push_back(cuda::monthsSinceBase<size_t>(prepay.ElbowRefinanceAdjBegin));
        elbow_.RefinanceAdjEnd.push_back(cuda::monthsSinceBase<size_t>(prepay.ElbowRefinanceAdjEnd));
        elbow_.LlpaCut.push_back(cuda::monthsSinceBase<size_t>(prepay.ElbowLlpaCut));
        elbow_.SecondHomeCltvCut.push_back(cuda::monthsSinceBase<size_t>(prepay.ElbowSecondHomeCltvCut));

        // Occupancy, Property, Purpose
        const size_t elbowOccupancyIdx = mdlIdx * cuda::NumOccupancyTypes;
        elbow_.Occupancy_start_indices.push_back(elbowOccupancyIdx);
        for (const auto& [k, v] : prepay.ElbowOccupancy) {
            auto idx = static_cast<size_t>(k);
            if (idx < cuda::NumOccupancyTypes)
                elbow_.Occupancy[elbowOccupancyIdx + idx] = v;
        }

        const size_t elbowPropertyIdx = mdlIdx * cuda::NumPropertyTypes;
        elbow_.Property_start_indices.push_back(elbowPropertyIdx);
        for (const auto& [k, v] : prepay.ElbowProperty) {
            auto idx = static_cast<size_t>(k);
            if (idx < cuda::NumPropertyTypes)
                elbow_.Property[elbowPropertyIdx + idx] = v;
        }

        const size_t elbowPurposeIdx = mdlIdx * cuda::NumPurposeTypes;
        elbow_.Purpose_start_indices.push_back(elbowPurposeIdx);
        for (const auto& [k, v] : prepay.ElbowPurpose) {
            auto idx = static_cast<size_t>(k);
            if (idx < cuda::NumPurposeTypes)
                elbow_.Purpose[elbowPurposeIdx + idx] = v;
        }
    
        statefunc_.ElbowState.push_back(build_weighted_avg(prepay.ElbowState, 0.));

        prepay_->CalsAdjMult_start_indices.push_back(prepay_->CalsAdjMult_x.size());
        if (prepay.CalsAdjMult.isValid()) {
            std::transform(
                prepay.CalsAdjMult.X.begin(), prepay.CalsAdjMult.X.end(),
                std::back_inserter(prepay_->CalsAdjMult_x),
                [](auto y) { return cuda::monthsSinceBase<double>(y); }
            );
            std::move(prepay.CalsAdjMult.Y.begin(), prepay.CalsAdjMult.Y.end(), std::back_inserter(prepay_->CalsAdjMult_y));
            std::move(prepay.CalsAdjMult.Z.begin(), prepay.CalsAdjMult.Z.end(), std::back_inserter(prepay_->CalsAdjMult_z));
        }
        else {
            prepay_->CalsAdjMult_x.push_back(0.0);
            prepay_->CalsAdjMult_y.push_back(0.0);
            prepay_->CalsAdjMult_z.push_back(1.0);
        }

        prepay_->BlendRateCut.push_back(prepay.BlendRateCut);
        prepay_->WamCorrection.push_back(prepay.WamCorrection);
        derivedRateParams_.emplace(std::piecewise_construct, std::forward_as_tuple(mdl),
        std::forward_as_tuple(prepay.DerivedRatePmms15Coef, prepay.DerivedRatePmms30Coef, prepay.DerivedRateSpread));
        mdlIdx++;
    }

// update all start_indices with the current length of their corresponding vector
co_.Hpa1y_start_indices.push_back(co_.Hpa1y_x.size());
co_.CumHpa_start_indices.push_back(co_.CumHpa_x.size());
co_.AgeDecay_start_indices.push_back(co_.AgeDecay_x.size());
co_.Age_start_indices.push_back(co_.Age_x.size());
co_.Fico_start_indices.push_back(co_.Fico_x.size());
co_.Cltv_start_indices.push_back(co_.Cltv_x.size());
co_.Lockin_start_indices.push_back(co_.Lockin_x.size());
co_.Cals_start_indices.push_back(co_.Cals_x.size());
co_.ValuationYear_start_indices.push_back(co_.ValuationYear_x.size());
co_.PeriodMultiplier_start_indices.push_back(co_.PeriodMultiplier_x.size());

ct_.Wam_start_indices.push_back(ct_.Wam_x.size());
ct_.Age_start_indices.push_back(ct_.Age_x.size());
ct_.Fico_start_indices.push_back(ct_.Fico_x.size());
ct_.Cals_start_indices.push_back(ct_.Cals_x.size());
ct_.FactorRatio_start_indices.push_back(ct_.FactorRatio_x.size());
ct_.Cltv_start_indices.push_back(ct_.Cltv_x.size());
ct_.PurchaseAge_start_indices.push_back(ct_.PurchaseAge_x.size());
ct_.PurchaseAgeWacAdj_start_indices.push_back(ct_.PurchaseAgeWacAdj_x.size());
ct_.PeriodMultiplier_start_indices.push_back(ct_.PeriodMultiplier_x.size());

ht_.AgeDecay_start_indices.push_back(ht_.AgeDecay_x.size());
ht_.Hpa2y_start_indices.push_back(ht_.Hpa2y_x.size());
ht_.Lockin_start_indices.push_back(ht_.Lockin_x.size());
ht_.LockinCals_start_indices.push_back(ht_.LockinCals_x.size());
ht_.HighLtvAgeMult_start_indices.push_back(ht_.HighLtvAgeMult_x.size());
ht_.Cltv_start_indices.push_back(ht_.Cltv_x.size());
ht_.LockinDecay_start_indices.push_back(ht_.LockinDecay_x.size());
ht_.Fico_start_indices.push_back(ht_.Fico_x.size());
ht_.Sato_start_indices.push_back(ht_.Sato_x.size());
ht_.Cals_start_indices.push_back(ht_.Cals_x.size());
ht_.LockinDecayWala_start_indices.push_back(ht_.LockinDecayWala_x.size());
ht_.LockinDecayCumLockin_start_indices.push_back(ht_.LockinDecayCumLockin_x.size());
ht_.PeriodMultiplier_start_indices.push_back(ht_.PeriodMultiplier_x.size());
ht_.PurchaseAge_start_indices.push_back(ht_.PurchaseAge_x.size());
ht_.Seasoning_start_indices.push_back(ht_.Seasoning.size());
ht_.Property_start_indices.push_back(ht_.Property.size());

refi_.NontpoAge_start_indices.push_back(refi_.NontpoAge_x.size());
refi_.CorrespondentAge_start_indices.push_back(refi_.CorrespondentAge_x.size());
refi_.BrokerAge_start_indices.push_back(refi_.BrokerAge_x.size());
refi_.Fico_start_indices.push_back(refi_.Fico_x.size());
refi_.Cltv_start_indices.push_back(refi_.Cltv_x.size());
refi_.Hpa3y_start_indices.push_back(refi_.Hpa3y_x.size());
refi_.Turbo_start_indices.push_back(refi_.Turbo_x.size());
refi_.Scurve_start_indices.push_back(refi_.Scurve_x.size());
refi_.Burnout_start_indices.push_back(refi_.Burnout_x.size());
refi_.AgingDecay_start_indices.push_back(refi_.AgingDecay_x.size());
refi_.Sato_start_indices.push_back(refi_.Sato_x.size());
refi_.HighCltvPenalty_start_indices.push_back(refi_.HighCltvPenalty_x.size());
refi_.PeriodMultiplier_start_indices.push_back(refi_.PeriodMultiplier_x.size());
refi_.CalsAge_start_indices.push_back(refi_.CalsAge_x.size());
refi_.VintageAdj_start_indices.push_back(refi_.VintageAdj_x.size());
refi_.Occupancy_start_indices.push_back(refi_.Occupancy.size());
refi_.Property_start_indices.push_back(refi_.Property.size());

elbow_.Als_start_indices.push_back(elbow_.Als_x.size());
elbow_.Fico_start_indices.push_back(elbow_.Fico_x.size());
elbow_.Cltv_start_indices.push_back(elbow_.Cltv_x.size());
elbow_.OccupancyCltv_SecondHome_start_indices.push_back(elbow_.OccupancyCltv_SecondHome_x.size());
elbow_.OccupancyCltv_Investor_OldLlpa_start_indices.push_back(elbow_.OccupancyCltv_Investor_OldLlpa_x.size());
elbow_.OccupancyCltv_Investor_NewLlpa_start_indices.push_back(elbow_.OccupancyCltv_Investor_NewLlpa_x.size());
elbow_.Sato_start_indices.push_back(elbow_.Sato_x.size());
elbow_.FicoWalaCure_start_indices.push_back(elbow_.FicoWalaCure_x.size());
elbow_.FicoCltv_start_indices.push_back(elbow_.FicoCltv_x.size());
elbow_.Occupancy_start_indices.push_back(elbow_.Occupancy.size());
elbow_.Property_start_indices.push_back(elbow_.Property.size());
elbow_.Purpose_start_indices.push_back(elbow_.Purpose.size());

prepay_->CalsAdjMult_start_indices.push_back(prepay_->CalsAdjMult_x.size());

}
} // namespace wfmcm::gpu
// #endif USE_GPU