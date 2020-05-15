#Version 1.49.5.3
- Only update PnL on offline GTC fill when CheckQuotes/RV/MM would enable a recalculation
- A reloaded GTC that has been partially filled does not correctly set total quantity in OM

# Version 1.49.5.2
- Only trigger GTC reconciliation if status event triggered in new MD packet

# Version 1.49.5.1
- If MD lost while external quoting disabled, do not disable instrument in MM
- Rhonepublic fix (pull request 1385) for CME snapshot recovery
- Print MD books on monitor disconnect

# Version 1.49.5
- If EMA calculation in rv results in nan, skip ema update
- Implement log throttle (RhoneCore)

# Version 1.49.4
- Added Order Status testing grid
- Disable RV if any product goes into pre-open
- Added green color to GTC grid for non-crossed market
- Fixed overfill handling code to handle multiple overfills before terminal state

# Version 1.49.3.4
- Added some logging statistic log messages

# Version 1.49.3.3
- Bugfix: Use absolute vector position value in isWithinPositionLimits check

# Version 1.49.3.2
- Bugfix: GUI heartbeat code does not reliability send heartbeats due to bad timer calculation

# Version 1.49.3.1
- Bugfix: #Offers and #Orders wrong in GTC Management Grid
- Market without bids or offers now shows as "crossed" in GTC Management grid during pre-open
- Bugfix: fix double value comparison.

# Version 1.49.3
- Bugfix: Skip MD status for contract not used in MM.

# Version 1.49.2
- Fixed detection of ICE ACE cancels
- Added logging of book state on ICE recovery
- Removed logging of duplicate ICE events in Viper

# Version 1.49.1
- Changed more log messages to debug level.
- Bugfix: Fixed ICE FAK filled sometimes does not generate FAK done event.

# Version 1.49.0
- When market data lost, disable RV, disable MM, MM external quoting and useGTC for that instrument.
- Bugfix: Fixed order not canceled when disabling RV after market goes auction state.
- Add ability to filter log message by logging level
- Reduced RV log messages.
- Attempt to send number of GTCs on GTC reload.
- Added debug logging for MM duplicate batch
- Notify user of Firmsoft cancel

# Version 1.48.1
- Rebase onto 1.47.2
- "UseGTC" column configurable

# Version 1.47.2
- Bugfix: Fixed an issue where quote banner is not created if there are both quote and hedge on a level
- Bugfix: Fixed hedge price calculation. Some place still uses mid point.
- Bugfix: Fixed an issue where hedge did not re-send after a blocking order ack received.
- Bugfix: Fixed an ICE quote removal issue where modified order will not be removed from quote when decoder 
  received md update before receiving modify order ack.

# Version 1.47.1
- Changed MMWorsenPayupCond from percent of width to percent of tick. Internal payup price will be the worse of 
  (bestPrice +/- MMWorsenPayupCond%) and (midPoint +/- width).
- When useGTC turned off, if MM is not enabled or cannot quote externally, do not put orders back into market.
- Bugfix: Fixed a crash when disable UseGTC and a hedge GTC order is in the market.
- Added a Stacker parameter to allow/disallow multiple new hedge orders go to the market.

# Version 1.48.0.8
- Changed time calculation for mass-cancel of GTCs not to sometimes time out.  
- Updated cancel/reject check to account for GTCs in collection but not actually in the market.

# Version 1.48.0.7
- Changed MMWorsenPayupCond from percent of width to percent of tick. Internal payup price will be the worse of 
  (bestPrice +/- MMWorsenPayupCond%) and (midPoint +/- width).
- ICE GTCs correctly restore fill state on reload
- Market Maker wouldn't correctly quote ICE orders while CME gateway was down.
- Added support for CME GTCs
- When useGTC turned off, if MM is not enabled or cannot quote externally, do not put orders back into market.

# Version 1.48.0.6
- Moved RV MaxTicksWide parameter to PriceConfig
- Partial Candidate fix. If we can resolve the potential overfill then reset the candidate value, send a message to GUI
  and continue to quote the market. Otherwise, disable Market Maker.

# Version 1.48.0.5
- Rebase onto 1.47.0.21
- Log unsolicited cancels from ACE.
- Reset of GTCManagementGrid now cleans all cells.

# Version 1.47.0.21
- Do not disable MM external quoting if a rejected was caused by market closed.
- When banner expired, if market already closed do not put the quote back into IME.
- Add MaxTicksWide parameter in MM and RV.

# Version 1.48.0.4
- Rebase onto 1.47.0.20

# Version 1.47.0.20
- Default QuoteExternal to Disabled
- Bugfix: Fixed crash caused by invalid RVWidthMultiplier update. 
- Allow Market Maker to be enabled during gateway down time.
- Added log message to make sure startViper.sh script can start reliably.

# Version 1.47.0.19
- Add QuoteExternal switch in MainControl. Disable external quoting when market changed from NotTradable to Tradable.
- Bugfix: Fixed name of the contracts which not used in MM does not shown in market status message send to GUI.

# Version 1.47.0.18
- Updated logger with explicit 2 second timeout if MD component does not process a request queue packet (rhonecore only).

# Version 1.47.0.17
- Updated for ICE tag9707 changes. CME Session level reject should respect reconnect setting (rhonecore only).

# Version 1.48.0.3
- Added numGTCs column to GTC Management
- Many minor fixes to GTC Management pane
- Unsolicited cancel handled properly during ICE pre-open
- Duplicate OA event handled properly in OM

# Version 1.48.0.2.1
- Rebase onto v1.47.0.16
- Remove logger throttling to force Add(...) calls in logger at the potential expense of not handling FAK misses
- Tag 9700 sent on all order messages on ICE

# Version 1.47.0.16
- Disabled AVX instructions in binary

# Version 1.47.0.15
- Bugfix: Another rhone risk limit check scraping fix for all OMs.
- Bugfix: Fix infinite new order reject when disabling RV during market close. Do not sync orders if contract not
  tradable or gateway not active.
  
# Version 1.48.0.2
- Rebase onto Version 1.47.0.14
- Explicitly remove gtc from gtc collection on full fill before MarketMaker enabled.

# Version 1.47.0.14
- Bugfix: Rhone risk limit check fails for CME Gateway Containers.

# Version 1.47.0.13
- Disable MM when MD status changed from not tradable to tradeble; Increase Viper version sending delay

# Version 1.47.0.12
- Bugfix: Handle CME MD status in RV
- Changed SystemState to atomic because it is shared between multiple threads.
- Send MD status change to GUI
- If a calculated RV Hedge size is greater than Rhone max clip size limit, disable RV
- If price is null, skip negative quantity & order count checks on enabling MM

# Version 1.47.0.11
- Bugfix: The rounding error cause a huge residuals. Solution: Round vectorRisk to 2 decimal places in getResiduals function.
- Bugfix: When MM still hedge internally even though it has been disabled.
- Make ICE cancel resting configurable so that we can switch between old and new firm.
- Handle exchange state transition.

# Version 1.48.0.1
- Support for GTCs on ICE RoundRobin OMs.
- Changed colors in fill window.
- "Cancel X Levels" buttons in GTC Management now cancel relative to best working GTC price.
- Minor logging fix on GUI-driven GTC cancel.
- Bugfix: ICE "IsSMPUnsolicitedCancel" check returned opposite value
- Bugfix: "Cancel All Orders" button will cancel GTCs not yet assigned to an OM

# Version 1.48.0.0
- Added support for ICE GTCs.  Does not support RoundRobin OMs.
- Bugfix: A sweep event generated by the MD will cause MD Updates not to be sent until Book/Implied update generated in
          the same packet, or the next packet generates an update.

# Version 1.47.0.10
- Bugfix: Save staticQuote as Boolean.
- Bugfix: Fix Ice FAK cancel crashes MD feed

# Version 1.47.0.9
- Bugfix: Fixed negative manual fills shows as positives fill in Positions
- Bugfix: Fixed price in SMP unsolicited cancel message

# Version 1.47.0.8
- Bugfix: ICE SMP detection returned opposite value on check

# Version 1.47.0.7
- Swapped SMP handling on ICE from Cancel Aggressor to Cancel Resting

# Version 1.47.0.6.2
- RV disables if total quantity to hedge is greater than max clip size in Rhone.

# Version 1.47.0.6.1
- Bugfix: The rounding error cause a huge residuals. Solution: Round vectorRisk to 2 decimal places in getResiduals function. 

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

