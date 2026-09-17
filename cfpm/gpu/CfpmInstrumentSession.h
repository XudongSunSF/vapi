/**
 * @file CfpmInstrumentSession.h
 * @author Wells Fargo MMDC
 * @brief GPU compatibility header for the unified CFPM instrument session.
 * @copyright 2025 Wells Fargo MMDC
 *
 * The instrument session implementation has been unified in
 * `src/core/behavioral/cfpm/CfpmInstrumentSession.h`. This header is kept so
 * existing includes continue to resolve.
 */

#pragma once

// GPU implementation - only available if WFMCM_CFPM_GPU_ENABLED is defined at compile time
#ifdef WFMCM_CFPM_GPU_ENABLED

#include <src/core/behavioral/cfpm/CfpmInstrumentSession.h>

#endif  // WFMCM_CFPM_GPU_ENABLED