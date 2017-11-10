//
// Created by cmilutinovic on 7/12/16.
//

#ifndef RHONEOMS_GSCALPPRODUCTLIMIT_H
#define RHONEOMS_GSCALPPRODUCTLIMIT_H

#include <string>

namespace oms{

    /*
     * Exchange,Type  ,Code ,Name          ,FeesPerFill,FrequencyMask,TradingPeriods,ConsecutiveMonths,IsRestricted,OnlySameMonths,Decimals,TickIncrement,TickValue,RhoneConvFactor
     * CME     ,Future,ES   ,E-mini S&P 500,0.42       ,2340         ,5             ,0                 ,0          ,0             ,0       ,25            ,12.5    ,100000
     */
    
    struct RawProductLimitRow{
        std::string exchange;
        std::string type;
        std::string code;
        std::string name;
        double feesPerFill;
        int frequencyMask;
        int tradingPeriods;
        int consecutiveMonths;
        bool isRestriced;
        bool onlySameMonths;
        int decimals;
        int tickIncrement;
        double tickValue;
        int rhoneConvFactor;
    };
}

#endif //RHONEOMS_GSCALPPRODUCTLIMIT_H
