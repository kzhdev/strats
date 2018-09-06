#ifndef RHONEDS_LOGREADER_H
#define RHONEDS_LOGREADER_H

#include <hf/hf.h>
#include <unordered_map>
#include <string>
#include <map>
#include <fstream>
#include <dirent.h>
#include <cstdio>
#include <boost/lexical_cast.hpp>
#include <boost/date_time.hpp>
#include <hf/util/TimeUtils.h>
#include "../../rhonepublic/rhonelogging/src/PythonLogging.h"

enum LogPartType {String, Contract, Factor, Int, Double};

struct LogPart {
    LogPartType _type;
    uint32_t _id;
    std::string _data;
};

struct Log {
    std::vector<LogPart> _parts;
};

namespace bt = boost::posix_time;

std::time_t pt_to_time_t(const bt::ptime& pt) {
    bt::ptime timet_start(boost::gregorian::date(1970,1,1));
    bt::time_duration diff = pt - timet_start;
    return diff.ticks() / bt::time_duration::rep_type::ticks_per_second;

}
std::time_t seconds_from_epoch(const std::string& s) {
    std::locale timeFormat(std::locale::classic(), new bt::time_input_facet("%Y-%m-%d-%H:%M:%S"));
    std::istringstream is(s);
    is.imbue(timeFormat);
    bt::ptime pt;
    is >> pt;
    return pt_to_time_t(pt);
}

hf::nanos_t getTimeInNanos(char* s) {
    return hf::util::TimeUtils::nanos(seconds_from_epoch(s), 0);
}

void getNeededFiles(std::map<hf::nanos_t, std::string>& files, hf::nanos_t start, hf::nanos_t end) {
    DIR *dir;
    struct dirent *ent;
    if ((dir = opendir (".")) != NULL) {
        while ((ent = readdir (dir)) != NULL) {
            std::string name = ent->d_name;
            std::size_t under = name.find("_");
            std::string timeString = name.substr(under, std::string::npos);
            hf::nanos_t nanos = boost::lexical_cast<hf::nanos_t>(timeString);
            if (nanos > start && nanos < end) {
                files.insert({nanos, name});
            }
        }
        closedir(dir);
    }
}

bool extractLogPart(LogPart& stringPart, LogPart& part, std::size_t& index, std::string& s) {
    stringPart._type = LogPartType::String;
    std::size_t next = s.find(s, index);
    if (next == std::string::npos) {
        stringPart._data = s.substr(index, s.length());
        return false;
    }
    stringPart._data = s.substr(index, next);
    char type = s[next + 1];
    if (type == 'V') {
        part._type = LogPartType::Factor;
        part._id = boost::lexical_cast<uint32_t>(s.substr(next + 3, next + 4));
    } else if (type == 'C') {
        part._type = LogPartType::Contract;
        part._id = boost::lexical_cast<uint32_t>(s.substr(next + 3, next + 4));
    } else if (type == 'i') {
        part._type = LogPartType::Int;
        part._id = boost::lexical_cast<uint32_t>(s.substr(next + 3, next + 4));
    } else if (type == 'd') {
        part._type = LogPartType::Double;
        part._id = boost::lexical_cast<uint32_t>(s.substr(next + 3, next + 4));
    }
    index = next;
    return true;
}

void loadTemplatesAndLookups(std::unordered_map<uint32_t, Log>& templates, std::unordered_map<uint32_t, std::string>& vectorLookup,
                            std::unordered_map<uint32_t, std::string>& contractLookup) {
    std::ifstream templatesFile("./config/templates.txt", std::ios::in);
    std::string line;
    uint32_t templateId;
    if (templatesFile.is_open()) {
        while (getline(templatesFile, line)) {
            Log log;
            std::size_t index = line.find(":");
            std::string idString = line.substr(0, index);
            templateId = atoi(idString.c_str());
            while (true) {
                LogPart stringPart;
                LogPart part;
                bool yes = extractLogPart(stringPart, part, index, line);
                log._parts.push_back(stringPart);
                if (yes) {
                    log._parts.push_back(part);
                } else {
                    break;
                }
            }
            templates.insert({templateId, log});
        }
        templatesFile.close();
    }
}

void write(logging::LogEntry& entry, std::unordered_map<uint32_t, Log>& templates, std::unordered_map<uint32_t, std::string>& vectorLookup,
                             std::unordered_map<uint32_t, std::string>& contractLookup, std::ofstream& file) {
    auto it = templates.find(entry._templateId);
    if (it == templates.end()) {
        file << "missing template " << entry._templateId << std::endl;
    } else {
        std::unordered_map<uint32_t, int64_t> ints;
        std::unordered_map<uint32_t, double> doubles;
        for (uint32_t i = 0; i < entry._numberOfValues; ++i) {
            auto& value = entry._values[i];
            if (value._type == logging::VALUE_TYPE_INT) {
                ints.insert({value._sequenceNumber, (int64_t) value._value});
            } else {
                doubles.insert({value._sequenceNumber, (double) value._value});
            }
        }
        Log& log = it->second;
        for (auto& part : log._parts) {
            switch (part._type) {
                case LogPartType::String: {
                    file << part._data;
                    break;
                }
                case LogPartType::Factor: {
                    auto iit = ints.find(part._id);
                    if (iit == ints.end())  {
                        file << "missing_int_factor=" << part._id;
                    } else {
                        uint32_t id = iit->second;
                        auto fit = vectorLookup.find(id);
                        if (fit == vectorLookup.end()) {
                            file << "missing_factor = " << id;
                        } else {
                            file << fit->second;
                        }
                        }
                    break;
                }
                case LogPartType::Contract: {
                    auto iit = ints.find(part._id);
                    if (iit == ints.end())  {
                        file << "missing_int_contract=" << part._id;
                    } else {
                        uint32_t id = iit->second;
                        auto cit = contractLookup.find(id);
                        if (cit == contractLookup.end()) {
                            file << "missing_contract=" << id;
                        } else {
                            file << cit->second;
                        }
                    }
                    break;
                }
                case LogPartType::Int: {
                    auto iit = ints.find(part._id);
                    if (iit == ints.end())  {
                        file << "missing_int=" << part._id;
                    } else {
                        file << iit->second;
                    }
                    break;
                }
                case LogPartType::Double: {
                    auto dit = doubles.find(part._id);
                    if (dit == doubles.end()) {
                        file << "missing_double=" << part._id;
                    } else {
                        file << dit->second;
                    }
                    break;
                }
                default:
                    throw std::runtime_error("some");
            }
        }
        file << std::endl;
    }
}

int main(int argc, char* argv[]) {
    std::unordered_map<uint32_t, Log> templates;
    std::unordered_map<uint32_t, std::string> vectorLookup;
    std::unordered_map<uint32_t, std::string> contractLookup;
    loadTemplatesAndLookups(templates, vectorLookup, contractLookup);

    hf::nanos_t start = getTimeInNanos(argv[0]);
    hf::nanos_t end = getTimeInNanos(argv[1]);

    std::map<hf::nanos_t, std::string> neededFiles;
    getNeededFiles(neededFiles, start, end);

    std::ofstream outputFile("log.txt", std::ios::out);

    char buffer[2000];
    for (auto& fileName : neededFiles) {
        std::ifstream dataFile(fileName.second, std::ios::in|std::ios::binary);
        while(true) {
            dataFile.read(buffer, 12);
            logging::Header& header = *reinterpret_cast<logging::Header*>(buffer);
            if (header._time < start) {
                dataFile.seekg(header._size, std::ios_base::cur);
                continue;
            }
            if (header._time > end) {
                break;
            }
            dataFile.read(buffer, header._size);
            logging::LogEntry& entry = *reinterpret_cast<logging::LogEntry*>(buffer);
            write(entry, templates, vectorLookup, contractLookup, outputFile);
         }
        dataFile.close();
    }

    outputFile.close();

    return 0;
}

#endif //RHONEDS_LOGREADER_H
