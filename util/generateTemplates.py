from os import listdir, getcwd, remove
from os.path import isfile, join
import fileinput
import sys

def findFiles(directory, templatesFile):
    objects = listdir(directory)  
    for i in objects:  
        fullPath = join(directory, i)
        if not isfile(fullPath):
            findFiles(fullPath, templatesFile)
        else: 
            doLoggingWork(fullPath, templatesFile)

def doLoggingWork(filename, templatesFile):
    with open(filename, 'r') as file :
        filedata = file.read()
    
    global templateId
    index = 0
    startIndex = 0
    replacedData = ''
    while True:
        index = filedata.find('LOG(0', startIndex)
        if index == -1:
            #print "could not find coolog"
            replacedData += filedata[startIndex:len(filedata)]
            break;
        else:
            #print "found log: " + str(index)
            replacedData += filedata[startIndex:index+4] + str(templateId)

            endOfTemplate = filedata.find(',', index+6)
            template = str(templateId) + ':' +  filedata[index+8:endOfTemplate-1]
            #print template
            templatesFile.write(template)
            templatesFile.write('\n')

            templateId += 1
            startIndex = index + 5
    
    with open(filename, 'w') as file:
        file.write(replacedData)

templateId = 0

if __name__== "__main__":
    if len(sys.argv) != 2:
        sys.stderr.write("Missing argument (template file path)\n")
        exit(1)

    templatesFile = open(sys.argv[1], 'w')
    findFiles("./src", templatesFile)
    #findFiles("/home/pkusch/coolog/src", templatesFile)
    templatesFile.close()

