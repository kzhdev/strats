from __future__ import print_function
import vim
import sys
import time
import subprocess
from os import listdir
from os import path
from struct import unpack
from datetime import datetime
from calendar import timegm

def user_input(message):
    vim.command('call inputsave()')
    vim.command('let userInput = inputdialog("' + message + '")')
    vim.command('call inputrestore()')
    return vim.eval('userInput')

class LogViewer:
    def __init__(self):
        self.trades = ['back', 'brent', 'btc', 'fed_funds', 'wti_gtc', 'hogo_term', 'rates', 'wti', 'sim']
        self.homeDir = path.expanduser('~')
        self.isSim = path.exists(self.homeDir + "/sim")
        
        if self.isSim:
            with open(self.homeDir + "/sim/models/.exchange") as f:
                self.simViperDir = self.homeDir + "/sim/" + f.readline().strip() + "/viper/"
         
        self.trade = None
        self.contracts = []
        self.vectors = []
        self.currentLog = None
        self.templates = []
        self.filter = None
    
    def loadLog(self):
        if self.isSim:
            self.trade = 'sim'
        else:
            print('1. back')
            print('2. brent')
            print('3. btc')
            print('4. fed_funds')
            print('5. hogo_arb')
            print('6. hogo_term')
            print('7. rates')
            print('8. wti')
            while True:
                try:
                    index = int(user_input('Witch trade: [1-8] '))
                    if index < 1 or index > 8:
                        print('invalid selection.', index)
                    else:
                        break
                except KeyboardInterrupt:
                    return
                except:
                    print('invalid selection')
            print(' \n')
            self.trade = self.trades[index - 1]

        if self.isSim:
            with open(self.homeDir + "/sim/models/.exchange") as f:
                self.logDir = self.homeDir + "/sim/" + f.readline().strip() + "/viper/"
        else:
            self.logDir = self.homeDir + "/" + self.trade + "_logging/"
        
        self.contracts = []
        self.vectors = []
        self.currentLog = None
        self.templates = []

        self._loadTemplate()
        self._loadContracts()
        self._loadVectors()
        self._loadLogfiles()

        if len(self.logFiles) > 0:
            logFile = self.logFiles[len(self.logFiles) - 1]
            if self._openLogFile(logFile, True):
                self._readlogs()
        else:
            print('Nothing to load.', self.logDir, 'is empty.')

    def _openLogFile(self, logFile, createNewBuffer = False):
        log = self.filesDir + logFile
        timestamp = int(logFile[4:])
        logDatetime = int(timestamp / 1000000000)
        if 'log' in vim.current.buffer.vars.keys() and vim.current.buffer.vars['log'] == log:
            # log file is showing
            return False

        f = open(log, 'rb')
        if 'log' not in vim.current.buffer.vars.keys():
            if len(vim.current.buffer.name) > 0 or len(vim.current.buffer) > 1 or len(vim.current.buffer[0]) > 0:
                # current buffer not empty
                vim.command('tabnew /home/pythonsvc/logfiles/log.' + self.trade + '.' + time.strftime('%b%d.%H%M',time.localtime(logDatetime)))
            else: 
                #current buffer is empty, just use the buffer
                vim.command('file /home/pythonsvc/logfiles/log.' + self.trade + '.' + time.strftime('%b%d.%H%M',time.localtime(logDatetime)))
        else:
            if createNewBuffer:
                if self.trade in vim.current.buffer.vars['log']:
                    if len(vim.buffers) > 1:
                        vim.command('q!')
                        vim.command('tabnew /home/pythonsvc/logfiles/log.' + self.trade + '.' + time.strftime('%b%d.%H%M',time.localtime(logDatetime)))
                    else:
                        vim.current.buffer[:] = None
                else:
                    # trade changed, open a buffer for different trade
                    vim.command('tabnew /home/pythonsvc/logfiles/log.' + self.trade + '.' + time.strftime('%b%d.%H%M',time.localtime(logDatetime)))
                    
        vim.current.buffer.vars['log'] = log
        vim.current.buffer.vars['logDatetime'] = logDatetime
        vim.current.buffer.vars['logFile'] = logFile
        self.currentLog = f
        return True 

    def _readlogs(self):
        for i in range(10000): 
            addToBuffer = True
            header = self.currentLog.read(12)
            if not header: 
                return False
            timestamp, size = unpack('=QI', header)
            microsec = int((timestamp % 1000000000) / 1000)
            timestamp = '{}.{}'.format(time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(int(timestamp / 1000000000))), microsec) 
            entry = self.currentLog.read(7)
            if not entry: 
                return False
            _, templateId, nValues = unpack('=IHB', entry)
            assert size == 7 + nValues * 10
            b = True
            ints = [0]          #sequance start from 1, add dummy
            doubles = [0.0]     #sequance start from 1, add dummy
            if nValues == 0:
                if len(vim.current.buffer[0]) == 0:
                    vim.current.buffer[0] = timestamp + ' Invalid message. nValues = 0'
                else:
                    vim.current.buffer.append(timestamp + 'Invalid message. nValues = 0')
                continue

            for i in range(nValues):
                ty_seq = self.currentLog.read(2)
                if not ty_seq:
                    b = False
                    break;
                ty, seq = unpack('=BB', ty_seq)
                if ty == 0:
                    val = self.currentLog.read(8)
                    if not val:
                        b = false
                        break
                    ints.append(unpack('Q', val)[0])
                    assert seq == len(ints) - 1
                elif ty == 1:
                    val = self.currentLog.read(8)
                    if not val:
                        b = false
                        break
                    doubles.append(unpack('d', val)[0])
                    assert seq == len(doubles) - 1
                else:
                    print("Invalid value type", ty)
            log = self._genLog(timestamp, templateId, ints, doubles)
            if log is not None:
                if len(vim.current.buffer[0]) == 0:
                    vim.current.buffer[0] = log
                else:
                    vim.current.buffer.append(log)

            if not b:
                return False
        return True

    def _genLog(self, timestamp, templateId, ints, doubles):
        if templateId >= len(self.templates):
            return "Invalid template " + templateId
        template = self.templates[templateId];
        log = timestamp + ' '
        pos = template.find('{')
        filteredOut = False
        while pos != -1:
            log += template[0:pos]
            if template[pos + 1] == 'C':
                index = int(template[pos + 4: template.find(']')])
                if index >= len(self.contracts):
                    log += 'missing_contract=' + str(index)
                else:
                    contract = self.contracts[ints[index]]
                    log += contract
                    if self.filter is not None:
                        if contract != self.filter:
                            filteredOut = True
                            break
            elif template[pos + 1] == 'V':
                index = int(template[pos + 4: template.find(']')])
                if index >= len(self.vectors):
                    log += 'missing_vector=' + str(index)
                else:
                    log += self.vectors[index]
            elif template[pos + 1] == 'i':
                index = int(template[pos + 2: template.find('}')])
                if index >= len(ints):
                    log += 'missing_int=' + str(index)
                else:
                    log += str(ints[index])
            elif template[pos + 1] == 'd':
                index = int(template[pos + 2: template.find('}')])
                if index >= len(doubles):
                    log += 'missing_double=' + str(index)
                else:
                    log += str(doubles[index])
            else:
                log += 'invalid_value_type=' + template[pos + 2 : (template.find('}') + 1)]
            template = template[(template.find('}') + 1):]
            pos = template.find('{')
        if filteredOut:
            return None
        log += template
        return log

    def loadMore(self):
        if 'log' not in vim.current.buffer.vars.keys():
            return
        if float(vim.current.window.cursor[0]) / len(vim.current.buffer) > 0.66:
            if not self._readlogs():
                self._loadLogfiles()
                index = self.logFiles.index(vim.current.buffer.vars['logFile'])
                if index != -1:
                    if (index + 1) <= len(self.logFiles) - 1:
                        if self._openLogFile(self.logFiles[index + 1]):
                            self._readlogs()

    def _loadLogfiles(self):
        if self.isSim:
            pass
        else:
            self.filesDir = self.logDir + "files/0/"
            self.logFiles = [f for f in listdir(self.filesDir) if path.isfile(path.join(self.filesDir, f))]
            self.logFiles.sort()

        if len(self.logFiles) == 0:
            print("No log file exists in", self.filesDir)

    def _loadTemplate(self):
        if self.trade is None:
            #print("Error: LoadLog <trade> need to be called first")
            return
        if self.isSim:
            with open(self.homeDir + "/sim/bin/templates.txt") as f:
                lines = f.readlines()
        else:
            with open(self.logDir + "config/templates.txt") as f:
                lines = f.readlines()
        for line in lines:
            self.templates.append(line[(line.find(':') + 1):])

    def _loadContracts(self):
        if self.trade is None:
            #print("Error: LoadLog <trade> need to be called first")
            return

        if not self.isSim:
            with open(self.logDir + "/config/contracts.txt") as f:
                for line in f.readlines():
                    parts = line.split(":")
                    contract = parts[1].strip()
                    if len(contract) > 0:
                        self.contracts.append(contract)

    def _loadVectors(self):
        if self.trade is None:
            #print("Error: LoadLog <trade> need to be called first")
            return

        if not self.isSim:
            with open(self.logDir + "/config/factors.txt") as f:
                for line in f.readlines():
                    parts = line.split(":")
                    v = parts[1].strip()
                    if len(v) > 0:
                        self.vectors.append(v)

    def showTemplate(self):
        if self.trade is None:
            #print("Error: LoadLog <trade> need to be called first")
            return

        templatePath = "/" + self.trade + "_logging/config/templates.txt" 

        if templatePath in vim.current.buffer:
            return;

        for b in vim.buffers:
            if templatePath in b.name:
                vim.current.buffer = b
                return
        vim.command('tabnew ' + self.homeDir + templatePath)
        vim.current.buffer.vars['child'] = True

    def showContracts(self):
        if self.trade is None:
            #print("Error: LoadLog <trade> need to be called first")
            return
        
        vim.command('tabnew')
        i = 1
        for c in self.contracts:
            vim.current.buffer.append(str(i) + ':' + c)
            i += 1
        del vim.current.buffer[0]
        vim.current.buffer.vars['child'] = True

    def close(self):
        if 'child' in vim.current.buffer.vars.keys():
            vim.command('q!')

    def onBufDelete(self):
        if 'log' not in vim.current.buffer.vars:
            return

        self.currentLog.close()
        self.currentLog = None

            
    def jumpTo(self):
        if self.trade is None:
            #print("Error: LoadLog <trade> need to be called first")
            return
        assert 'logDatetime' in vim.current.buffer.vars
        date = time.strftime("%Y-%m-%d ", time.localtime(vim.current.buffer.vars['logDatetime']))
        print('        YYYY-MM-DD HH:MM:SS')
        try:
            dt = user_input('JumpTo  ' + date)
            if dt == '':
                return
            utc_dt = subprocess.check_output('date -d "'+ date + dt + '" +@%s | xargs date -u +"%Y-%m-%d %H:%M:%S" -d', shell=True)
            utc_time = time.strptime(utc_dt.strip(), "%Y-%m-%d %H:%M:%S")
            epoch_time = timegm(utc_time)
        except KeyboardInterrupt:
            return
        except:
            print(' \nTime invalid')
            return
        logFile = self._findLogFileForGivenTime(epoch_time)
        if len(logFile) == 0:
            print('No log file contains specified time',date,dt)
            return
        print(" \nOpening log file:", self.logDir+logFile, '(', time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(int(int(logFile[4:]) / 1000000000))), ')', end="\nSearching " + date + dt + '...')
        if self._openLogFile(logFile, True):
            self._readlogs()
        lineNumber = self._searchTime(date + dt)
        if lineNumber == -1:
            print('No log file contains specified time',date,dt)
        else:
            vim.current.window.cursor = (lineNumber + 1, 0)

    def _findLogFileForGivenTime(self, dt):
        preLog = ''
        self._loadLogfiles()
        for l in self.logFiles:
            timestamp = int(int(l[4:]) / 1000000000)
            if dt <= timestamp:
                if len(preLog) == 0:
                    return l
                return preLog
            preLog = l
        return preLog
        

    def _searchTime(self, dt):
        currentLine = vim.current.window.cursor[0] - 1 
        lowerBound = None
        upperBound = None
        reachedEnd = False
        while True:
            if dt <= vim.current.buffer[currentLine][:19]:
                if lowerBound is not None:
                    for i in range(lowerBound, currentLine + 1):
                        if dt <= vim.current.buffer[i][:19]:
                            return i
                    break;

                upperBound = currentLine
                if currentLine <= 10000:
                    for i in range(currentLine):
                        if dt <= vim.current.buffer[i][0:19]:
                            return i
                    return -1  # should never come here
                else:
                    currentLine -= 10000
                    continue
            else:
                if upperBound is not None:
                    for i in range(currentLine, upperBound):
                        if dt >= vim.current.buffer[i][:19]:
                            return i

                while True:
                    if not self._readlogs():
                        reachedEnd = True

                    last = len(vim.current.buffer) - 1
                    if dt > vim.current.buffer[last][:19]:
                        if reachedEnd:
                            return last 
                        lowerBound = last
                    else:
                        currentLine = last
                        break
     
    def filterContract(self):
        if self.trade is None:
            #print("Error: LoadLog <trade> need to be called first")
            return
        try:
            contract = user_input("Which contract (Enter to clear existing filter): ")
        except KeyboardInterrupt:
            return

        if len(contract) == 0:
            self.filter = None
            self._reloadlog()
            return

        if contract not in self.contracts:
            print(" \nInvalid contract", contract)
            return

        self.filter = contract
        self._reloadlog()
    
    def _reloadlog(self):
        if self.currentLog is not None:
            vim.current.buffer[:] = None
            self.currentLog.seek(0)
            self._readlogs()

    def openPreviousLogFile(self):
        if self.trade is None:
            return

        if 'logFile' not in vim.current.buffer.vars:
            return

        if len(self.logFiles) == 0:
            return

        if vim.current.buffer.vars['logFile'] == self.logFiles[0]:
            print('This is the first log file')
            return

        index = self.logFiles.index(vim.current.buffer.vars['logFile'])
        if index == -1:
            print('Error:', vim.current.buffer.vars['logFile'], 'does not exist')
            return
        
        assert index > 0
        if self._openLogFile(self.logFiles[index - 1]):
            self._readlogs()

