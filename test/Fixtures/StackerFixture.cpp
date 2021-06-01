#include "StackerFixture.h"

namespace Python {

ConfigContract iceContract {._name="name", ._gid=0, ._sid=0, ._tickSize=Python::ToRhonePrice(0.01), ._tickValue=0, ._venue=hf::VENUE::ICE, ._internalId=1, ._orderIncrementQty=1 };
ConfigContract cmeContract {._name="name", ._gid=0, ._sid=0, ._tickSize=Python::ToRhonePrice(0.01), ._tickValue=0, ._venue=hf::VENUE::CME, ._internalId=1, ._orderIncrementQty=1};
ConfigContract eurexContract {._name="name", ._gid=0, ._sid=0, ._tickSize=Python::ToRhonePrice(0.01), ._tickValue=0, ._venue=hf::VENUE::EUREX, ._internalId=1, ._orderIncrementQty=1};
StackerConfig config_60_0_0 {._contract="", ._quoteSize=60, ._quoteGap=1, ._maxQuote=0, ._numberOfLevels=0, ._stackSlack=0, ._maxTicksOffTopLevel=-1, ._useGtc=false, ._allowMultipleNewHedge=false, ._enableQuoteSprayer=false};
StackerConfig config_60_2_0 {._contract="", ._quoteSize=60, ._quoteGap=1, ._maxQuote=0, ._numberOfLevels=2, ._stackSlack=0, ._maxTicksOffTopLevel=-1, ._useGtc=false, ._allowMultipleNewHedge=false, ._enableQuoteSprayer=false};
StackerConfig config_60_2_0_allowMultipleNewHedge {._contract="", ._quoteSize=60, ._quoteGap=1, ._maxQuote=0, ._numberOfLevels=2, ._stackSlack=0, ._maxTicksOffTopLevel=-1, ._useGtc=false, ._allowMultipleNewHedge=true, ._enableQuoteSprayer=false};
StackerConfig config_60_5_2 {._contract="", ._quoteSize=60, ._quoteGap=1, ._maxQuote=0, ._numberOfLevels=5, ._stackSlack=2, ._maxTicksOffTopLevel=-1, ._useGtc=false, ._allowMultipleNewHedge=false, ._enableQuoteSprayer=false};
StackerConfig config_100_5_0 {._contract="", ._quoteSize=100, ._quoteGap=1, ._maxQuote=0, ._numberOfLevels=5, ._stackSlack=0, ._maxTicksOffTopLevel=-1, ._useGtc=false, ._allowMultipleNewHedge=false, ._enableQuoteSprayer=false};

}
