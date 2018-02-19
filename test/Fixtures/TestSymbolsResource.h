#ifndef VIPER_TESTSYMBOLSRESOURCE_H
#define VIPER_TESTSYMBOLSRESOURCE_H

#include <hf/hf.h>
#include <vector>

namespace Python {

class TestSymbolsResource {
public:
    const hf::symbol_info_t& getSymbol(hf::symbol_id_t id) const {
        auto it = _sidToSymbolInfo.find(id);
        if (it != _sidToSymbolInfo.end()) {
            return it->second;
        }
        throw std::runtime_error("unknown sid");
    }

    hf::symbol_id_t findSymbol(const std::string &symbol) const {
        auto it = _symbolToSid.find(symbol);
        if (it != _symbolToSid.end()) {
            return it->second;
        }
        throw std::runtime_error("unknown symbol");
    }

    void addContract(const std::string& name, hf::VENUE venue, hf::price_t tickSize, std::vector<hf::symbol_leg_info_t>& legs) {
        _symbolToSid.insert(std::pair<std::string, hf::symbol_id_t>(name, _addContractCount));
        hf::symbol_info_t info;
        info.sid = _addContractCount;
        info.gid = _addContractCount;
        info.tick_size = tickSize;
        info.enabled = true;
        info.legs = legs;
        setVenue(info, venue);
        _sidToSymbolInfo.insert(std::pair<hf::symbol_id_t, hf::symbol_info_t>(_addContractCount, info));
        ++_addContractCount;
    }

    void setVenue(hf::symbol_info_t& info, hf::VENUE venue) {
        switch(venue) {
            case hf::VENUE::CME: {
                info.cme.enabled = true;
                break;
            }
            case hf::VENUE::ICE: {
                info.ice.enabled = true;
                break;
            }
            case hf::VENUE::EURONEXT: {
                info.euronext.enabled = true;
                break;
            }
            case hf::VENUE::EUREX: {
                info.eurex.enabled = true;
                break;
            }
            default:
                break;
        }
    }

    void populateLegs(std::vector<hf::symbol_leg_info_t>& legs, const std::string& positive, const std::string& negative) {
        legs.clear();
        hf::symbol_id_t sid = findSymbol(positive);
        const hf::symbol_info_t* info = &getSymbol(sid);
        legs.push_back({info->gid, +1});
        sid = findSymbol(negative);
        info = &getSymbol(sid);
        legs.push_back({info->gid, -1});
    }

    std::unordered_map<std::string, hf::symbol_id_t> _symbolToSid;
    std::unordered_map<hf::symbol_id_t, hf::symbol_info_t> _sidToSymbolInfo;

    uint32_t _addContractCount = 0;
};

}

#endif //VIPER_TESTSYMBOLSRESOURCE_H
