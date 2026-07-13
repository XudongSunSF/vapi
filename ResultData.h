#ifndef WFMCM_RESULT_DATA_H
#define WFMCM_RESULT_DATA_H

#include <src/app-common/messages/responses/Response.h>

#include <utility>

namespace app::vasara {

/**
 * Calc result wrapper returned by the vasara calc* APIs.
 * Owns its Response by value; no native handles are held here, so it has no
 * interaction with the Java-side handle stack and may be freed by either side.
 */
struct ResultData
{
    app::messages::Response response_;

    ResultData() = default;

    explicit ResultData(app::messages::Response response)
        : response_(std::move(response))
    {}
};

} // namespace app::vasara

#endif // WFMCM_RESULT_DATA_H
