#include <src/core/behavioral/cfpm/CfpmInstrumentRatePathSession.h>

using namespace std;

namespace wfmcm
{
	CfpmInstrumentRatePathSessionBuilder& 
	CfpmInstrumentRatePathSessionBuilder::withEitLag2(std::pmr::vector<double>&& eitLag2) {
		pathDependent_.eitLag2_ = std::move(eitLag2);
		return *this;
	}

	CfpmInstrumentRatePathSessionBuilder& 
	CfpmInstrumentRatePathSessionBuilder::withCombinedEit(std::pmr::vector<double>&& combinedEit) {
		pathDependent_.combinedEit_ = std::move(combinedEit);
		return *this;
	}

	CfpmInstrumentRatePathSessionBuilder& 
	CfpmInstrumentRatePathSessionBuilder::withBurnout(std::pmr::vector<double>&& burnout) {
		pathDependent_.burnout_ = std::move(burnout);
		return *this;
	}

	CfpmInstrumentRatePathSessionBuilder&
		CfpmInstrumentRatePathSessionBuilder::withCumLockin(std::pmr::vector<double>&& cumLockin) {
		pathDependent_.cumLockin_ = std::move(cumLockin);
		return *this;
	}


	CfpmInstrumentRatePathSession 
	CfpmInstrumentRatePathSessionBuilder::build() {
		return std::move(pathDependent_);
	}
}
