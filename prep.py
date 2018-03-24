from os import listdir, getcwd, remove
from os.path import isfile, join
import fileinput

def findFiles(directory):
    templatesFile = open('templates.txt', 'w')
    objects = listdir(directory)  
    for i in objects:  
        fullPath = join(directory, i)
        if not isfile(fullPath):
            findFiles(fullPath)
        else: 
            doLoggingWork(fullPath, templatesFile)

def doLoggingWork(filename, templatesFile):
    templatesFile = open('templates.txt', 'a')

    with open(filename, 'r') as file :
        filedata = file.read()
    
    global templateId
    index = 0
    startIndex = 0
    replacedData = ''
    while True:
        index = filedata.find('COOLOG(0', startIndex)
        if index == -1:
            replacedData += filedata[startIndex:len(filedata)]
            break;
        else:
            replacedData += filedata[startIndex:index+7] + str(templateId)
            endOfTemplate = filedata.find(',', index+9)
            template = str(templateId) + ': ' +  filedata[index+10:endOfTemplate]
            templatesFile.write(template)
            templatesFile.write('\n')
            templateId += 1
            startIndex = index + 8
    templatesFile.close()

    with open(filename, 'w') as file:
        file.write(replacedData)

templateId = 1

if __name__== "__main__":
    findFiles("/home/pkusch/exito/viper/src")

