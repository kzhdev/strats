from os import listdir, getcwd, remove
from os.path import isfile, join
import fileinput

def generateFile(configFile, lookupFile, refFile, mdsFile):
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
                if contract in contracts:
                    dbLine = contracts[contract]
                    lookupFile.write(dbLine)
                    endIdIndex = dbLine.find(',')
                    ID = dbLine[0:endIdIndex]
                    mdsLine = ID + ',ICE_Futures_Brent\n'
                    mdsFile.write(mdsLine)
                else:
                    print 'did not find contract: ' + contract
        if line.startswith('PricingConfigs'):
            processLine = True

if __name__== "__main__":
    configFile = open('/home/pkusch/exito/viper/config/US_Rates_H19.csv', 'r')
    refFile = open('/home/pkusch/exito/viper/config/secdb.cme_msgw.csv', 'r')

    lookupFile = open('secDb.txt', 'w')
    lookupFile.write('gid,sym,tick,tick_value,activate,cme_sym,cme_sid,cme_exch,cme_prod,cme_group,cme_depth,cme_impl_depth,cme_df,cme_md,cme_om,ice_sym,ice_sid,ice_denominator,ice_md,eurex_sym,eurex_sid,eurex_mid,eurex_denominator,eurex_md,cme_segmentid,eurex_nolegs,eurex_partitionid,ice_market_type,euronext_sym,euronext_sid,euronext_symbol_sid,euronext_price_decimals,euronext_qty_decimals,euronext_amount_decimals,euronext_md_channel,product,details,ice_deal_denominator\n')
    
    mdsSymbolsFile = open('mds_pp_symbols.csv', 'w')
    mdsSymbolsFile.write('gid,md\n')

    generateFile(configFile, lookupFile, refFile, mdsSymbolsFile)
    configFile.close()
    lookupFile.close()
    mdsSymbolsFile.close()
