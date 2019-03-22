from os import listdir, getcwd, remove
from os.path import isfile, join
import fileinput

def generateFile(configFile, lookupFile):
    processLine = False
    contractIndex = 0
    for line in configFile:
        if processLine:
            index = line.find(',')
            entry = str(contractIndex) + ':' +  line[0:index]
            lookupFile.write(entry + '\n')
            contractIndex += 1
        if line.startswith('PricingConfigs'):
            processLine = True

if __name__== "__main__":
    configFile = open('/home/jconnell/produtil/models/model.csv', 'r')
    lookupFile = open('contracts.txt', 'w')
    generateFile(configFile, lookupFile)
    configFile.close()
    lookupFile.close()
