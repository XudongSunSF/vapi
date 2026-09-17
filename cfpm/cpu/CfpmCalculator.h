#ifndef WFMCM_CFPM_CALCULATOR_H
#define WFMCM_CFPM_CALCULATOR_H

#include <chrono>
#include <functional>
#include <map>

// note: include path will change on name change
#include <mortgage/utility/types/constants.h>

#include <src/core/behavioral/detail/BehavioralModelUtils.h>
#include <src/core/states.h>
#include <wfmcm/mortgage_enums.h>

// {FIXME}
//
// need to rethink the relation between "parameter" and "calculator" types.
// currently a lot of hand-coding is being done to allow the "calculator" type
// to be created from a consumed (via std::move) "parameter" type.
//
// there are notes that the CfpmParameter (and maybe other related types)
// should use polyvar but there is no explanation of what capacity and how.
//

namespace wfmcm::detail {

struct CfpmParameter;

// {FIXME} document, just looks like a way to hold functors for computation
struct CfpmCalculator {
private:
    // helper for unset_value
    template <typename T>
    static inline const T& unset_v = wfmutil::unset_value<T>;

public:

    CfpmCalculator() = default;

    /**
     * Ctor.
     *
     * Populate the calculator state using the given `CfpmParameter` state.
     */
    CfpmCalculator(CfpmParameter&& param);

    struct Prepay {

        struct Cashout {
            std::function<double(double)> Hpa1y;
            std::function<double(double)> CumHpa;
            std::function<double(double)> AgeDecay;
            std::function<double(double)> Age;
            std::function<double(double)> Fico;
            std::function<double(double)> Cltv;
            std::function<double(double)> Lockin;
            std::function<double(double)> Cals;

            std::function<double(std::chrono::year)> ValuationYear;
            std::function<double(std::chrono::year_month)> PeriodMultiplier;

            double ServicerFast = unset_v<double>;
            double ServicerSlow = unset_v<double>;
            double LockinAlpha = unset_v<double>;
            double Intercept = unset_v<double>;
            double ModelMultiplier = unset_v<double>;
        };

        struct Curtailment {
            std::function<double(double)> Wam;
            std::function<double(double)> Age;
            std::function<double(double)> Fico;
            std::function<double(double)> Cals;
            std::function<double(double)> FactorRatio;
            std::function<double(double)> Cltv;
            std::function<double(double)> PurchaseAge;
            std::function<double(double)> PurchaseAgeWacAdj;

            std::function<double(std::chrono::year_month)> PeriodMultiplier;

            double Intercept = unset_v<double>;
            double ModelMultiplier = unset_v<double>;

            std::function<double(std::chrono::year_month)> ValuationDateMultiplier;
            std::function<double(const std::map<State, double>&)> State;
        };

        struct Turnover {
            std::function<double(double)> AgeDecay;
            std::function<double(double)> Hpa2y;
            std::function<double(double)> Lockin;
            std::function<double(double)> LockinCals;
            std::function<double(double)> HighLtvAgeMult;
            std::function<double(double)> Cltv;
            std::function<double(double)> LockinDecay;
            std::function<double(double)> Fico;
            std::function<double(double)> Sato;
            std::function<double(double)> Cals;
            std::function<double(double)> LockinDecayWala;
            std::function<double(double)> LockinDecayCumLockin;

            std::function<double(std::chrono::year_month)> PeriodMultiplier;

            std::function<double(double, double)> PurchaseAge;

            std::map<std::chrono::month, double> Seasoning;
            std::function<double(const std::map<State, double>&)> State;
            std::map<PropertyType, double> Property;

            double LockinAlpha = unset_v<double>;
            double HighOltvCut = unset_v<double>;
            double HighCltvCut = unset_v<double>;
            double Intercept = unset_v<double>;
            double ServicerFast = unset_v<double>;
            double ServicerSlow = unset_v<double>;
            double ModelMultiplier = unset_v<double>;
            double EitCeiling = unset_v<double>;
        };

        struct Refinance {
            std::function<double(double)> NontpoAge;
            std::function<double(double)> CorrespondentAge;
            std::function<double(double)> BrokerAge;
            std::function<double(double)> Fico;
            std::function<double(double)> Cltv;
            std::function<double(double)> Hpa3y;
            std::function<double(double)> Turbo;
            std::function<double(double)> Scurve;
            std::function<double(double)> Burnout;
            std::function<double(double)> AgingDecay;
            std::function<double(double)> Sato;
            std::function<double(double)> HighCltvPenalty;
            std::function<double(std::chrono::year_month)> PeriodMultiplier;

            std::function<double(double, double)> CalsAge;

            double PropertyInspectionWaiver = unset_v<double>;
            double EitCeiling = unset_v<double>;
            double EitThreshold = unset_v<double>;
            double Intercept = unset_v<double>;
            double Lag1Weight = unset_v<double>;
            double ServicerFast = unset_v<double>;
            double ServicerSlow = unset_v<double>;
            double ModelMultiplier = unset_v<double>;

            std::chrono::year_month ValuationDateAdjCut;
            std::chrono::year_month HighCltvPenaltyBegin;
            std::chrono::year_month HighCltvPenaltyEnd;

            std::map<OccupancyType, double> Occupancy;
            std::map<PropertyType, double> Property;
            std::function<double(const std::map<State, double>&)> State;
            std::function<double(std::chrono::year)> VintageAdj;
        };

        //Elbow
        std::function<double(double)> ElbowAls;
        std::function<double(double)> ElbowFico;
        std::function<double(double)> ElbowCltv;
        std::function<double(double)> ElbowOccupancyCltv_SecondHome;
        std::function<double(double)> ElbowOccupancyCltv_Investor_OldLlpa;
        std::function<double(double)> ElbowOccupancyCltv_Investor_NewLlpa;
        std::function<double(double)> ElbowSato;

        std::function<double(double, double)> ElbowFicoWalaCure;
        std::function<double(double, double)> ElbowFicoCltv;

        double ElbowFicoCureCut = unset_v<double>;
        double ElbowAdj = unset_v<double>;
        double ElbowRefinanceAdj = unset_v<double>;
        double ElbowSatoRamp = unset_v<double>;
        double ElbowPropertyAdj_Condo_HighCltv = unset_v<double>;
        double ElbowIntercept = unset_v<double>;
        double ElbowHighCltvCut = unset_v<double>;

        std::chrono::year_month ElbowAdjBegin;
        std::chrono::year_month ElbowAdjEnd;
        std::chrono::year_month ElbowRefinanceAdjBegin;
        std::chrono::year_month ElbowRefinanceAdjEnd;
        std::chrono::year_month ElbowLlpaCut;
        std::chrono::year_month ElbowSecondHomeCltvCut;

        std::map<OccupancyType, double> ElbowOccupancy;
        std::map<PropertyType, double> ElbowProperty;
        std::map<PurposeType, double> ElbowPurpose;
        std::function<double(const std::map<State, double>&)> ElbowState;

        //other param
        std::function<double(std::chrono::year_month, double)> CalsAdjMult;

        double BlendRateCut = unset_v<double>;
        double WamCorrection = unset_v<double>;
        double DerivedRatePmms15Coef = unset_v<double>;
        double DerivedRatePmms30Coef = unset_v<double>;
        double DerivedRateSpread = unset_v<double>;

        Cashout Cashout_;
        Curtailment Curtailment_;
        Turnover Turnover_;
        Refinance Refinance_;
        TurboParam Turbo_;
    };

    Prepay Prepay_;

    /**
     * Compute a derived fixed tenor rate using PMMS 15, 30 rates.
     *
     * This uses the prepay structure's derived rate PMMS 15 and 30
     * coefficients and spread values to compute the derived rate. Whether the
     * output is for fixed 10 or 20 year depends on the state values.
     *
     * @param pmms15 PMMS 15 rate
     * @param pmms30 PMMS 30 rate
     */
    double derived_rate(double pmms15, double pmms30) const noexcept;

private:
    /**
     * Populate the CFPM calculator state from the parameter values.
     */
    void createCalculator(CfpmParameter&& param);
};

}  // namespace wfmcm::detail

#endif  // WFMCM_CFPM_CALCULATOR_H
