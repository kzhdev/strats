#include <boost/format.hpp>
#include <unordered_map>
#include <string>
#include <map>
#include <fstream>
#include <dirent.h>
#include <cstdio>
#include <boost/lexical_cast.hpp>
#include <boost/date_time.hpp>

enum VALUE_TYPE {VALUE_TYPE_INT = 0, VALUE_TYPE_DOUBLE = 1};

#pragma pack(1)

struct Value {
    uint8_t _type;
    uint8_t _sequenceNumber;
    uint64_t _value;
};

struct Header {
    uint64_t _time;
    uint32_t _size;
};

struct LogEntry {
    uint32_t _containerId;
    uint16_t _templateId;
    uint8_t _numberOfValues;
    Value _values[0];
};

#pragma pack()

enum class LogPartType {String, Contract, Factor, Int, Double};

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
    std::locale timeFormat(std::locale::classic(), new bt::time_input_facet("%Y-%m-%d %H:%M:%S"));
    std::istringstream is(s);
    is.imbue(timeFormat);
    bt::ptime pt;
    is >> pt;
    return pt_to_time_t(pt);
}

static constexpr int64_t ONE_SECOND = 1000000000LL;

int64_t getTimeInNanos(char* s) {
    return int64_t(seconds_from_epoch(s)) * ONE_SECOND;
}

void getNeededFiles(std::map<int64_t, std::string>& files, int64_t start, int64_t end, const std::string& filesDir) {
    std::map<int64_t, std::string> allFiles;
    DIR *dir;
    struct dirent *ent;
    if ((dir = opendir (filesDir.c_str())) != NULL) {
        while ((ent = readdir (dir)) != NULL) {
            std::string name = ent->d_name;
            if (name == "." || name == "..")
                continue;
            std::size_t under = name.find("_");
            std::string timeString = name.substr(under + 1, std::string::npos);
            std::cout << "data file: " << name << " " << timeString << std::endl;
            int64_t nanos = boost::lexical_cast<int64_t>(timeString);
            std::string fullName = filesDir;
            fullName.append(name);
            allFiles.insert({nanos, fullName});
        }
        closedir(dir);
    }

    int64_t priorTime = 0;
    std::string priorName;
    bool first = true;
    for (auto& pair : allFiles) {
        int64_t nanos = pair.first;
        if (nanos < start) {
            priorTime = nanos;
            priorName = pair.second;

        }
        bool afterStart = nanos > start;
        bool beforeEnd = nanos < end;
        if (afterStart || beforeEnd) {
            if (first && priorTime != 0) {
                files.insert({priorTime, priorName});
                first = false;
            }
            else {
                files.insert({nanos, pair.second});
            }
            std::cout << "included " << pair.second << std::endl;
        } else {
            std::cout << "not in range " << pair.second << " " << start << " < " << nanos << " " << end << " " << afterStart << " " << beforeEnd << std::endl;
        }
    }
}

uint32_t extractID(const std::string& s, int startingPos) {
    int charsToConsume = 1;
    while(s[startingPos + charsToConsume] != ']' && s[startingPos + charsToConsume] != '}') {
        ++charsToConsume;
    }
    return boost::lexical_cast<uint32_t>(s.substr(startingPos, charsToConsume));
}

bool extractLogPart(LogPart& stringPart, LogPart& part, std::size_t& index, std::string& s) {
    stringPart._type = LogPartType::String;
    std::size_t next = s.find("{", index);
    if (next == std::string::npos) {
        stringPart._data = s.substr(index, std::string::npos);
        return false;
    }
    stringPart._data = s.substr(index, next - index);
    char type = s[next + 1];
    std::cout << stringPart._data << std::endl;
    if (type == 'V') {
        part._type = LogPartType::Factor;
        part._id = extractID(s, next + 4);
        std::cout << "Vector " << part._id << std::endl;
    } else if (type == 'C') {
        part._type = LogPartType::Contract;
        part._id = extractID(s, next + 4);
        std::cout << "Contract " << part._id << std::endl;
    } else if (type == 'i') {
        part._type = LogPartType::Int;
        part._id = extractID(s, next + 2);
        std::cout << "Int " << part._id << std::endl;
    } else if (type == 'd') {
        part._type = LogPartType::Double;
        part._id = extractID(s, next + 2);
        std::cout << "Double " << part._id << std::endl;
    }
    index = s.find("}", next) + 1;
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
            ++index;
            std::cout << templateId << " " << line << std::endl;
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
            std::cout << std::endl;
        }
        templatesFile.close();
    }

    std::ifstream vectorsFile("./config/factors.txt", std::ios::in);
    if (vectorsFile.is_open()) {
        while (getline(vectorsFile, line)) {
            std::size_t index = line.find(":");
            uint32_t id = boost::lexical_cast<uint32_t>(line.substr(0, index));
            std::string value = line.substr(index + 1, std::string::npos);
            vectorLookup.insert({id, value});
            std::cout << "added vector " << id << " " << value << std::endl;
        }
    }
    vectorsFile.close();

    std::ifstream contractsFile("./config/contracts.txt", std::ios::in);
    if (contractsFile.is_open()) {
        while (getline(contractsFile, line)) {
            std::size_t index = line.find(":");
            uint32_t id = boost::lexical_cast<uint32_t>(line.substr(0, index));
            std::string value = line.substr(index + 1, std::string::npos);
            contractLookup.insert({id, value});
            std::cout << "added contract " << id << " " << value << std::endl;
        }
    }
    contractsFile.close();
}

boost::posix_time::ptime epoch_milliseconds_to_ptime(unsigned long int ms) {
    using namespace boost::gregorian;
    using namespace boost::posix_time;
    static const ptime epoch(date(1970, 1, 1));
    return epoch + microseconds(ms / 1000);
}

std::string time_to_string(int64_t time) {
    boost::posix_time::ptime pp = epoch_milliseconds_to_ptime(time);
    boost::posix_time::time_facet* facet = new boost::posix_time::time_facet();
    facet->format("%Y-%m-%d %H:%M:%S.%f");
    std::stringstream stream;
    stream.imbue(std::locale(std::locale::classic(), facet));
    stream << pp;
    return stream.str();
}

void write(int64_t time, LogEntry& entry, std::unordered_map<uint32_t, Log>& templates, std::unordered_map<uint32_t, std::string>& vectorLookup,
           std::unordered_map<uint32_t, std::string>& contractLookup, std::ofstream& file) {
    file << time_to_string(time) << ": ";
    auto it = templates.find(entry._templateId);
    if (it == templates.end()) {
        file << "missing template " << entry._templateId << std::endl;
    } else {
        std::unordered_map<uint32_t, int64_t> ints;
        std::unordered_map<uint32_t, double> doubles;
        for (uint32_t i = 0; i < entry._numberOfValues; ++i) {
            auto &value = entry._values[i];
            uint32_t sequenceNumber = (uint32_t) value._sequenceNumber;
            if (value._type == VALUE_TYPE_INT) {
                int64_t v = *reinterpret_cast<int64_t *>(&value._value);
#ifndef NDEBUG
                std::cout << "INT: " << sequenceNumber << " => " << v << std::endl;
#endif
                ints.insert({sequenceNumber, v});
            } else {
                double v = *reinterpret_cast<double *>(&value._value);
                doubles.insert({sequenceNumber, v});
#ifndef NDEBUG
                std::cout << "DOUBLE: " << sequenceNumber << " => " << v << std::endl;
#endif
            }
        }
        Log &log = it->second;
        for (auto &part : log._parts) {
            switch (part._type) {
                case LogPartType::String: {
                    file << part._data;
                    break;
                }
                case LogPartType::Factor: {
                    auto iit = ints.find(part._id);
                    if (iit == ints.end()) {
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
                    if (iit == ints.end()) {
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
                    if (iit == ints.end()) {
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

    if (argc != 4) {
        std::cout << "start and stop time needed. Format: %Y-%m-%d-%H:%M:%S N=" << argc << std::endl;
        return 0;
    }

    std::cout << argv[0] << " " << argv[1] << " " << argv[2] << std::endl;

    int64_t start = getTimeInNanos(argv[1]);
    int64_t end = getTimeInNanos(argv[2]);
    std::string filesDir = argv[3];

    std::cout << "Start=" << start << " End="  << end << std::endl;

    std::map<int64_t, std::string> neededFiles;
    getNeededFiles(neededFiles, start, end, filesDir);

    std::ofstream outputFile("log.txt", std::ios::out);

    char buffer[2000];
    for (auto& fileName : neededFiles) {
        std::cout << "File: " << fileName.second << std::endl;
        std::ifstream dataFile(fileName.second, std::ios::in|std::ios::binary);
        uint64_t used = 0;
        uint64_t count = 0;
        uint64_t skipped = 0;
        while(true) {
            dataFile.read(buffer, 12);
            Header& header = *reinterpret_cast<Header*>(buffer);
            int64_t time_nanos = header._time;
            used += header._size;
            ++count;
//            std::cout << "got " << time_nanos << " " << header._size << " " << used << " " << count << " " << skipped << std::endl;
            if (time_nanos < start) {
//		std::cout << "skipping " << skipped << std::endl;    
                dataFile.seekg(header._size, std::ios_base::cur);
                continue;
            }
            if (time_nanos > end) {
                break;
            }
            dataFile.read(buffer, header._size);

            if (!dataFile) {
                break;
            }

            LogEntry& entry = *reinterpret_cast<LogEntry*>(buffer);
            write(time_nanos, entry, templates, vectorLookup, contractLookup, outputFile);
        }
        dataFile.close();
    }

    outputFile.close();

    return 0;
}
