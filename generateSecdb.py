from os import listdir, getcwd, remove
from os.path import isfile, join
import fileinput

def generateFile(configFile, lookupFile, refFile):
    contracts = {}
    for line in refFile:
        start = line.find(',')
        end = line.find(',', start + 1)
        contract = line[start+1:end]
        contracts[contract] = line
    processLine = False
    for line in configFile:
        if processLine:
            index = line.find(',')
            contract = line[0:index]
            if contract != '':
                dbLine = contracts[contract]
                lookupFile.write(dbLine)
        if line.startswith('PricingConfigs'):
            processLine = True

if __name__== "__main__":
    configFile = open('/home/pkusch/exito/viper/config/Q8H9_Live_Config.csv', 'r')
    refFile = open('/home/pkusch/exito/viper/config/iceus', 'r')
    lookupFile = open('secDb.txt', 'w')
    generateFile(configFile, lookupFile, refFile)
    configFile.close()
    lookupFile.close()
