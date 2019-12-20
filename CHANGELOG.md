# Version 1.46.2.6
- If a decrease in quantity fails due to in-flight actions, prevent anything from increasing quantity at a level
- for that product for that side until in-flight action is resolved.

# Version 1.46.2.5
- Fixed infinite banner loop 

# Version 1.46.2.4
- Reverted bugfix in v1.46.23
- Changed Quote Banner s.t. if worse quote is let through the banner, the banner px/qty is set to the new price/qty
- Fixed a bug where Quote Banner wouldn't expire correctly due to putting quote back in before resetting banner

# Version 1.46.2.3 #
- [REVERTED IN 1.46.2.4] Fixed a bug where IME banner wouldn't ban moving back to the same price and quantity as when banner was set

# Version 1.46.2.2 #
- Fixed a bug where own orders would not be removed until NO Ack due to bad pricing formula in on MD update. (rhonepublic only)

# Version 1.46.2.1 #
- Added event to detect intra-event sweeps
- Added parameters (SweepTicks/SweepQty) to disable RV disable MM external quote on intra-event sweeps

# Version 1.46.2.0 #
- Implied orders on CME integrated into book and Viper-specific Trade events
- RV will explicitly check to see if book changed rather than assume event correctly reports it

# Version 1.46.1.10 #
- Fixed a log statement that would send the log generator into an infinite loop
- EMA of <= 0s interpreted as "Set LTF to Market"

# Version 1.46.1.9 #
- Prevent a crash due to bad OM Event handling in ICE Decoder
- Added new mandatory Tag1031 to CME OM
- Added new 'MaxTicksThroughMarket' parameter to RV

# Version 1.46.1.8 #
- MaxTicksOffTopLevel behaves correctly with an empty book
- Force Python LogWriterReceiver to explicitly subscribe to multicast channels
- Apply realpath to model to allow swapping sym link to new model while Viper is running /w old model
- Several errors that would result in disabling RV+MM instead will result in disabling RV and disabling External quoting in MM, including...
- + Prior: Disable RV+MM, Current: *NO CHANGE*
- + - Hedger could not create order (HedgersManager.h: 218)
- + - Stacker could not create order (Stacker.h: 574)
- + - Hedger order rejected due to SMP, not in payup (HedgesManager.h:357)
- + - MM: "handlePayUpSelfMatch" occurs five times (HedgesManager.h:409)
- + - Hedger: Order cancel rejected not due to fill in flight (HedgesManager.h:367)
- + Prior: Disable RV+MM, Current: Disable RV, Disable External Quote in MM
- + - RV Hedge Order Rejected, not due to SMP (Executor.h:1543)
- + - Order rejected due to RV Hedge SMP more than 20 times. (Executor.h:1574)  //This can only happen on ICE.
- + - onOrderCanceled due to SMP. (Executor.h:1642)  //This can only happen on CME.  Note if this is a hedger order, it will disable hedger as well
- + - After changing parameters, Market Maker would cross (ExecutorForOneContract.h:115)
- + - MM: When enabling MM, if there is a Position Limit breach, MM enable with only external quoting disabled (SimpleMarketMaker.h:815)
- + - Stacker: Order is cancel rejected not due to fill-in-flight and Stacker owns the order (Stacker.h:1077)
- + Prior: Disable MM, Current: Disable External Quoting in MM
- + - Fill returned from RV to MM, position limits after MM takes over is violated (Executor.h:256)
- + - External fill was overfill, remaining size after Market Maker takes over fill, RV is disabled (Executor.h:332)

# Version 1.46.1.7 #
- Several errors that would result in disabling RV+MM instead will result in disabling RV and disabling External quoting in MM, including...
+ ... Negative Scalp Breach
+ ... PnL Limit Violations
+ ... PnL Limit Violation on changing PnL parameters
+ ... PnL Limit Violations when explicitly sending PnL state to GUI -- including enabling Market Maker.

# Version 1.46.1.6 #
- Added support for new Manual Fills Entry window
- Fixed a bug where P&L Limits for RV and MM would be applied to 'total' instead

# Version 1.46.1.5 #
- Added 'maxTicksOffTopLevel' parameter to stacker to prevent quoting deep in the book

# Version 1.46.1.4 #
- Added explicit check on RV/MM startup for negative quantities
- Removed old EndOfEventBook handler from Rhone

# Version 1.46.1.3 #
- Reverted om::ORDER_NUM change, now back to 2M
- Update LogWriter to use $HOME instead of absolute path

# Version 1.46.1.2 #
- Can set RVQuoteMaxPercentWidth (now called %stfMove) per-contract
- Pre-allocate space for FAK adjustment trackers
- Increase om::ORDER_NUM to 6M, forced some objects to be heap-allocated to fit within binary image.
- Fixed CME FAK handling logic to send FAKDone events correctly
- Added a new MD decoder (Cme::MBO::EndOfEventBookWithFAKRemoval::MSockets) which on a FAK miss, will remove the level.

# Version 1.46.1.1 #
- Reserve space on FillBanner to prevent realloc

# Version 1.46.1.0 #
- Added support for modifies on ICE Stacker
- Bumped max order number up to 2M
- Made banner comparison function transitive, which may resolve a std::sort(...) crash
- Removed commas from log statement, which crash the log file generator

# Version 1.46.0.1 #
- Made overfills (i.e. MO->CO or MO->OM->CO) non-atomic, to allow quote(...) to be called between individual events.
- GUI: Time columns now typed properly to allow sorting, added confirmation to main trader control buttons.

# Version 1.46.0.0 #
- Stacker modified to use modifies
- New GUI proto

# Version 1.45.3.5#
- Allow the use of Gateway Containers
- Allow Round-Robining between Gateway Containers
- Requires at least viper-1.43.3

# Version  1.45.3.4#
- On CME SMP Rejects, explicitly remove timer and use existing SMP logic to prevent payup loop

# Version 1.45.3.3#
- Added Zhi as an authorized trader

