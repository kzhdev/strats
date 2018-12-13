#! /bin/sh

rm templates.txt
rm contractsLookup.txt
rm vectorsLookup.txt
python generateTemplates.py
python generateContractsLookup.py
python generateVectorsLookup.py
ssh pkusch@corlsp003 "mkdir /home/pkusch/templates/$1"
scp templates.txt pkusch@corlsp003:/home/pkusch/templates/$1
