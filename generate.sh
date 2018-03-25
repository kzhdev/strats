#! /bin/sh

rm templates.txt
rm contractsLookup.txt
python generateTemplates.py
python generateContractsLookup.py
