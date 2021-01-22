#Version 1.52.9.2
- Merge rhonepublic 4.1.1 changes into Viper rhonepublic branch
- Hedge price in marketmaker uses better of calculated hedge price or market midpoint

#Version 1.52.9.1
- [bugfix] %EdgeSTF/%EdgeMkt off by ~10^8

#Version 1.52.9.0
- Add AutoRVDisableTime and AutoMMDisableTime in global config

#Version 1.52.8.0
- Removed "enabled" column from StackerParameters
- When sending new orders due to "failed decrement block new orders" is unblocked, attempt to use the
  unblocked order to hedge via a modify if it would be cancelled.
- [bugfix] On a fill, target quantity is reduced on the final level after a modify chain if exists.
- [bugfix] Fix MM sprayer flicker and not adding new sprayer order in some scenario.
- MM WorsePayupCondition must be greater than 0 now.
- [bugfix] Recalculate MM hedge price if either side TOB changed.  
- [bugfix] update MM hedge price after updating quotes on both side.
- MM evaluated price by orders always stay on the target price.
- When one side MM quote update did not sent due to minInternalPriceChange guarded, if the other side change 
  causes internal price cross, MM will force send quote update on both side.
- [bugfix] When MM hedge switched side in 2 different fill transaction, removing old hedge and the new hedge might be 
  processed by IME in same process. Since removing old hedge has older execTransaction id, it will be ignored by
  IME which will end up having hedge order on both side and cross each other.
- [bugfix] Fix B3 swapping trader causes ResendRequest. (RhonePublic)
- [rhonepublic] Removing levels from CME book fixed for ilink3 due to new reject code
- [rhonepublic] Ice sweep detector had false positives if first trade of a transaction was not in the first packet of a transaction

#Version 1.52.7.2
- [bugfix] When MM took partial overfill, it sent entire overfill size to Front incorrectly, which caused PnL calculation issue.

#Version 1.52.7.1
- Total PnL Breech will only disable non-MM strategies and quote external for non-MM strategies
- Min qty on FAK B3 orders changed to 5
- B3 FAK Order type implemented in RhonePublic

#Version 1.52.7.0
- Changed MM Sprayer implementation

#Version 1.52.6.4
- [bugfix] Certain Stacker and GTC Grid GUI updates could cause a crash in the RV width tightening check
- [bugfix] Fix B3 overfill protection not decreased properly when order modify confirmed.

#Version 1.52.6.3
- Reject Shift change if RV width tightens up by more than 0.5
- Fix stacker spray even when everything disabled

#Version 1.52.6.2
- Implement MM order execution token

#Version 1.52.6.1
- Manual fills properly are reflected in PnL calculations
- All manual fills in the GUI and KDB are marked as Internal, even if they have external allocation

#Version 1.52.6.0
- Added B3 support

#Version 1.52.5.3
- Manual fills properly are reflected in PnL calculations

#Version 1.52.5.2
- Add ICE decoder that cannot generate fills based off of Trade events

#Version 1.52.5.1
- [bugfix] Manual fill prices use midpoint if able
- If autoShutdownTime is at or before 17:00 on a Sunday, advance time one day

#Version 1.52.5.0
- Implemented MM Sprayer. quote%TOB removed from stacker parameters. quoteGap added to MM parameters.

#Version 1.52.4.8
- Manual fills properly are reflected in PnL calculations

#Version 1.52.4.7
- Add ICE decoder that cannot generate fills based off of Trade events

#Version 1.52.4.6
- [bugfix] Manual fill prices use midpoint if able
- If autoShutdownTime is at or before 17:00 on a Sunday, advance time one day

#Version 1.52.4.5
- Add manual fills to GUI and KDB
- [bugfix] rvStf value in KDB off by Rhone Price Factor
- Log NegativeScalpCheck performance to rhone logger

#Version 1.52.4.4
- [bugfix] Queue position code no longer ignores GTCs
- Fills in KDB "DI" information now store prices as doubles instead of ints

#Version 1.52.4.3
- Increased read buffer size to 20 MiB
- Added optional logging of fills

#Version 1.52.4.2
- [bugfix] fix random strategy disabling caused by uninitialized flag
- When fillRate/orderRate checker breached, do not disable MM and MM ext quoting.
- Try to offset positions first when MM takes overfills.
- Send fills to FrontSize for negative scalp checking right away when fills received.

#Version 1.52.4.1

- Disable UseGTC when contract negative scalp checker breached
- [bugfix]: Fix MD level change index tracking

#Version 1.52.4.0
- Implemented negative scalp checker per contract.
- Exclude ICE implied order from queue position tracking.

#Version 1.52.3.2 
- Mark implied qty in md book update
- [bugfix]: Fixed OM send multiple hedge orders if the contract has any unadopted GTC orders
- When fillRateChecker/orderRateChecker over limit, disable all strategies 

#Version 1.52.3.1
- Added additional check to failed decrease quantity in OM before notifying user
- [bugfix] Capture Matrix Inverse exception. Revert parameter, disable RV and alert GUI.
- Eurex OM updated to ETI v8.1
- Added a safety check to avoid using invalid iterator in ICE queue position tracking.

#Version 1.52.3.0
- Updated MarketMaker to calculate quote price and size based on orders' queue position.
- [bugfix] Fix MM EstablishedLevel not updating

#Version 1.52.2.17
- Call OM onMDUpdate only when market data changed
- Fix Logging
- log unsolicited onMDFirmOrderExecutionEventBeforeFillArbitration callback and notify to GUI

#Version 1.52.2.16
- Updated to Rhone v4.0.3 including CME MassOrderStatus timeout retry [rhonepublic only]

#Version 1.52.2.15
- Reduced amount of logging on GTC recovery

#Version 1.52.2.14
- [bugfix] Multiple ICE gateways started at the same time get assigned the same prefix (rhonepublic only)

#Version 1.52.2.13
- Added logging and notification for MDUpdates that have invalid index

#Version 1.52.2.12
- [bugfix] Fix MM established level sent updating GUI (cherry-pick)

#Version 1.52.2.11
- Ignore leg fills on GTC Fill Recovery
- Ignore MarketBookUpdates with invalid change indicies.

#Version 1.52.2.10
- [bugfix]: Fixed OM send multiple hedge orders if the contract has any unadopted GTC orders
- When fillRateChecker/orderRateChecker over limit, disable all strategies 

#Version 1.52.2.9 (rhonepublic only)
- Merged latest 4.0.0 changes into rhonepublic branch
- ICE GTCs send invalid checksums after swapTrader

#Version 1.52.2.8
- Eurex OM updated to ETI v8.1 (rhonepublic only)

#Version 1.52.2.7
- [bugfix] Leavesqty on orders set incorrectly during GTC fills before enabling strategies

#Version 1.52.2.6
- FAK misses that erase implied orders in CME market data handling code no longer touch QueuePositionUtils (rhonepublic only)

#Version 1.52.2.5
- [bugfix] Sweep detector parameters update correctly
- [bugfix] Rhonepublic queue priority code respects "shouldErase" flag in deleteOrder function
- Sweep detector logic moved to RV
- Eurex sweep detection changed from log-only to fully enabled

#Version 1.52.2.4
- Improved AutoShutdownTime GlobalConfig. 1. Changed the time th HHMMSS format. 2. Disable strategies before shutdown Viper.
- ICE Sweep Detector Implementation
- [bugfix] Only remove order from queue position tracker if marked for deletion
- [bugfix] Sweep events no longer ignored by Viper

#Version 1.52.2.3
- Implied Orders on CME are not added to the queue position tracking collections

#Version 1.52.2.2
- [bugfix] If a quote overrides a banner, a new quote banner is created

#Version 1.52.2.1
- [bugfix] Revert RhoneLogger change made in 1.52.1.3 which caused Viper not receiving order responses

#Version 1.52.2.0
- Add AutoShutdownTime GlobalConfig
- [bugfix] Fix MM established level EMA become to 0.
- Build With GCC9 with debug std contianer

#Version 1.52.1.6
- [bugfix] RhoneSimulator would overflow price calculations for some CME products

#Version 1.52.1.5
- [bugfix] EMA On/Off incorrectly parsed and written to config file if disabled
- [bugfix] Queueposition iterator not checked vs. end

#Version 1.52.1.4
- Implemented EMAOnTime and EMAOffTime in FactorParameter. EMA will be turned on/off at the specified time.
- Implemented MM EstablishedLevelEMA. Added EstLevelEMASeconds and EstLevel%EMA columns in MarketMaker parameter.
- Eurex will wait until NO Ack when a cancel is in flight with a new order before sending new orders
- [bugfix] Market Data batches were not properly cleared by logger

#Version 1.52.1.3
- RhoneLogger will no longer zero out events before MD components have a chance to process
- Added EFVI viper handlers

#Version 1.52.1.2
- Disabling quote sprayer puts the correct quantity at the top level
- Various bad memory accessor fixes discovered via asan

#Version 1.52.1.1
- Rename Release shift to SafeMode

#Version 1.52.1.0
- Added a column for each shift in global config
- [bugfix]: Fix a sprayer issue where when market moved away, it keeps adding orders in the back

#Version 1.52.0.4
- Added 'enableQuoteSprayer' parameters to Stacker Config
- Disallow values <1 for Stacker QuoteGap parameter

#Version 1.52.0.3
- On start up, send MM net positions as phantom fills to Front so that MM PNLs shows correctly. 
- Minor cleanup of Transaction Capacity code
- Added "Release Width" config parameter
- Better error reporting for bad config files

#Version 1.52.0.2
- [bugfix]: Fix ICE recovered GTC order queue position update issue.
- [bugfix]: Fix top level quote size changed to maxQuote when toggling UseGTC

#Version 1.52.0.1
- [bugfix]: Fix some of recovered GTC orders queue position not updating.
- [bugfix]: Fix an issue where after Viper restarted, a new order put in behind recovered GTC order even though the gap
  requirement is not reached.

#Version 1.52.0.0
- Subscribe 10 levels market data update
- Enable queue position update
- Implement QuoteGap, maxQuote and quote%TOB Stacker Parameter.

#Version 1.51.2.4
- [bugfix]: Fixed badly formatted boost format and Template strings.
- Added handling for Eurex status to RV

#Version 1.51.2.3
- [bugfix]: Allow traders to be swapped on Eurex in Rhone4

#Version 1.51.2.2
- Merge Rhone 4.0.0 update. Fix CME GTC recovery issue.

#Version 1.51.2.1
- [bugfix]: Fix an issue where re-enable MM will not put stack back if the external size is 0 and the price hasn't
  changed.
- Handle LockFreeQueue reset properly.
- If there is a gapped market, Market Maker will use a Simple Average instead of a Weighted Average for pricing.

#Version 1.51.2.0
- Added support for Eurex to Viper
- ICEFodHandler was not incrementing the read count on an empty batch and
  was increasing the read count on a to-be-written batch.

#Version 1.51.1.1
- Merge fix from 1.50.1.29

#Version 1.51.1.0
- Merge ViperLogger change from 1.50.1.22

#Version 1.51.0.4
- Avoid logging debugInfo of viper logs into kdb.

#Version 1.51.0.3
- Always accept spread log files. (required by rhone)
- Reduce IME/OM logging
- Notify recovered filly fill only if the contract is to market make

#Version 1.51.0.2
- Mute "Notify order request log message"

#Version 1.51.0.1
- Pick up RhoneLogger log rotation fix from viper-1.42.3 branch.

#Version 1.51.0
- Port to Rhone 4.0.0

#Version 1.50.1.30
- [bugfix]: Fix an issue where re-enable MM will not put stack back if the external size is 0 and the price hasn't
  changed.
- Handle LockFreeQueue reset properly.
- If there is a gapped market, Market Maker will use a Simple Average instead of a Weighted Average for pricing.

#Version 1.50.1.29
- [bugfix]: Fix quote flickering caused by banner. When banner expired, if persisted price is better than top price
  level, use the persisted price and 0 lot to quote, otherwise use the top level price and size to quote.
- [bugfix]: When MM take overfills, it should only report the size it took to Front.  

#Version 1.50.1.28
- Fix logger not write the logs to viper log queue.

#Version 1.50.1.27
- When orderReduce level target when order filled
- Changed fill allocation based on MM orders in IME.
- When MM no longer want the external quote (externalSize == 0 and externalPrice != NULL_PRICE), if MM external price
  is better than RV, stacker should not re-stack based on RV quote, it should reserve current stacks.

#Version 1.50.1.26
- Create viper logger queue when viper connected to ViperLogger

#Version 1.50.1.25
- Merge of 1.50.1.24 and 1.50.1.23 changes

#Version 1.50.1.24
- Fix logger stuck

#Version 1.50.1.23 [Does not include 1.50.1.22 changes]
- Changed coloring of GTC "through" columns to not color if on best level
- Updated to throughBid/throughAsk definitions

#Version 1.50.1.22
- Implemented ViperLogger

#Version 1.50.1.21
- [bugfix] Fixed a macro to log more GTC grid information that was preventing proper template generation
- [bugfix] Removed throughBid/throughAsk change in 1.50.1.20
- Added significantly more GTCGrid logging

#Version 1.50.1.20
- [bugfix] Fixed an issue where throughBid/throughAsk could be swapped
- Log all GTC messages to the GUI to debug preopen issues

#Version 1.50.1.19
- When sweep detected only disable RV and RV external quoting.

#Version 1.50.1.18
- [bugfix] Fix MM self match issue. Prevent IME from adding order with NULL_PRICE into internal map.
- When MM maxTicksWide breached, send order with 0 size and NULL_PRICE to IME only once.

#Version 1.50.1.17
- Added gate on creation and modification of orders to explicitly check enabled state

#Version 1.50.1.16
- [RhoneLogger] Make sure log folder config has trailing "/"; Change RhoneLogger either write log to disk or send through
  multicast
- [bugfix] Fix rhone logger not creating new file when file size limit reached

#Version 1.50.1.15
- Added Quote%TOB parameter to limit max MM quote size to percentage of best level
- [bugfix] Fixed "TicksThoughBid" and "TicksThroughAsk" column values being swapped
- MMWorseThanInstrument% made per-instrument instead of global

#Version 1.50.1.14
- Add a protection to prevent infinite self match loop. Viper will shutdown if self match reached 50 times.
- [RhoneLogger] Add config option to write Viper log directly to disk.

#Version 1.50.1.13
- Fix RV factor position calculation issue.

#Version 1.50.1.12
- Fix rhonelogger crash

#Version 1.50.1.11
- Fix rhonelogger message corruption

#Version 1.50.1.10
- FilLRateChecker: Do not count internal fills
- FillRateChecker: Adjust for missed FAK hedges
- FillRateChecker: Shortened breech error message to prevent truncation in GUI
- GTC Grid: Refactored "ticksthrough" code to properly handle no-IOP cases
- GTC Grid: Fixed a bug where CME IOP price would sometimes be incorrect
- Added Order rate throttler
- Disallow negative counts and sumTotalQuantities in Fill/Order rate throttlers
- On RV Disable due to MaxTicksThrough, disable RV (same as previous version), 
  disable RV External Quote (different from previous version), do not disable exchange
  external quote (different from previous version) 

#Version 1.50.1.9
- Do not disable Market Maker if fill rate breached

#Version 1.50.1.8
- [bugfix] Removed trace logging from build

#Version 1.50.1.7
- Fix MM manual fill not send out hedge until hedge price changed.

#Version 1.50.1.6
- Added total fill qty to fill rate checker
- Consolidated non-error non-fill GTC reload messages into a single message
- Updated LogFileGenerator to try to recover if bad data is encountered
- Fill rate blocker and Negative Scalp Checker will disable MM, RV, MM External Quote, RV External Quote,
  and all exchange external quoting.
- While in a MD gap, strategies cannot be enabled (used to be RV blocked, other strategies were could enable)
- When a MD gap is detected, in addition to disabling all strategies (current behaviour), disable ext quote for *all* venues.
- Added color coding to 'BidCross' and 'AskCross' in RV Parameters
- Kill viper if started with GTCs that cannot be controlled.  Allow explicit override in config.
- Several PnL Changes.  In general, allow MM to always be enabled.  All quote external and all other strategies checked
  vs. strategy and global pnl.

#Version 1.50.1.5
- Fix MM hedge size might not be updated issue.
- Fix RV maxTicksWide only checked for pricing contract on MD update.

#Version 1.50.1.4
- Suppress "Fill received while MM disabled." message for manual fills.
- Check quote internal price again NULL_PRICE in onChangeInOtherBook.

#Version 1.50.1.3
- Notify GUI of end of MD gap
- RV cannot be enabled during md gap.  QuoteExternal cannot be enabled during MD gap.
- Added fill rate checker

#Version 1.50.1.2
- Merge ime branch to maste
- change rhonelogger throttle_time config to ms (rhone)

#Version 1.50.1.1
- Fix DisableRelativeValue function not sending state update to GUI.

#Version 1.50.1
- Add strategy quote external switch in main control.
- Add color in main control switch grid.

#Version 1.50
- Implement new internal matching engine.

#Version 1.49.7
- Added Shift in TraderLogin. 
- In MarketMakerParameter, changed MMWidthPercent to readonly, added AsiaWidthPercent, EuroWidthPercent and
  USWidthPercent. MMWidthPercent will be updated according to the shift.

#Version 1.49.6.1
- Fix gateway login and status tracking issue

#Version 1.49.6.0
- Store RV positions in separate file
- Only allow one GUI to be connected at a time

#Version 1.49.5.6
- Fix OMQueue creation race problem (rhonepublic)

#Version 1.49.5.5
- Updated leavesqty correctly if fill occurs after GTC reload but before MarketMaker enabled

#Version 1.49.5.4
- Fix RhoneLogger stop sending log messages after throttling
- Fix Viper crash caused by invalid tickSize

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

