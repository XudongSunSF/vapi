#ifndef WFMCM_CFPMPARAMETER_H_
#define WFMCM_CFPMPARAMETER_H_

#include <wfmcm/mortgage_enums.h>
#include <src/core/states.h>
#include <mortgage/utility/containers/lookup/lookup.h>
#include <mortgage/utility/strings/to_string.h>
#include <mortgage/utility/time/time_series.h>
#include <src/core/numerics/FunctionParameters.h>
#include <src/core/behavioral/detail/BehavioralModelUtils.h>

#include <vector>
#include <map>

namespace wfmcm::detail 
{
	//FIXME: Replace with polyvar
	struct CfpmParameter
	{
		struct Prepay {
			struct Cashout {
				OneDimFuncParameter Hpa1y;
				OneDimFuncParameter CumHpa;
				OneDimFuncParameter AgeDecay;
				OneDimFuncParameter Age;
				OneDimFuncParameter Fico;
				OneDimFuncParameter Cltv;
				OneDimFuncParameter Lockin;
				OneDimFuncParameter Cals;
				OneDimFuncParameter ValuationYear;
				OneDimFuncParameter PeriodMultiplier;

				double ServicerFast;
				double ServicerSlow;
				double LockinAlpha;
				double Intercept;
				double ModelMultiplier;
			};

			struct Turnover {
				OneDimFuncParameter AgeDecay;
				OneDimFuncParameter Hpa2y;
				OneDimFuncParameter Lockin;
				OneDimFuncParameter LockinCals;
				OneDimFuncParameter HighLtvAgeMult;
				OneDimFuncParameter Cltv;
				OneDimFuncParameter LockinDecay;
				OneDimFuncParameter Fico;
				OneDimFuncParameter Sato;
				OneDimFuncParameter Cals;
				OneDimFuncParameter PeriodMultiplier;
				OneDimFuncParameter LockinDecayWala;
				OneDimFuncParameter LockinDecayCumLockin;

				TwoDimFuncParameter PurchaseAge;

				std::map<std::chrono::month, double> Seasoning;
				std::map<enum wfmcm::State, double> State;
				std::map<PropertyType, double> Property;

				double LockinAlpha;
				double HighOltvCut;
				double HighCltvCut;
				double Intercept;
				double ServicerFast;
				double ServicerSlow;
				double ModelMultiplier;
				double EitCeiling;
			};
			struct Refinance {
				OneDimFuncParameter NontpoAge;
				OneDimFuncParameter CorrespondentAge;
				OneDimFuncParameter BrokerAge;
				OneDimFuncParameter Fico;
				OneDimFuncParameter Cltv;
				OneDimFuncParameter Hpa3y;
				OneDimFuncParameter Turbo;
				OneDimFuncParameter Scurve;
				OneDimFuncParameter Burnout;
				OneDimFuncParameter AgingDecay;
				OneDimFuncParameter Sato;
				OneDimFuncParameter HighCltvPenalty;
				OneDimFuncParameter VintageAdj;
				OneDimFuncParameter PeriodMultiplier;


				TwoDimFuncParameter CalsAge;

				double PropertyInspectionWaiver;
				double Intercept;
				double Lag1Weight;
				double ServicerFast;
				double ServicerSlow;
				double EitThreshold;
				double EitCeiling;
				double ModelMultiplier;

				std::chrono::year_month ValuationDateAdjCut;
				std::chrono::year_month HighCltvPenaltyBegin;
				std::chrono::year_month HighCltvPenaltyEnd;

				std::map<OccupancyType, double> Occupancy;
				std::map<PropertyType, double> Property;
				std::map<enum wfmcm::State, double> State;
			};
			struct Curtailment {
				OneDimFuncParameter Wam;
				OneDimFuncParameter Age;
				OneDimFuncParameter Fico;
				OneDimFuncParameter Cals;
				OneDimFuncParameter FactorRatio;
				OneDimFuncParameter Cltv;
				OneDimFuncParameter PurchaseAge;
				OneDimFuncParameter PeriodMultiplier;
				OneDimFuncParameter PurchaseAgeWacAdj;

				double Intercept;
				std::chrono::year_month RegimeDate;
				double RegimeMult;
				double RegimeRamping;
				double ModelMultiplier;

				std::map<enum wfmcm::State, double> State;
			};

			Cashout Cashout_;
			Turnover Turnover_;
			Curtailment Curtailment_;
			Refinance Refinance_;
			TurboParam Turbo_;

			OneDimFuncParameter ElbowAls;
			OneDimFuncParameter ElbowFico;
			OneDimFuncParameter ElbowCltv;
			OneDimFuncParameter ElbowOccupancyCltv_SecondHome;
			OneDimFuncParameter ElbowOccupancyCltv_Investor_OldLlpa;
			OneDimFuncParameter ElbowOccupancyCltv_Investor_NewLlpa;
			OneDimFuncParameter ElbowSato;

			TwoDimFuncParameter ElbowFicoWalaCure;
			TwoDimFuncParameter ElbowFicoCltv;

			double ElbowFicoCureCut;
			double ElbowAdj;
			double ElbowRefinanceAdj;
			double ElbowSatoRamp;
			double ElbowPropertyAdj_Condo_HighCltv;
			double ElbowIntercept;
			double ElbowHighCltvCut;

			std::chrono::year_month ElbowAdjBegin;
			std::chrono::year_month ElbowAdjEnd;
			std::chrono::year_month ElbowRefinanceAdjBegin;
			std::chrono::year_month ElbowRefinanceAdjEnd;
			std::chrono::year_month ElbowLlpaCut;
			std::chrono::year_month ElbowSecondHomeCltvCut;


			std::map<OccupancyType, double> ElbowOccupancy;
			std::map<PropertyType, double> ElbowProperty;
			std::map<PurposeType, double> ElbowPurpose;
			std::map<State, double> ElbowState;

			//other param
			double BlendRateCut;
			double WamCorrection;

			double DerivedRatePmms15Coef = unset_value<double>;
			double DerivedRatePmms30Coef = unset_value<double>;
			double DerivedRateSpread = unset_value<double>;

			TwoDimFuncParameter CalsAdjMult;
		};
		Prepay Prepay_;
	};
}

#endif
