#pragma once
#include "../src/core/domain/Candle.h"

namespace api::upbit::mappers {

    // 추후 day와 week으로 늘려간다
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
            // 프로젝트 스타일에 맞춰 처리:
            // 1) throw 금지 선호면 기본값 선택
            // 2) 또는 optional/expected로 위로 전파
            return core::TimeFrame::MIN_5;
        }
    }

} // namespace api::upbit::mappers
