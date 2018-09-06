#ifndef RHONEDS_LOGREADER_H
#define RHONEDS_LOGREADER_H

#include <hf/hf.h>
#include <unordered_map>
#include <string>
#include <iostream>
#include <experimental/filesystem>
#include <map>
#include <fstream>
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

hf::nanos_t getTimeInNanos(char* s) {
    return 0;
}

void readFileNames(std::unordered_map<hf::nanos_t, std::string>& files) {
    namespace fs = std::experimental::filesystem;

    std::string path = ".";
    for (const auto & p : fs::directory_iterator(path)) {
        auto& path = p.path();
        std::string fileName = path.filename();
    }
}

void loadTemplatesAndLookups(std::unordered_map<uint32_t, std::string>& templates, std::unordered_map<uint32_t, std::string>& vectorLookup,
                            std::unordered_map<uint32_t, std::string>& contractLookup) {
}

void write(logging::LogEntry& entry, std::unordered_map<uint32_t, std::string>& templates, std::unordered_map<uint32_t, std::string>& vectorLookup,
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
    }
}

void getNeededFiles(std::map<hf::nanos_t, std::string>& neededFiles, hf::nanos_t start, hf::nanos_t end) {
    std::unordered_map<hf::nanos_t, std::string> files;
    readFileNames(files);

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

    std::ofstream file("log.txt", std::ios::out);

    char buffer[2000];
    for (auto& f : neededFiles) {
        std::ifstream file(f.second, std::ios::in|std::ios::binary);
        while(true) {
            file.read(buffer, 4);
            uint32_t size = *reinterpret_cast<uint32_t*>(buffer);
            file.read(buffer, size);
            logging::LogEntry& entry = *reinterpret_cast<logging::LogEntry*>(buffer);
            if (entry._time < start) {
                continue;
            }
            if (entry._time > end) {
                break;
            }
            write(entry, templates, vectorLookup, contractLookup, file);
         }
        file.close();
    }

    return 0;
}



#endif //RHONEDS_LOGREADER_H
