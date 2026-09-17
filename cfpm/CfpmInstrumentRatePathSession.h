#ifndef WFMCM_INSTRUMENT_RATEPATH_SESSION_H
#define WFMCM_INSTRUMENT_RATEPATH_SESSION_H

#include <vector>

namespace wfmcm
{
	//Instrument dependent, Rate dependent, Model dependent
	struct CfpmInstrumentRatePathSession {
		double eitLag1(size_t idx) const { return eitLag2_[idx + 1]; }
		double eitLag2(size_t idx) const { return eitLag2_[idx]; }
		double comBinedEit(size_t idx) const { return combinedEit_[idx]; }
		double burnout(size_t idx) const { return burnout_[idx]; }
		double cumLockin(size_t idx) const { return cumLockin_[idx]; }
	private:
		CfpmInstrumentRatePathSession() = default;
		std::pmr::vector<double> eitLag2_;
		std::pmr::vector<double> combinedEit_;
		std::pmr::vector<double> burnout_;
		std::pmr::vector<double> cumLockin_;
		friend struct CfpmInstrumentRatePathSessionBuilder;
	};

	struct CfpmInstrumentRatePathSessionBuilder {
		CfpmInstrumentRatePathSessionBuilder() = default;
		CfpmInstrumentRatePathSessionBuilder& withEitLag2(std::pmr::vector<double>&& eitLag2);
		CfpmInstrumentRatePathSessionBuilder& withCombinedEit(std::pmr::vector<double>&& combinedEit);
		CfpmInstrumentRatePathSessionBuilder& withBurnout(std::pmr::vector<double>&& burnout);
		CfpmInstrumentRatePathSessionBuilder& withCumLockin(std::pmr::vector<double>&& cumLockin);
		CfpmInstrumentRatePathSession build();
	private:
		CfpmInstrumentRatePathSession pathDependent_;
	};
}

#endif

