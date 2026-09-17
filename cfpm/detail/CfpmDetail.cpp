#include <src/core/behavioral/cfpm/detail/CfpmDetail.h>

// note: libstdc++ requires memory_resource for pmr allocator to be complete
#include <memory_resource>
#include <vector>

#include <mortgage/utility/logging/logging_macros.h>
#include <mortgage/utility/types/singleton.h>

#include <src/app-common/InstrumentLogger.h>
#include <src/app-common/util.h>
#include <src/core/behavioral/cfpm/CfpmInstrumentRatePathSession.h>
#include <src/core/behavioral/cfpm/cpu/CfpmInstrumentSession.h>
#include <src/core/behavioral/cfpm/cpu/CfpmRatePathSession.h>
#include <src/core/behavioral/cfpm/cpu/CfpmCalculator.h>
#include <src/core/behavioral/detail/BehavioralModelUtils.h>
#include <src/core/behavioral/detail/MortgageBehavioralModelPerfmonLogger.h>

#include <wfmcm/mortgage_enums.h>

// {FIXME} reduce using namespace later
using namespace wf::mortgage::utility;
using namespace wfmcm::detail;
using namespace app;
using namespace std;

namespace wfmcm::detail {

// note: could use CfpmCalculator::Prepay instead of CfpmCalculator
std::pmr::vector<double> elbowRateIndependent(
    const CfpmCalculator::Prepay& prepay,
    const CfpmInstrumentEx& ex,
    const DiagnosticSettings& diagSettings,
    const CfpmMultiplier& mult)
{

    if (diagSettings.debugOutput_) {
        TLOG_ALWAYS(
            retrieveInstrumentLoggerId(),
            "InstrumentId,AmortMonth,AmortDate,"
            "AmortFico,AmortAls,AmortAdjAls,AmortLtv,AmortWam,AmortWala,AmortCumHpa,"
            "ElbowOrigFico,ElbowState,ElbowPurchase,ElbowMultiFamily,ElbowManufacturedHome,"
            "ElbowSato,ElbowAls,ElbowCltv,ElbowCondoHighCltv,ElbowInvestorCltv,"
            "ElbowSecondHomeCltv,ElbowFicoCltv,ElbowAdj,ElbowRefiAdj,"
            "Dial_ExtraElbowShift,ElbowRateIndependent"
        );
    }
    // {TODO} no idea what this is
    auto amortLength = ex.amortLength();
    std::pmr::vector<double> rateIndependent;
    // {TODO} we can write directly, why reserve?
    rateIndependent.reserve(amortLength);

    auto elbowOrigFico = prepay.ElbowFico(ex.fico());
    auto elbowState = prepay.ElbowState(ex.state());

    auto elbowPurchase =
        ex.purpose().at(PurposeType::Purchase) *
        prepay.ElbowPurpose.at(PurposeType::Purchase);

    auto elbowMultiFamily =
        ex.property().at(PropertyType::MultiFamily) *
        prepay.ElbowProperty.at(PropertyType::MultiFamily);

    auto elbowManufacturedHome =
        ex.property().at(PropertyType::ManufacturedHome) *
        prepay.ElbowProperty.at(PropertyType::ManufacturedHome);

    auto elbowTimeIndependent =
        elbowOrigFico + elbowState + elbowPurchase +
        elbowMultiFamily + elbowManufacturedHome;

    // {TODO} docs where?
    for (size_t i = 0; i < amortLength; ++i) {
        const auto& amortDate = ex.amortDatesSeries()[i];
        auto extraElbowShiftMult = mult.ExtraElbowShift(amortDate);
        auto elbowSato =
            prepay.ElbowSato(ex.sato()) *
            (1.0 - std::min(1.0, (ex.walaSeries()[i] * 1.0) / prepay.ElbowSatoRamp));
        auto elbowAls = prepay.ElbowAls(ex.loanSizeSeries()[i] / 1000.);
        auto elbowCltv = prepay.ElbowCltv(ex.ltvSeries()[i]);
        auto elbowCondoHighCltv = (ex.ltvSeries()[i] > prepay.ElbowHighCltvCut) ?
            prepay.ElbowPropertyAdj_Condo_HighCltv *
                ex.property().at(PropertyType::Condo) :
            0.;
        auto elbowInvestorCltv = (amortDate >= prepay.ElbowLlpaCut) ?
            ex.occupancy().at(OccupancyType::Investor) *
                prepay.ElbowOccupancyCltv_Investor_NewLlpa(ex.ltvSeries()[i]) :
            ex.occupancy().at(OccupancyType::Investor) *
                prepay.ElbowOccupancyCltv_Investor_OldLlpa(ex.ltvSeries()[i]);
        auto elbowSecondHomeCltv = (amortDate >= prepay.ElbowSecondHomeCltvCut) ?
            ex.occupancy().at(OccupancyType::SecondHome) *
                prepay.ElbowOccupancyCltv_SecondHome(ex.ltvSeries()[i]) :
            0.;
        auto elbowFicoCltv = prepay.ElbowFicoCltv(ex.ficoSeries()[i], ex.ltvSeries()[i]);
        auto elbowAdj =
            (
                amortDate >= prepay.ElbowAdjBegin &&
                amortDate <= prepay.ElbowAdjEnd
            ) ?
                prepay.ElbowAdj : 0.;
        auto elbowRefiAdj =
            (
                amortDate >= prepay.ElbowRefinanceAdjBegin &&
                amortDate <= prepay.ElbowRefinanceAdjEnd
            ) ?
                prepay.ElbowRefinanceAdj : 0.;
        rateIndependent.emplace_back(
            elbowTimeIndependent + elbowSato + elbowAls + elbowCltv +
            elbowCondoHighCltv + elbowInvestorCltv + elbowSecondHomeCltv +
            elbowFicoCltv + elbowAdj + elbowRefiAdj + extraElbowShiftMult
        );

        if (diagSettings.debugOutput_) {
            TLOG_ALWAYS(
                retrieveInstrumentLoggerId(),
                logging::comma_format,
                ex.instrumentId(),
                i,
                std::to_string(amortDate),
                ex.ficoSeries()[i],
                ex.loanSizeSeries()[i],
                ex.adjLoanSizeSeries()[i],
                ex.ltvSeries()[i],
                ex.wamSeries()[i],
                ex.walaSeries()[i],
                ex.cumHpaSeries()[i],
                elbowOrigFico, elbowState, elbowPurchase, elbowMultiFamily,
                elbowManufacturedHome, elbowSato, elbowAls, elbowCltv,
                elbowCondoHighCltv, elbowInvestorCltv, elbowSecondHomeCltv,
                elbowFicoCltv, elbowAdj, elbowRefiAdj, extraElbowShiftMult,
                rateIndependent[i]
            );
        }
    }
    return rateIndependent;
}

// note: only CfpmCalculator::Prepay::Refinance is required
std::pmr::vector<double> refinanceRateIndependent(
    const CfpmCalculator::Prepay::Refinance& rf,
    const CfpmInstrumentEx& ex,
    const DiagnosticSettings& diagSettings,
    const CfpmMultiplier& mult)
{
    if (diagSettings.debugOutput_) {
        TLOG_ALWAYS(
            retrieveInstrumentLoggerId(),
            "InstrumentId,ProjMonth,ProjDate,ProjSeasoning,"
            "ProjFico,ProjAls,ProjAdjAls,ProjFactorRatio,ProjLtv,ProjWam,"
            "ProjWala,ProjCumHpa,ProjHpa1y,ProjHpa2y,ProjHpa3y,ProjHpa5y,"
            "RfCoreSato,RfVintageAdj,RfCoreFico,RfCoreOccupancy,RfCorePiw,"
            "RfCoreProperty,RfCoreState,RfCoreServicerSpeed,RfCoreIntercept,"
            "RfCoreHpa3y,RfCoreCltv,RfCoreAgingDecy,RfCoreTimeAdj,RfCoreAlsAge,"
            "RfCoreChannel,RfCoreHighCltvPenalty,RfCorePeriodMultiplier,"
            "Dial_RfRampingMultiplier, RfRateIndependent"
        );
    }

    size_t projLength = ex.projectionLength();
    std::pmr::vector<double> rateIndependent;
    // note: consider just constructing to the correct size
    rateIndependent.reserve(projLength);

    auto rfSato = rf.Sato(ex.sato());
    auto rfVintageAdj = rf.VintageAdj(ex.origDate().year()); //does not apply to rf directly
    auto rfFico = rf.Fico(ex.fico());
    auto rfOccupancy =
        rf.Occupancy.at(OccupancyType::Investor) *
        ex.occupancy().at(OccupancyType::Investor) +
        // {FIXME} 1 * n is equal to n
        1 * (1 - ex.occupancy().at(OccupancyType::Investor));
    auto rfPiw = rf.PropertyInspectionWaiver * ex.piw() + 1 * (1 - ex.piw());
    auto rfProperty =
        rf.Property.at(PropertyType::MultiFamily) *
        ex.property().at(PropertyType::MultiFamily) +
        // {FIXME} 1 * n is equal to n
        1 * (1 - ex.property().at(PropertyType::MultiFamily));
    auto rfState = rf.State(ex.state());
    auto rfServicerSpeed =
        rf.ServicerFast * ex.rfSpeed().at(ServicerSpeedType::Fast) +
        rf.ServicerSlow * ex.rfSpeed().at(ServicerSpeedType::Slow) +
        // {FIXME} if we do not do normal = 1- sum, we should use (1-fast - slow)
        1 * ex.rfSpeed().at(ServicerSpeedType::Normal);
    auto rfIntercept = rf.Intercept;
    auto rfTimeIndependent =
        rfSato * rfFico * rfOccupancy * rfPiw * rfProperty * rfState *
        rfServicerSpeed * rfIntercept;

    for (size_t i = 0; i < projLength; ++i) {
        const auto& projDate = ex.projDate(i);
        auto rfHpa3y = rf.Hpa3y(ex.projHpa3y(i));
        auto rfCltv = rf.Cltv(ex.projLtv(i));
        auto rfAgingDecy = rf.AgingDecay(ex.projWala(i));
        auto rfTimeAdj = projDate >= rf.ValuationDateAdjCut ? 1 : rfVintageAdj;
        auto rfAlsAge = rf.CalsAge(ex.projAdjLoanSize(i), ex.projWala(i));
        auto rfChannel =
            rf.BrokerAge(ex.projRfTunedWala(i)) *
            ex.channel().at(OriginationChannel::Broker) +
            rf.CorrespondentAge(ex.projRfTunedWala(i)) *
            ex.channel().at(OriginationChannel::Correspondent) +
            rf.NontpoAge(ex.projRfTunedWala(i)) * (1 - ex.chancelTPO());
        auto rfHighCltvPenalty =
            (
                rf.HighCltvPenaltyEnd >= projDate &&
                projDate >= rf.HighCltvPenaltyBegin
            ) ?
                1. + rf.HighCltvPenalty(ex.projLtv(i)) : 1.;
        auto rfPeriodMultiplier = rf.PeriodMultiplier(projDate);

        rateIndependent.emplace_back(
            rfTimeIndependent * rfHpa3y * rfCltv * rfAgingDecy * rfTimeAdj *
            rfAlsAge * rfChannel * rfHighCltvPenalty * rfPeriodMultiplier
        );

        if (diagSettings.debugOutput_) {
            TLOG_ALWAYS(
                retrieveInstrumentLoggerId(),
                logging::comma_format,
                ex.instrumentId(), i, std::to_string(projDate),
                std::to_string(ex.season(i)), ex.projFico(i),
                ex.projLoanSize(i), ex.projAdjLoanSize(i), ex.projFactorRatio(i),
                ex.projLtv(i), ex.projWam(i), ex.projWala(i), ex.projCumHpa(i),
                ex.projHpa1y(i), ex.projHpa2y(i), ex.projHpa3y(i), ex.projHpa5y(i),
                rfSato, rfVintageAdj, rfFico, rfOccupancy, rfPiw, rfProperty,
                rfState, rfServicerSpeed, rfIntercept, rfHpa3y, rfCltv,
                rfAgingDecy, rfTimeAdj, rfAlsAge, rfChannel, rfHighCltvPenalty,
                rfPeriodMultiplier, mult.RefinanceRamping(projDate), rateIndependent[i]
            );
        }
    }
    return rateIndependent;
}

std::pmr::vector<double> refinanceRateDependent(
    const CfpmCalculator::Prepay::Refinance& rf,
    const CfpmInstrumentEx& ex,
    const CfpmInstrumentRatePathSession& pathDependent,
    const IRatePathManager& turbo,
    size_t pathIndex,   // index of the projection path
    size_t proj_index,  // index of first projected rate in projection path
    const DiagnosticSettings& diagSettings,
    const CfpmMultiplier& mult)
{
    // {TODO} can reduce preconditions by using just the index_map interface
    const auto& rates = turbo.imanager<PrimaryRateType>();

    if (diagSettings.debugOutput_)
        TLOG_ALWAYS(
            retrieveInstrumentLoggerId(),
            "InstrumentId,ProjMonth,ProjDate,"
            "TbLag1,TbLag2,RfTunedTbLag1,RfTunedTbLag2,RfCoreScurve,"
            "RfCoreBurnout,Dial_RfMediaEffectMultiplier,RfSmm"
        );

    size_t projLength = ex.projectionLength();
    std::pmr::vector<double> smm;
    smm.reserve(projLength);
    auto rateView = rates.get(ex.primaryRate());
    // {TODO} should we check that projection length won't exceed last projected?

    for (size_t i = 0; i < projLength; ++i) {
        const auto& projDate = ex.projDate(i);
        auto refinanceMediaEffectMult = mult.RefinanceMediaEffect(projDate);
        // index by lagging from the projection rates (proj_index offset to first)
        auto tbLag1 = rateView(pathIndex, proj_index + i - 1);
        auto tbLag2 = rateView(pathIndex, proj_index + i - 2);
        // {TODO} document more?
        auto rfTunedTbLag1 = 1. + (rf.Turbo(tbLag1) - 1.) * refinanceMediaEffectMult;
        auto rfTunedTbLag2 = 1. + (rf.Turbo(tbLag2) - 1.) * refinanceMediaEffectMult;
        auto rfScurve = (
            rf.Lag1Weight * rfTunedTbLag1 *
            wfmcm::math::exp(rf.Scurve(pathDependent.eitLag1(i))) +
            (1 - rf.Lag1Weight) * rfTunedTbLag2 *
            wfmcm::math::exp(rf.Scurve(pathDependent.eitLag2(i)))
        );
        auto rfBurnout = rf.Burnout(pathDependent.burnout(i));
        smm.emplace_back(rfScurve * rfBurnout * ex.rfRateIndependent()[i]);

        if (diagSettings.debugOutput_)
            TLOG_ALWAYS(
                retrieveInstrumentLoggerId(),
                logging::comma_format,
                ex.instrumentId(),
                i,
                // note: C++20 provides the year_month overload for to_string
                std::to_string(projDate),
                tbLag1,
                tbLag2,
                rfTunedTbLag1,
                rfTunedTbLag2,
                rfScurve,
                rfBurnout,
                refinanceMediaEffectMult,
                smm[i]
            );
    }
    return smm;
}

std::pmr::vector<double> turnoverRateIndependent(
    const CfpmCalculator::Prepay::Turnover& ht,
    const CfpmInstrumentEx& ex,
    const DiagnosticSettings& diagSettings,
    const CfpmMultiplier& mult)
{
    if (diagSettings.debugOutput_) {
        TLOG_ALWAYS(retrieveInstrumentLoggerId(),
            "InstrumentId,ProjMonth,ProjDate,"
            "HtCoreSato,HtCoreFico,HtCoreProperty,HtCoreIntercept,HtCoreState,HtCoreServicerSpeed,"
            "HtCoreHpa2y,HtCoreSeason,HtCoreCltv,HtCoreCals,HtCoreLockDecay,HtCoreAgeDecay,HtCorePurAge,HtCoreHighLtv,HtCorePeriodMultiplier,"
            "Dial_HtRampingMultiplier,HtRateIndependent"
        );
    }

    size_t projLength = ex.projectionLength();
    std::pmr::vector<double> rateIndependent; rateIndependent.reserve(projLength);

    auto htSato = ht.Sato(ex.sato());
    auto htFico = ht.Fico(ex.fico());
    auto htProperty = ex.property().at(PropertyType::MultiFamily) * ht.Property.at(PropertyType::MultiFamily) +
        ex.property().at(PropertyType::SingleFamily) * 1.0; //{FIXME} 1 - %MultiFamily = %Single?
    auto htIntercept = wfmcm::math::exp(ht.Intercept);
    auto htState = ht.State(ex.state());
    auto htServicerSpeed = ht.ServicerFast * ex.htSpeed().at(ServicerSpeedType::Fast) +
        ht.ServicerSlow * ex.htSpeed().at(ServicerSpeedType::Slow) +
        1 * ex.htSpeed().at(ServicerSpeedType::Normal); //{FIXME} if we do not do normal = 1- sum, we should use (1-fast - slow)

    auto htTimeIndependent = htSato * htFico * htProperty * htIntercept * htState * htServicerSpeed;

    for (size_t i = 0; i < projLength; ++i) {
        const auto& projDate = ex.projDate(i);
        auto htHpa2y = ht.Hpa2y(ex.projHpa2y(i));
        auto htSeason = ht.Seasoning.at(ex.season(i));
        auto htCltv = ht.Cltv(ex.projLtv(i));
        auto htCals = ht.Cals(ex.projAdjLoanSize(i));
        auto htLockDecay = ht.LockinDecay(ex.projWala(i));
        auto htAgeDecay = ht.AgeDecay(ex.projWala(i));
        auto htPurAge = ht.PurchaseAge(ex.purpose().at(PurposeType::Purchase), ex.projHtTunedWala(i));
        //one is >=, the other one is >. keep the same as Python
        auto htHighLtv = (ex.oltv() >= ht.HighOltvCut && ex.projLtv(i) > ht.HighCltvCut) ? (1. + ht.HighLtvAgeMult(ex.projWala(i))) : 1.;
        auto htPeriodMultiplier = ht.PeriodMultiplier(projDate);

        rateIndependent.emplace_back(htTimeIndependent * htHpa2y * htSeason * htCltv *
            htCals * htLockDecay * htAgeDecay * htPurAge * htHighLtv * htPeriodMultiplier);

        if (diagSettings.debugOutput_) {
            TLOG_ALWAYS(retrieveInstrumentLoggerId(), logging::comma_format,
                ex.instrumentId(), i, std::to_string(projDate),
                htSato, htFico, htProperty, htIntercept, htState, htServicerSpeed,
                htHpa2y, htSeason, htCltv, htCals, htLockDecay, htAgeDecay, htPurAge, htHighLtv, htPeriodMultiplier,
                mult.TurnoverRamping(projDate),rateIndependent[i]
            );
        }

    }
    return rateIndependent;
}

std::pmr::vector<double> turnoverRateDependent(
    const CfpmCalculator::Prepay::Turnover& ht,
    const CfpmInstrumentEx& ex,
    const CfpmInstrumentRatePathSession& pathDependent,
    size_t pathIndex,
    const DiagnosticSettings& diagSettings,
    const CfpmMultiplier& mult)
{
    if (diagSettings.debugOutput_) {
        TLOG_ALWAYS(retrieveInstrumentLoggerId(),
            "InstrumentId,ProjMonth,ProjDate,"
            "HtLockin,HtLockinDecayWala,HtLockinDecayCumLockin,HtLockinDecay,HtAdjLockin,"
            "Dial_HtLockinEffectMultiplier,HtCoreTunedLockin,HtSmm"
        );
    }

    //per modeler's comment, ht cb is disabled
    size_t projLength = ex.projectionLength();
    std::pmr::vector<double> smm; smm.reserve(projLength);

    for (size_t i = 0; i < projLength; ++i) {
        const auto& projDate = ex.projDate(i);
        auto turnoverLockinEffectMult = mult.TurnoverLockinEffect(projDate);

        double htLockinDecayWala = ht.LockinDecayWala(ex.projWala(i));
        double htLockinDecayCumLockin = ht.LockinDecayCumLockin(pathDependent.cumLockin(i));
        double htLockinDecay = std::min(htLockinDecayWala, htLockinDecayCumLockin);

        double eitFlag = pathDependent.comBinedEit(i) < 0. ? 1. : 0.;
        auto htLockin = wfmcm::math::exp(ht.Lockin(pathDependent.comBinedEit(i)) * wfmcm::math::exp(eitFlag * ht.LockinCals(ex.projAdjLoanSize(i))));
        auto htAdjLockin = htLockin + (1. - htLockin) * (1 - htLockinDecay) * eitFlag;
        auto htTunedLockin = htAdjLockin >= 1. ? htAdjLockin :
            std::min(1., std::max(0., (1. + (htAdjLockin - 1.) * turnoverLockinEffectMult)));
        smm.emplace_back(htTunedLockin * ex.htRateIndependent()[i]);
        if (diagSettings.debugOutput_) {
            TLOG_ALWAYS(retrieveInstrumentLoggerId(), logging::comma_format,
                ex.instrumentId(), i, std::to_string(projDate),
                htLockin, htLockinDecayWala, htLockinDecayCumLockin, htLockinDecay, htAdjLockin,
                turnoverLockinEffectMult, htTunedLockin, smm[i]
            );
        }
    }
    return smm;
}

std::pmr::vector<double> cashoutRateIndependent(
    const CfpmCalculator::Prepay::Cashout& co,
    const CfpmInstrumentEx& ex,
    const DiagnosticSettings& diagSettings,
    const CfpmMultiplier& mult)
{
    if (diagSettings.debugOutput_) {
        TLOG_ALWAYS(retrieveInstrumentLoggerId(),
            "InstrumentId,ProjMonth,ProjDate,"
            "CoCoreFico,CoCoreIntercept,CoCoreServicerSpeed,"
            "CoCoreAge,CoCoreCltv,CoCoreCumHpa,CoCoreCals,CoCoreValuationYear,"
            "CoCoreHpa1y,CoCorePeriodMultiplier,Dial_CoRampingMultiplier,CoRateIndependent"
        );
    }

    size_t projLength = ex.projectionLength();
    std::pmr::vector<double> rateIndependent; rateIndependent.reserve(projLength);

    auto coFico = co.Fico(ex.fico());
    auto coIntercept = wfmcm::math::exp(co.Intercept);
    auto coServicerSpeed = co.ServicerFast * ex.htSpeed().at(ServicerSpeedType::Fast) +
        co.ServicerSlow * ex.htSpeed().at(ServicerSpeedType::Slow) +
        1 * ex.htSpeed().at(ServicerSpeedType::Normal); //{FIXME} if we do not do normal = 1- sum, we should use (1-fast - slow)
    auto coTimeIndependent = coFico * coIntercept * coServicerSpeed;

    for (size_t i = 0; i < projLength; ++i) {
        const auto& projDate = ex.projDate(i);
        auto coAge = co.Age(ex.projCoTunedWala(i));
        auto coCltv = co.Cltv(ex.projLtv(i));
        auto coCumHpa = co.CumHpa(ex.projCumHpa(i));
        auto coCals = co.Cals(ex.projAdjLoanSize(i));
        auto coValuationYear = co.ValuationYear(projDate.year());
        auto coHpa1y = wfmcm::math::exp(co.Hpa1y(ex.projHpa1y(i)));
        auto coPeriodMultiplier = co.PeriodMultiplier(projDate);

        rateIndependent.emplace_back(coTimeIndependent * coAge * coCltv * coCumHpa * coCals *
            coValuationYear * coHpa1y * coPeriodMultiplier);

        if (diagSettings.debugOutput_) {
            TLOG_ALWAYS(retrieveInstrumentLoggerId(), logging::comma_format,
                ex.instrumentId(), i, std::to_string(projDate),
                coFico, coIntercept, coServicerSpeed,
                coAge, coCltv, coCumHpa, coCals, coValuationYear, coHpa1y, coPeriodMultiplier,
                mult.CashoutRamping(projDate), rateIndependent[i]
            );
        }
    }
    return rateIndependent;
}

std::pmr::vector<double> cashoutRateDependent(
    const CfpmCalculator::Prepay& prepay,
    const CfpmInstrumentEx& ex,
    const CfpmInstrumentRatePathSession& pathDependent,
    size_t pathIndex,
    const DiagnosticSettings& diagSettings,
    const CfpmMultiplier& mult)
{
    if (diagSettings.debugOutput_) {
        TLOG_ALWAYS(retrieveInstrumentLoggerId(),
            "InstrumentId,ProjMonth,ProjDate,"
            "CoCoreLockin,CoCoreBurnout,CoSmm"
        );
    }

    size_t projLength = ex.projectionLength();
    const auto& co = prepay.Cashout_;
    std::pmr::vector<double> smm; smm.reserve(projLength);

    for (size_t i = 0; i < projLength; ++i) {
        const auto& projDate = ex.projDate(i);
        auto coLockin = wfmcm::math::exp(co.Lockin(pathDependent.comBinedEit(i)) * co.LockinAlpha);
        // co shares the same burnout formula with rf
        auto coBurnout = prepay.Refinance_.Burnout(pathDependent.burnout(i));
        smm.emplace_back(coBurnout * coLockin * ex.coRateIndependent()[i]);
        if (diagSettings.debugOutput_) {
            TLOG_ALWAYS(retrieveInstrumentLoggerId(), logging::comma_format,
                ex.instrumentId(), i, std::to_string(projDate),
                coLockin, coBurnout, smm[i]
            );
        }
    }
    return smm;
}

std::pmr::vector<double> curtailmentRateIndependent(
    const CfpmCalculator::Prepay::Curtailment& ct,
    const CfpmInstrumentEx& ex,
    const DiagnosticSettings& diagSettings,
    const CfpmMultiplier& mult)
{
    if (diagSettings.debugOutput_) {
        TLOG_ALWAYS(retrieveInstrumentLoggerId(),
            "InstrumentId,ProjMonth,ProjDate,"
            "CtCoreFico,CtCoreState,CtCoreIntercept,"
            "CtCoreWam,CtCoreAge,CtCoreCals,CtCoreValuationDate,CtCorePurpose,"
            "CtCoreFactorRatio,CtCoreCltv,CtCorePeriodMultiplier,"
            "CtRateIndependent,CtSmm"
        );
    }

    size_t projLength = ex.projectionLength();
    std::pmr::vector<double> rateIndependent; rateIndependent.reserve(projLength);

    auto ctFico = ct.Fico(ex.fico());
    auto ctState = ct.State(ex.state());
    auto ctTimeIndependent = ctFico * ctState * ct.Intercept;

    for (size_t i = 0; i < projLength; ++i) {
        const auto& projDate = ex.projDate(i);
        auto ctWam = ct.Wam(ex.projWam(i));
        auto ctAge = ct.Age(ex.projWala(i));
        auto ctCals = ct.Cals(ex.projAdjLoanSize(i));
        auto ctValuationDate = ct.ValuationDateMultiplier(projDate);
        auto ctPurpose = ex.purpose().at(PurposeType::Purchase) *
            (1. + (ct.PurchaseAge(ex.projWala(i)) - 1.) * ct.PurchaseAgeWacAdj(ex.wac())) +
            (1 - ex.purpose().at(PurposeType::Purchase)) * 1.0;
        auto ctFactorRatio = ct.FactorRatio(ex.projFactorRatio(i));
        auto ctCltv = ct.Cltv(ex.projLtv(i));
        auto ctPeriodMultiplier = ct.PeriodMultiplier(projDate);

        rateIndependent.emplace_back(ctTimeIndependent * ctWam * ctAge * ctCals * ctValuationDate * ctPurpose *
            ctFactorRatio * ctCltv * ctPeriodMultiplier);
        if (diagSettings.debugOutput_) {
            TLOG_ALWAYS(retrieveInstrumentLoggerId(), logging::comma_format,
                ex.instrumentId(), i, std::to_string(projDate),
                ctFico, ctState, ct.Intercept,
                ctWam, ctAge, ctCals, ctValuationDate, ctPurpose, ctFactorRatio, ctCltv, ctPeriodMultiplier,
                rateIndependent[i], rateIndependent[i]
            );
        }

    }
    return rateIndependent;
}

CfpmInstrumentRatePathSession amortize(
    const CfpmCalculator::Prepay& prepay,
    const CfpmInstrumentEx& ex,
    const IRatePathManager& rates,
    size_t pathIndex,
    size_t proj_index,
    const DiagnosticSettings& diagSettings,
    const CfpmMultiplier& mult)
{
    if (diagSettings.debugOutput_) {
        TLOG_ALWAYS(retrieveInstrumentLoggerId(),
            "InstrumentId,AmortMonth,AmortDate,"
            "Lag1PrimaryRate,Lag1BlendRate,WamCorrection,"
            "Dial_BurnoutCumulativeSpeedMultiplier,Dial_HtBurnoutCumulativeSpeedMultiplier,"
            "EitCombined,Eit1,Eit2,Burnout,CumLockin"
        );
    }

    const auto amortLength = ex.amortLength();
    const auto projLength = ex.projectionLength();

    // [wala - 1, wala + project - 1]
    std::pmr::vector<double> eitLag2;
    eitLag2.reserve(projLength + 1);
    // [wala, wala + project - 1]
    std::pmr::vector<double> eitCombined;
    eitCombined.reserve(projLength);
    // [wala, wala + project - 1]
    std::pmr::vector<double> burnoutData;
    burnoutData.reserve(projLength);
    // [wala, wala + project - 1]
    std::pmr::vector<double> cumulativeLockinData;
    cumulativeLockinData.reserve(projLength);

    const auto& primaryRates = rates.imanager<PrimaryRateType>();

    const size_t age = size_t(ex.wala());
    double eit1 = 0.;
    double eit2 = 0.;
    double burnout = 0.;
    double preEit = 0.;
    // need wala to offset the projection index
    auto wala = ex.wala();
    auto rateView = primaryRates.get(ex.primaryRate());
    double cumulativeLockin = 0.;

    // Cache WAC discount factor (constant across loop)
    const auto wacDiscFactor = ex.wacDiscFactor();

    // Configure EIT for CFPM (present value, decimal scale, with WAM correction)
    const EitConfig eitConfig {
        .type = EitCalculationType::PresentValue,
        .scaleFactor = 1.0,
        .wamCorrection = prepay.WamCorrection
    };

    // Configure burnout for CFPM
    const BurnoutConfig burnoutConfig {
        .floor = prepay.Refinance_.EitThreshold,  // CFPM uses threshold as floor
        .ceiling = prepay.Refinance_.EitCeiling
    };

    // compute
    for (size_t i = 0; i < amortLength; ++i) {
        auto amortDate = ex.amortDatesSeries()[i];
        auto burnoutCumulativeSpeedMult = mult.BurnoutCumulativeSpeed(amortDate);
        auto turnoverBurnoutCumulativeSpeedMult = mult.TurnoverBurnoutCumulativeSpeed(amortDate);

        // note: projection indices must be offset by -WALA to index similarly
        // to amortDateSeries() dates (as projection date is amort date + WALA)
        auto lag1Rate = rateView(pathIndex, proj_index - wala + i - 1);
        auto blendedRate = lag1Rate + ex.elbowRateIndependent()[i];

        // Apply blended rate floor/ceiling using standardized clamp logic
        // [0, BlendRateCut) -> BlendRateCut
        // (-BlendRateCut, 0) -> -BlendRateCut
        if (blendedRate < prepay.BlendRateCut && blendedRate >= 0)
            blendedRate = prepay.BlendRateCut;
        else if (-prepay.BlendRateCut < blendedRate && blendedRate < 0)
            blendedRate = -prepay.BlendRateCut;

        // Calculate EIT using present-value formula (standardized functions)
        const EitInputs eitInputs {
            .wac = ex.wac(),
            .wam = ex.wamSeries()[i],
            .blendedRate = blendedRate,
            .wacDiscFactor = wacDiscFactor,
            .rateDiscFactor = calculateDiscFactor(blendedRate)
        };
        eit1 = computeEitRaw(eitInputs, eitConfig);
        
        // CFPM uses weighted average of current and lagged EIT
        double eit = 0.5 * eit1 + 0.5 * eit2;

        // Accumulate burnout and cumLockin using standardized functions
        if (i > 0) {
            // Burnout accumulation using standardized function
            burnout = accumulateBurnout(
                burnoutConfig,
                burnout,
                preEit,
                ex.burnoutCltvMult()[i - 1],
                burnoutCumulativeSpeedMult
            );

            // Calculate lockin accumulation using standardized function
            cumulativeLockin = accumulateLockin(
                cumulativeLockin,
                preEit,
                prepay.Refinance_.EitThreshold,
                prepay.Turnover_.EitCeiling,
                turnoverBurnoutCumulativeSpeedMult
            );
        }

        if (i >= age) {
            eitLag2.emplace_back(eit2);
            eitCombined.emplace_back(eit);
            burnoutData.emplace_back(burnout);
            cumulativeLockinData.emplace_back(cumulativeLockin);
        }

        if (diagSettings.debugOutput_)
            TLOG_ALWAYS(
                retrieveInstrumentLoggerId(),
                logging::comma_format,
                ex.instrumentId(), i, std::to_string(amortDate),
                lag1Rate, blendedRate,
                prepay.WamCorrection, burnoutCumulativeSpeedMult,
                turnoverBurnoutCumulativeSpeedMult,
                eit, eit1, eit2, burnout, cumulativeLockin
            );

        eit2 = eit1;
        preEit = eit;
    }

    // eitData will be used to lookup both eit1 and eit2
    eitLag2.emplace_back(eit1);

    // note: not really a "session"; it just holds output
    return CfpmInstrumentRatePathSessionBuilder()
        .withEitLag2(std::move(eitLag2))
        .withCombinedEit(std::move(eitCombined))
        .withBurnout(std::move(burnoutData))
        .withCumLockin(std::move(cumulativeLockinData))
        .build();
}

MortgageBehavioralModelOutput forecast(
    const CfpmCalculator::Prepay& prepay,
    const CfpmInstrumentEx& ex,
    const CfpmInstrumentRatePathSession& pathDependent,
    std::span<const double> rfSmm,
    std::span<const double> htSmm,
    std::span<const double> coSmm,
    std::span<const double> ctSmm,
    const wfmutil::time_series<double>& businessDaysAdjRatio,
    const DiagnosticSettings& diagSettings,
    const CfpmMultiplier& mult)
{
    if (diagSettings.debugOutput_) {
        TLOG_ALWAYS(retrieveInstrumentLoggerId(),
            "InstrumentId,ProjMonth,ProjDate,"
            "FinalSmm,BusinessDayAdj,"
            "RfModelAdj,HtModelAdj,CoModelAdj,CtModelAdj,"
            "Dial_TotalSmmMultiplier,Dial_RfSmmMultiplier,Dial_HtSmmMultiplier,Dial_CoSmmMultiplier,Dial_CtSmmMultiplier,"
            "Dial_TotalSmmAssetMultiplier,Dial_RfSmmAssetMultiplier,Dial_HtSmmAssetMultiplier,Dial_CoSmmAssetMultiplier,Dial_CtSmmAssetMultiplier,"
            "KnobTotalMultiplier,KnobRfMultiplier,KnobHtMultiplier,"
            "FinalRfSmm,FinalHtSmm,FinalCoSmm,FinalCtSmm"
        );
    }

    size_t projLength = ex.projectionLength();
    const auto& assetMult = ex.assetMultiplier();
    std::map<MortgageBehavioralModelOutput::Key, pmr::vector<double>> output;
    output[MortgageBehavioralModelOutput::Key::Prepay].reserve(projLength);

    auto lastDate = businessDaysAdjRatio.endDate();
    auto lastDateAdj = *businessDaysAdjRatio.at(lastDate);
    for (size_t i = 0; i < projLength; ++i) {
        auto projDate = ex.projDate(i);
        
        //global tuning
        auto totalSmmMult = mult.TotalSmm(projDate);
        auto refinanceSmmMult = mult.RefinanceSmm(projDate);
        auto turnoverSmmMult = mult.TurnoverSmm(projDate);
        auto cashoutSmmMult = mult.CashoutSmm(projDate);
        auto curtailmentSmmMult = mult.CurtailmentSmm(projDate);
        
        //asset level tuning
        auto totalSmmAssetMult = assetMult.TotalSmm(i);
        auto refinanceSmmAssetMult = assetMult.RefinanceSmm(i);
        auto turnoverSmmAssetMult = assetMult.TurnoverSmm(i);
        auto cashoutSmmAssetMult = assetMult.CashoutSmm(i);
        auto curtailmentSmmAssetMult = assetMult.CurtailmentSmm(i);

        auto businessDaysAdj = (projDate > lastDate) ? lastDateAdj : *businessDaysAdjRatio.at(projDate);

        auto rfAdjSmm = rfSmm[i] * businessDaysAdj * prepay.Refinance_.ModelMultiplier * mult.KnobRefinance * refinanceSmmMult * refinanceSmmAssetMult;
        auto htAdjSmm = htSmm[i] * businessDaysAdj * prepay.Turnover_.ModelMultiplier * mult.KnobTurnover * turnoverSmmMult * turnoverSmmAssetMult;
        auto coAdjSmm = coSmm[i] * businessDaysAdj * prepay.Cashout_.ModelMultiplier * cashoutSmmMult * cashoutSmmAssetMult;
        auto ctAdjSmm = ctSmm[i] * businessDaysAdj * prepay.Curtailment_.ModelMultiplier * curtailmentSmmMult * curtailmentSmmAssetMult;

        auto finalSmm = std::min(std::max(ResultFloor,
            (rfAdjSmm + htAdjSmm + coAdjSmm + ctAdjSmm) * mult.KnobPrepay * totalSmmMult * totalSmmAssetMult), ResultCap);
        auto finalCtSmm = ctAdjSmm * mult.KnobPrepay * totalSmmMult * totalSmmAssetMult;

        if (diagSettings.debugOutput_) {
            TLOG_ALWAYS(retrieveInstrumentLoggerId(), logging::comma_format,
                ex.instrumentId(), i, std::to_string(projDate),
                finalSmm, businessDaysAdj,
                prepay.Refinance_.ModelMultiplier, prepay.Turnover_.ModelMultiplier,
                prepay.Cashout_.ModelMultiplier, prepay.Curtailment_.ModelMultiplier,
                totalSmmMult, refinanceSmmMult, turnoverSmmMult, cashoutSmmMult, curtailmentSmmMult,
                totalSmmAssetMult, refinanceSmmAssetMult, turnoverSmmAssetMult, cashoutSmmAssetMult, curtailmentSmmAssetMult,
                mult.KnobPrepay, mult.KnobRefinance, mult.KnobTurnover,
                rfAdjSmm * mult.KnobPrepay * totalSmmMult * totalSmmAssetMult, htAdjSmm * mult.KnobPrepay * totalSmmMult * totalSmmAssetMult,
                coAdjSmm * mult.KnobPrepay * totalSmmMult * totalSmmAssetMult, ctAdjSmm * mult.KnobPrepay * totalSmmMult * totalSmmAssetMult
            );
        }

        if (diagSettings.aggregateOutput_) {
            LOG_ALWAYS(singleton<MortgageBehavioralModelPerfmonLogger>::instance()->logger_,
                logging::comma_format,
                ex.instrumentId(), std::to_string(projDate),
                "",
                finalSmm, "", "",
                "", "", "", "",
                "",
                ex.sato(), pathDependent.comBinedEit(i),ex.projLtv(i)/100.,"",
                to_string(ex.modelType()), to_string(ex.subModelType()),
                "", "", "",
                "", "", "",
                "", "", "", "", "", "", "", "", "", "",
                "", "", "", "", "",
                "", "", "", "",
                "", "",
                "", "",
                "", "",
                "", "", "", "",
                "", "", "", "", "",
                "", "", "", "", "", "", "", "",
                "", "", "", "", "", "", "", "",
                "", "",
                "", "", "",
                "", "", "", "",
                "", "", "", "", "",
                "", "", "", "", "", "",
                "", "", "",
                "", "", "", "", "", "", "", "",
                "",
                "", "", "", "", "", "",
                "", "", ""
            );
        }

        output[MortgageBehavioralModelOutput::Key::Prepay].emplace_back(finalSmm);
        output[MortgageBehavioralModelOutput::Key::Curtail].emplace_back(finalCtSmm);
    }
    return MortgageBehavioralModelOutput(ex.instrumentId(), output);
}

CfpmMultiplier& convert(polyvar&& from, CfpmMultiplier* to)
{
    assert(to);
    assert(from.is_object());

    // Helper: build a MultiplierCurve from the polyvar in one shot
    auto buildCurve = [&](std::string_view multName, std::string_view startDateName, double dv) {
        double startMonth = 0.0;
        auto data = buildMultiplierVectors(from, multName, startDateName, dv, &startMonth);
        return MultiplierCurve(std::move(data), startMonth, dv);
    };

    to->TotalSmm = buildCurve("TotalSmmMultiplier", "TotalSmmMultiplierStartDate", 1.);
    to->RefinanceSmm = buildCurve("RfSmmMultiplier", "RfSmmMultiplierStartDate", 1.);
    to->TurnoverSmm = buildCurve("HtSmmMultiplier", "HtSmmMultiplierStartDate", 1.);
    to->CashoutSmm = buildCurve("CoSmmMultiplier", "CoSmmMultiplierStartDate", 1.);
    to->CurtailmentSmm = buildCurve("CtSmmMultiplier", "CtSmmMultiplierStartDate", 1.);
    to->RefinanceRamping = buildCurve("RfRampingMultiplier", "RfRampingMultiplierStartDate", 1.);
    to->TurnoverRamping = buildCurve("HtRampingMultiplier", "HtRampingMultiplierStartDate", 1.);
    to->CashoutRamping = buildCurve("CoRampingMultiplier", "CoRampingMultiplierStartDate", 1.);
    to->ExtraElbowShift = buildCurve("ExtraElbowShift", "ExtraElbowShiftStartDate", 0.);
    to->BurnoutCumulativeSpeed = buildCurve("BurnoutCumulativeSpeedMultiplier", "BurnoutCumulativeSpeedMultiplierStartDate", 1.);
    to->RefinanceMediaEffect = buildCurve("RfMediaEffectMultiplier", "RfMediaEffectMultiplierStartDate", 1.);
    to->TurnoverLockinEffect = buildCurve("HtLockinEffectMultiplier", "HtLockinEffectMultiplierStartDate", 1.);
    to->TurnoverBurnoutCumulativeSpeed = buildCurve("HtBurnoutCumulativeSpeedMultiplier", "HtBurnoutCumulativeSpeedMultiplierStartDate", 1.);

    return *to;
}

int getCfpmSubModelTerm(const ResidentialMortgage& mortgage)
{
    if (mortgage.OrigTerm >= 300) return 30;
    else if (mortgage.OrigTerm < 300 && mortgage.OrigTerm > 210) return 20;
    else if (mortgage.OrigTerm <= 210 && mortgage.OrigTerm >= 150) return 15;
    else if (mortgage.OrigTerm < 150 && mortgage.OrigTerm > 0) return 10;
    THROW("The Orig Term is: " + std::to_string(mortgage.OrigTerm) + " unable to map sub model term.");
}

MortgageBehavioralSubModelType
getCfpmSubModelType(const ResidentialMortgage& mortgage)
{
    for (const auto& [majorType, subType] : mortgage.MortgageBehavioralSubModel) {
        // note: use contains() in C++20 if iterator is unneeded
        if (CfpmSupportedSubModelTypes().contains(subType))
            return subType;
    }
    THROW(mortgage.Id + " does not contain a supported sub model type of cfpm.");
}

// note: if there is no throw usage this could be constexpr
PrimaryRateType
getCfpmPrimaryRate(const MortgageBehavioralSubModelType& subModelType)
{
    // {FIXME} we could put this in a map somewhere
    switch (subModelType) {
    case MortgageBehavioralSubModelType::Cfpm_F30:
        return PrimaryRateType::fhmrate_pmms;
    case MortgageBehavioralSubModelType::Cfpm_F20:
        return PrimaryRateType::conv_fixed20_pmms;
    case MortgageBehavioralSubModelType::Cfpm_F15:
        return PrimaryRateType::fhcr15_pmms;
    case MortgageBehavioralSubModelType::Cfpm_F10:
        return PrimaryRateType::conv_fixed10_pmms;
    default:
        THROW("Unable to find corresponding PrimaryRateType of " + to_string(subModelType));
    }
}

}  // namespace wfmutil::detail

