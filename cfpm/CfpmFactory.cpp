/**
 * @file CfpmFactory.cpp
 * @author Wells Fargo MMDC
 * @brief Factory implementation for creating CFPM models with runtime CPU/GPU selection
 * @copyright 2025 Wells Fargo MMDC
 */

#include <src/core/behavioral/cfpm/CfpmFactory.h>
#include <src/core/behavioral/cfpm/cpu/Cfpm.h>
#include <src/core/behavioral/cfpm/cpu/CfpmInstrumentSession.h>  // For complete CfpmInstrumentEx type
#include <src/core/behavioral/MortgageBehavioralModelImpl.h>
#include <src/core/behavioral/cfpm/gpu/Cfpm.h>  // CfpmGpuImpl (contents guarded by WFMCM_CFPM_GPU_ENABLED)

#include <mortgage/utility/logging/logging.h>  // For GLOG_INFO, GLOG_WARNING

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace wfmcm {

    bool CfpmFactory::gpuChecked_ = false;
    bool CfpmFactory::gpuAvailable_ = false;

    // Function pointer type for GPU availability check from CUDA DLL
    typedef int (*GetDeviceCountFunc)(int*);
    typedef const char* (*GetDeviceNameFunc)(int);

    bool CfpmFactory::isGpuAvailable() {
        if (!gpuChecked_) {
            gpuAvailable_ = false;

            // Try to dynamically load the CUDA runtime DLL
#ifdef _WIN32
        // Load CUDA runtime DLL - this contains cudaGetDeviceCount
            HMODULE cudaDll = LoadLibraryA("cudart64_12.dll");
            if (!cudaDll) {
                // Try CUDA 11 if CUDA 12 not found
                cudaDll = LoadLibraryA("cudart64_11.dll");
            }
            if (!cudaDll) {
                GLOG_INFO("CfpmFactory: CUDA runtime DLL not found, GPU unavailable");
                gpuChecked_ = true;
                return false;
            }

            // Try to get device count from CUDA runtime
            auto cudaGetDeviceCount = (GetDeviceCountFunc)GetProcAddress(cudaDll, "cudaGetDeviceCount");
            if (!cudaGetDeviceCount) {
                GLOG_WARNING("CfpmFactory: cudaGetDeviceCount function not found in DLL");
                gpuChecked_ = true;
                return false;
            }

            int deviceCount = 0;
            int result = cudaGetDeviceCount(&deviceCount);
            if (result != 0) {
                GLOG_INFO("CfpmFactory: cudaGetDeviceCount failed with error code " + std::to_string(result));
                gpuChecked_ = true;
                return false;
            }

            if (deviceCount > 0) {
                GLOG_INFO("CfpmFactory: Detected " + std::to_string(deviceCount) + " CUDA device(s)");
                gpuAvailable_ = true;
            }
            else {
                GLOG_INFO("CfpmFactory: No CUDA devices detected");
            }
            // Note: Keep DLL loaded for future use
#else
            void* cudaDll = dlopen("libmortgage.cuda.so", RTLD_LAZY);
            if (!cudaDll) {
                GLOG_INFO("CfpmFactory: CUDA library (libmortgage.cuda.so) not found, GPU unavailable");
                gpuChecked_ = true;
                return false;
            }

            auto cudaGetDeviceCount = (GetDeviceCountFunc)dlsym(cudaDll, "cudaGetDeviceCount");
            if (!cudaGetDeviceCount) {
                GLOG_WARNING("CfpmFactory: cudaGetDeviceCount function not found in library");
                gpuChecked_ = true;
                return false;
            }

            int deviceCount = 0;
            int result = cudaGetDeviceCount(&deviceCount);
            if (result != 0) {
                GLOG_INFO("CfpmFactory: cudaGetDeviceCount failed with error code " + std::to_string(result));
                gpuChecked_ = true;
                return false;
            }

            if (deviceCount > 0) {
                GLOG_INFO("CfpmFactory: Detected " + std::to_string(deviceCount) + " CUDA device(s)");
                gpuAvailable_ = true;
            }
            else {
                GLOG_INFO("CfpmFactory: No CUDA devices detected");
            }
#endif
            gpuChecked_ = true;
        }
        return gpuAvailable_;
    }

    std::unique_ptr<IMortgageBehavioralModel> CfpmFactory::tryCreateGpuModel(
        MortgageBehavioralModelType type,
        const std::map<ModelParamName, std::string>& modelSpec,
        const wfmutil::context& ctx)
    {
#ifdef WFMCM_CFPM_GPU_ENABLED
        if (isGpuAvailable()) {
            return std::make_unique<MortgageBehavioralModelImpl<CfpmGpuImpl>>(type, modelSpec, ctx);
        }
#else
        (void)type;
        (void)modelSpec;
        (void)ctx;
#endif
        return nullptr;
    }

} // namespace wfmcm