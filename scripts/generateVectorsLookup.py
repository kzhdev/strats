from os import listdir, getcwd, remove
from os.path import isfile, join
import fileinput

def generateFile(configFile, lookupFile):
    processLine = False
    contractIndex = 0
    for line in configFile:
        if line.startswith('RVConfigs'):
            break;
        if processLine:
            index = line.find(',')
            entry = str(contractIndex) + ':' +  line[0:index]
            lookupFile.write(entry + '\n')
            contractIndex += 1
        if line.startswith('FactorConfigs'):
            processLine = True

if __name__== "__main__":
    configFile = open('tmp/model.csv', 'r')
    lookupFile = open('tmp/factors.txt', 'w')
    generateFile(configFile, lookupFile)
    configFile.close()
    lookupFile.close()
