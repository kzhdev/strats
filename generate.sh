#! /bin/sh

echo Script name: $0
echo $# arguments

if [ $# -ne 1 ] 
then 
     echo "Missing: TEMPLATES_DIR"
else

rm templates.txt
python generateTemplates.py
ssh pkusch@corlsp003 "mkdir /home/pkusch/templates/$1"
scp templates.txt pkusch@corlsp003:/home/pkusch/templates/$1

rm -rf /home/pkusch/exito/build/*
rm -rf /home/pkusch/exito/viper/bin/*

cd /home/pkusch/exito/build
cmake -DCMAKE_BUILD_TYPE=Release ../viper
make -j20

cd /home/pkusch/exito/viper
git reset --hard HEAD

mv /home/pkusch/exito/viper/bin/RhoneDs /home/pkusch/exito/viper/bin/RhoneDs_1.$1

fi
