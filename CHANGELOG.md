# Version 1.47.0.6
- Bugfix: Fixed RV factor max position check.
- Bugfix: Fixed an issue where MM hedge changed from payup to none-payup, if internal hedge price has not changed,
  the hedge order did not send to IME. If payUp mode has changed, the hedge order should send to IME.
- Bugfix: Always send internal RV fill to Front, then follows with missed hedge fill. 
- Instead of expiring hedge banner, PayUp hedge now updates hedge banner's price and size.

# Version 1.47.0.5
- Implement static RV quote.
- Increased Viper version sending delay from 10 sec to 15 sec.
- Bugfix: Fixed another infinite loop happened when disabling MM and RV due to invalid tick size.
- Bugfix: Fixed MarketMaker internal/external has offset position issue.
- Bugfix: Fixed a MarketMaker take overfill issue where when external position reached maxExtPosition limit, 
  it will take a negative position.

# Version 1.47.0.4
- Removed check in RV for crossed book (check still exists in MM) due to pre-open triggers
- Round HedgeRatio to 4th decimal place.
- Bugfix: Fixed a bug in CME book cleaning based on trade summary that would crash if no best bid exists and bid trades (i.e. with an implied with implieds off).
- Bugfix: Fixed MM infinite loop caused by invalid tick size.

# Version 1.47.0.3
- Increased client message buffer size.
- Implemented no hedge path.

# Version 1.47.0.2
- Fixed Hedge target not set to 0 when hedge banner is enabled.
- Fixed log messages.

# Version 1.47.0.1
- Fixed an issue where wrong fill size sent to front when part of the fill matched with MM hedge. In this case only the
  matched size should send to front not entire fill size.

# Version 1.47.0.0
- Merged Stacker and HedgeManager into OrderManager. Removed ownership from orders.
- Changed CME SelfMatchPrevention to cancel resting.
- Introduced hedgeOffset MM Parameter. Changed internal hedge price to opposite side better of our internal/external
  offer +- hedge offset (minus for bid hedges). 
- Changed MM external quote to not better than top of book.
- When new order rejected, if it is for hedge, the hedge level will be synchronized immediately. If it is for quote, 
  a fill banner will be started. The level will be synchronized when fill banner expire.
- On SMP unsolicited order cancel (the resting order), if it is for hedge, the hedge level will be synchronized 
  immediately. If it is for quote, a fill banner will be started and the level will be synchronized when fill banner expire.
- A hedgeRetryCount is introduced to prevent infinite hedge retry. On new hedge order reject or SMP unsolicited cancel
  hedgeRetryCount will be increased by one. When it reached 20, MM and RV will be disabled. hedgeRetryCount will be
  reset when hedge target to 0.
- Removed hedgeIMMDelayMics global setting.
- Fixed hedge size not updated after internal fill.
- Changed Market Maker Quote internal size to the sum of internal and external size. Internal overfill will be allocated
  to external.
- Send Viper Version Information to GUI.

# Version 1.46.2.9
- Compatibility changes for RhoneGUI v1.4.0
- Server to Client heartbeats added (default=900ms, frontside parameter send_gui_heartbeat_interval_in_ms to override)
- Sweep detector will not alert GUI if no MM/RV state changes
- If IME matching fails (ex: crossed book) RV will be disabled, MM will disable offending product.  RV will not be allowed to re-enable
- Bugfix: If MM/Stacker changed enabled state, it would not update in the GUI.
- Bugfix: If book was updated by a trade message, the order count used would be copied from the previous best level
- Bugfix: Is MM disabled while a payup hedge that was removed from the IME was in flight, the hedge order would remain in the market

# Version 1.46.2.8
- Bugfix: Cancel in flight on ICE could incorrectly be marked as a blocking order on NO Ack.

# Version 1.46.2.7
- Fixed two cases in the decrease logic where new orders would not be blocked.  One involving bad detection on
  the LevelStealer ctor, the second not respecting the return value in the tryModify method in the level stealer.
  
# Version 1.46.2.6
- If a decrease in quantity fails due to in-flight actions, prevent anything from increasing quantity at a level
  for that product for that side until in-flight action is resolved.

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

