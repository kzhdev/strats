- Pre-allocate space for FAK adjustment trackers
- Increase om::ORDER_NUM to 6M, forced some objects to be heap-allocated to fit within binary image.

# Version 1.46.1.2 #
- Can set RVQuoteMaxPercentWidth (now called %stfMove) per-contract

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

