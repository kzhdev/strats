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

