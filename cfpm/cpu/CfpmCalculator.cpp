#include <src/core/behavioral/cfpm/cpu/CfpmCalculator.h>

#include <limits>
#include <utility>

#include <src/core/behavioral/cfpm/detail/CfpmParameter.h>
#include <src/core/behavioral/cfpm/detail/CfpmDetail.h>
#include <src/core/behavioral/detail/BehavioralModelUtils.h>
#include <src/core/numerics/FunctionBuilder.h>

namespace wfmcm::detail {

double CfpmCalculator::derived_rate(double pmms15, double pmms30) const noexcept
{
	return Prepay_.DerivedRatePmms15Coef * pmms15 +
		Prepay_.DerivedRatePmms30Coef * pmms30 +
		Prepay_.DerivedRateSpread;
}

CfpmCalculator::CfpmCalculator(CfpmParameter&& param)
{
	createCalculator(std::move(param));
}

void CfpmCalculator::createCalculator(CfpmParameter&& param)
{
	// {FIXME} add checks to make sure the parameters have been read correctly
	//prepayment model
	//Cashout
	Prepay_.Cashout_.Hpa1y = build1dFunc(param.Prepay_.Cashout_.Hpa1y);
	Prepay_.Cashout_.CumHpa = build1dFunc(param.Prepay_.Cashout_.CumHpa);
	Prepay_.Cashout_.AgeDecay = build1dFunc(param.Prepay_.Cashout_.AgeDecay);
	Prepay_.Cashout_.Age = build1dFunc(param.Prepay_.Cashout_.Age);
	Prepay_.Cashout_.Fico = build1dFunc(param.Prepay_.Cashout_.Fico);
	Prepay_.Cashout_.Cltv = build1dFunc(param.Prepay_.Cashout_.Cltv);
	Prepay_.Cashout_.Lockin = build1dFunc(param.Prepay_.Cashout_.Lockin);
	Prepay_.Cashout_.Cals = build1dFunc(param.Prepay_.Cashout_.Cals);

	Prepay_.Cashout_.ValuationYear = buildYearInterpolation(param.Prepay_.Cashout_.ValuationYear);

	Prepay_.Cashout_.PeriodMultiplier = buildYearMonthInterpolation(param.Prepay_.Cashout_.PeriodMultiplier);

	Prepay_.Cashout_.LockinAlpha = param.Prepay_.Cashout_.LockinAlpha;
	Prepay_.Cashout_.Intercept = param.Prepay_.Cashout_.Intercept;
	Prepay_.Cashout_.ServicerFast = param.Prepay_.Cashout_.ServicerFast;
	Prepay_.Cashout_.ServicerSlow = param.Prepay_.Cashout_.ServicerSlow;
	Prepay_.Cashout_.ModelMultiplier = param.Prepay_.Cashout_.ModelMultiplier;

	//Curtailment
	Prepay_.Curtailment_.Wam = build1dFunc(param.Prepay_.Curtailment_.Wam);
	Prepay_.Curtailment_.Age = build1dFunc(param.Prepay_.Curtailment_.Age);
	Prepay_.Curtailment_.Fico = build1dFunc(param.Prepay_.Curtailment_.Fico);
	Prepay_.Curtailment_.Cals = build1dFunc(param.Prepay_.Curtailment_.Cals);

	Prepay_.Curtailment_.FactorRatio = build1dFunc(param.Prepay_.Curtailment_.FactorRatio);

	Prepay_.Curtailment_.Cltv = build1dFunc(param.Prepay_.Curtailment_.Cltv);
	Prepay_.Curtailment_.PurchaseAge = build1dFunc(param.Prepay_.Curtailment_.PurchaseAge);
	Prepay_.Curtailment_.PurchaseAgeWacAdj = create1DFunctionWithDefault(param.Prepay_.Curtailment_.PurchaseAgeWacAdj, 1.);

	Prepay_.Curtailment_.Intercept = param.Prepay_.Curtailment_.Intercept;
	Prepay_.Curtailment_.ModelMultiplier = param.Prepay_.Curtailment_.ModelMultiplier;

	Prepay_.Curtailment_.ValuationDateMultiplier = buildTimeMultiplierFunc(param.Prepay_.Curtailment_.RegimeDate,
		param.Prepay_.Curtailment_.RegimeMult, param.Prepay_.Curtailment_.RegimeRamping);

	Prepay_.Curtailment_.PeriodMultiplier = buildYearMonthInterpolation(param.Prepay_.Curtailment_.PeriodMultiplier);

	//default state is 1
	Prepay_.Curtailment_.State = build_weighted_avg(param.Prepay_.Curtailment_.State, 1.);

	//Turnover
	Prepay_.Turnover_.AgeDecay = build1dFunc(param.Prepay_.Turnover_.AgeDecay);
	Prepay_.Turnover_.Hpa2y = build1dFunc(param.Prepay_.Turnover_.Hpa2y);
	Prepay_.Turnover_.Lockin = build1dFunc(param.Prepay_.Turnover_.Lockin);
	Prepay_.Turnover_.LockinCals = build1dFunc(param.Prepay_.Turnover_.LockinCals);
	Prepay_.Turnover_.HighLtvAgeMult = build1dFunc(param.Prepay_.Turnover_.HighLtvAgeMult);
	Prepay_.Turnover_.Cltv = build1dFunc(param.Prepay_.Turnover_.Cltv);
	Prepay_.Turnover_.LockinDecay = build1dFunc(param.Prepay_.Turnover_.LockinDecay);
	Prepay_.Turnover_.Fico = build1dFunc(param.Prepay_.Turnover_.Fico);
	Prepay_.Turnover_.Sato = build1dFunc(param.Prepay_.Turnover_.Sato);
	Prepay_.Turnover_.Cals = build1dFunc(param.Prepay_.Turnover_.Cals);

	Prepay_.Turnover_.PurchaseAge = build2dFunc(param.Prepay_.Turnover_.PurchaseAge);
	Prepay_.Turnover_.Seasoning = std::move(param.Prepay_.Turnover_.Seasoning);
	//default state is 1
	Prepay_.Turnover_.State = build_weighted_avg(param.Prepay_.Turnover_.State, 1.);
	Prepay_.Turnover_.Property = std::move(param.Prepay_.Turnover_.Property);

	Prepay_.Turnover_.PeriodMultiplier = buildYearMonthInterpolation(param.Prepay_.Turnover_.PeriodMultiplier);

	Prepay_.Turnover_.LockinDecayWala = create1DFunctionWithDefault(param.Prepay_.Turnover_.LockinDecayWala, 1.);
	Prepay_.Turnover_.LockinDecayCumLockin = create1DFunctionWithDefault(param.Prepay_.Turnover_.LockinDecayCumLockin, 1.);

	Prepay_.Turnover_.LockinAlpha = param.Prepay_.Turnover_.LockinAlpha;
	Prepay_.Turnover_.HighOltvCut = param.Prepay_.Turnover_.HighOltvCut;
	Prepay_.Turnover_.HighCltvCut = param.Prepay_.Turnover_.HighCltvCut;
	Prepay_.Turnover_.Intercept = param.Prepay_.Turnover_.Intercept;
	Prepay_.Turnover_.ServicerFast = param.Prepay_.Turnover_.ServicerFast;
	Prepay_.Turnover_.ServicerSlow = param.Prepay_.Turnover_.ServicerSlow;
	Prepay_.Turnover_.ModelMultiplier = param.Prepay_.Turnover_.ModelMultiplier;
	Prepay_.Turnover_.EitCeiling = is_unset_value(param.Prepay_.Turnover_.EitCeiling) ?
		std::numeric_limits<double>::max() : param.Prepay_.Turnover_.EitCeiling;

	//Refinance
	Prepay_.Refinance_.NontpoAge = build1dFunc(param.Prepay_.Refinance_.NontpoAge);
	Prepay_.Refinance_.CorrespondentAge = build1dFunc(param.Prepay_.Refinance_.CorrespondentAge);
	Prepay_.Refinance_.BrokerAge = build1dFunc(param.Prepay_.Refinance_.BrokerAge);
	Prepay_.Refinance_.Fico = build1dFunc(param.Prepay_.Refinance_.Fico);
	Prepay_.Refinance_.Cltv = build1dFunc(param.Prepay_.Refinance_.Cltv);
	Prepay_.Refinance_.Hpa3y = build1dFunc(param.Prepay_.Refinance_.Hpa3y);
	Prepay_.Refinance_.Turbo = build1dFunc(param.Prepay_.Refinance_.Turbo);
	Prepay_.Refinance_.Scurve = build1dFunc(param.Prepay_.Refinance_.Scurve);
	Prepay_.Refinance_.Burnout = build1dFunc(param.Prepay_.Refinance_.Burnout);
	Prepay_.Refinance_.AgingDecay = build1dFunc(param.Prepay_.Refinance_.AgingDecay);
	Prepay_.Refinance_.Sato = build1dFunc(param.Prepay_.Refinance_.Sato);
	Prepay_.Refinance_.HighCltvPenalty = build1dFunc(param.Prepay_.Refinance_.HighCltvPenalty);

	Prepay_.Refinance_.CalsAge = build2dFunc(param.Prepay_.Refinance_.CalsAge);

	Prepay_.Refinance_.PropertyInspectionWaiver = param.Prepay_.Refinance_.PropertyInspectionWaiver;
	Prepay_.Refinance_.EitCeiling = param.Prepay_.Refinance_.EitCeiling;
	Prepay_.Refinance_.EitThreshold = param.Prepay_.Refinance_.EitThreshold;
	Prepay_.Refinance_.Intercept = param.Prepay_.Refinance_.Intercept;
	Prepay_.Refinance_.Lag1Weight = param.Prepay_.Refinance_.Lag1Weight;
	Prepay_.Refinance_.ServicerFast = param.Prepay_.Refinance_.ServicerFast;
	Prepay_.Refinance_.ServicerSlow = param.Prepay_.Refinance_.ServicerSlow;
	Prepay_.Refinance_.ModelMultiplier = param.Prepay_.Refinance_.ModelMultiplier;

	Prepay_.Refinance_.ValuationDateAdjCut = param.Prepay_.Refinance_.ValuationDateAdjCut;
	Prepay_.Refinance_.HighCltvPenaltyBegin = param.Prepay_.Refinance_.HighCltvPenaltyBegin;
	Prepay_.Refinance_.HighCltvPenaltyEnd = param.Prepay_.Refinance_.HighCltvPenaltyEnd;

	Prepay_.Refinance_.Occupancy = std::move(param.Prepay_.Refinance_.Occupancy);
	Prepay_.Refinance_.Property = std::move(param.Prepay_.Refinance_.Property);

	Prepay_.Refinance_.PeriodMultiplier = buildYearMonthInterpolation(param.Prepay_.Refinance_.PeriodMultiplier);

	//default state is 1
	Prepay_.Refinance_.State = build_weighted_avg(param.Prepay_.Refinance_.State, 1.);
	Prepay_.Refinance_.VintageAdj = buildYearInterpolation(param.Prepay_.Refinance_.VintageAdj);

	//Elbow
	Prepay_.ElbowAls = build1dFunc(param.Prepay_.ElbowAls);
	Prepay_.ElbowFico = build1dFunc(param.Prepay_.ElbowFico);
	Prepay_.ElbowCltv = build1dFunc(param.Prepay_.ElbowCltv);
	Prepay_.ElbowOccupancyCltv_SecondHome = build1dFunc(param.Prepay_.ElbowOccupancyCltv_SecondHome);
	Prepay_.ElbowOccupancyCltv_Investor_OldLlpa = build1dFunc(param.Prepay_.ElbowOccupancyCltv_Investor_OldLlpa);
	Prepay_.ElbowOccupancyCltv_Investor_NewLlpa = build1dFunc(param.Prepay_.ElbowOccupancyCltv_Investor_NewLlpa);
	Prepay_.ElbowSato = build1dFunc(param.Prepay_.ElbowSato);

	Prepay_.ElbowFicoWalaCure = build2dFunc(param.Prepay_.ElbowFicoWalaCure);
	Prepay_.ElbowFicoCltv = build2dFunc(param.Prepay_.ElbowFicoCltv);

	Prepay_.ElbowFicoCureCut = param.Prepay_.ElbowFicoCureCut;
	Prepay_.ElbowAdj = param.Prepay_.ElbowAdj;
	Prepay_.ElbowRefinanceAdj = param.Prepay_.ElbowRefinanceAdj;
	Prepay_.ElbowSatoRamp = param.Prepay_.ElbowSatoRamp;
	Prepay_.ElbowPropertyAdj_Condo_HighCltv = param.Prepay_.ElbowPropertyAdj_Condo_HighCltv;
	Prepay_.ElbowIntercept = param.Prepay_.ElbowIntercept;
	Prepay_.ElbowHighCltvCut = param.Prepay_.ElbowHighCltvCut;

	Prepay_.ElbowAdjBegin = param.Prepay_.ElbowAdjBegin;
	Prepay_.ElbowAdjEnd = param.Prepay_.ElbowAdjEnd;
	Prepay_.ElbowRefinanceAdjBegin = param.Prepay_.ElbowRefinanceAdjBegin;
	Prepay_.ElbowRefinanceAdjEnd = param.Prepay_.ElbowRefinanceAdjEnd;
	Prepay_.ElbowLlpaCut = param.Prepay_.ElbowLlpaCut;
	Prepay_.ElbowSecondHomeCltvCut = param.Prepay_.ElbowSecondHomeCltvCut;

	Prepay_.ElbowOccupancy = std::move(param.Prepay_.ElbowOccupancy);
	Prepay_.ElbowProperty = std::move(param.Prepay_.ElbowProperty);
	Prepay_.ElbowPurpose = std::move(param.Prepay_.ElbowPurpose);
	//default state is 0
	Prepay_.ElbowState = build_weighted_avg(param.Prepay_.ElbowState, 0.);

	//other param
	Prepay_.BlendRateCut = param.Prepay_.BlendRateCut;
	Prepay_.Turbo_ = std::move(param.Prepay_.Turbo_);
	Prepay_.WamCorrection = param.Prepay_.WamCorrection;
	Prepay_.DerivedRatePmms15Coef = param.Prepay_.DerivedRatePmms15Coef;
	Prepay_.DerivedRatePmms30Coef = param.Prepay_.DerivedRatePmms30Coef;
	Prepay_.DerivedRateSpread = param.Prepay_.DerivedRateSpread;

	//2d special function: 1st year month, 2nd double, default = 1.
	Prepay_.CalsAdjMult = create2DYearMonthDoubleFunctionWithDefault(param.Prepay_.CalsAdjMult, 1.);
}

}  // namespace wfmcm::detail
