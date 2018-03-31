#! /bin/sh

rm templates.txt
rm contractsLookup.txt
rm vectorsLookup.txt
python generateTemplates.py
python generateContractsLookup.py
python generateVectorsLookup.py
