#pragma once

#include "core/domain/Candle.h"

namespace api::upbit::mappers {

    // 현재 WS 캔들은 minute 단위만 쓰므로 변환 범위도 minute enum으로 제한한다.
    inline core::TimeFrame toTimeFrameFromMinuteUnit(int unit)
    {
        switch (unit)
        {
        case 3:   return core::TimeFrame::MIN_3;
        case 5:   return core::TimeFrame::MIN_5;
        case 10:  return core::TimeFrame::MIN_10;
        case 15:  return core::TimeFrame::MIN_15;
        case 30:  return core::TimeFrame::MIN_30;
        case 60:  return core::TimeFrame::MIN_60;
        case 240: return core::TimeFrame::MIN_240;
        default:
            return core::TimeFrame::MIN_5;
        }
    }

} // namespace api::upbit::mappers
