#include <string>
#include <map>
#include <fstream>
#include <dirent.h>
#include <boost/date_time.hpp>

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

inline uint32_t maxValueCount(const std::string& templateFile) {
    std::ifstream temp(templateFile, std::ios::in);
    std::string line;
    uint32_t count = 0;
    while(getline(temp, line)) {
        count = std::max<uint32_t>(count, std::count(line.begin(), line.end(), '{'));
    }
    return count;
}

inline boost::posix_time::ptime epoch_milliseconds_to_ptime(unsigned long int ms) {
    using namespace boost::gregorian;
    using namespace boost::posix_time;
    static const ptime epoch(date(1970, 1, 1));
    return epoch + microseconds(ms / 1000);
}

inline std::string time_to_string(int64_t time) {
    boost::posix_time::ptime pp = epoch_milliseconds_to_ptime(time);
    boost::posix_time::time_facet* facet = new boost::posix_time::time_facet();
    facet->format("%Y-%m-%d %H:%M:%S.%f");
    std::stringstream stream;
    stream.imbue(std::locale(std::locale::classic(), facet));
    stream << pp;
    return stream.str();
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cout << "Missing log directory" << std::endl;
        return EXIT_FAILURE;
    }

    std::string logDir = argv[1];
    std::string filesDir = logDir + "/files/0/";
    std::string templateFile = logDir + "/config/templates.txt";

    uint32_t maxNumOfValues = maxValueCount(templateFile);
    uint32_t minLogSize = sizeof(LogEntry) + sizeof(Value);
    uint32_t maxLogSize = sizeof(LogEntry) + maxNumOfValues * sizeof(Value);

    std::cout << "maxValueCount: " << maxNumOfValues << std::endl;
    std::cout << "sizeof(Header): " << sizeof(Header) << std::endl;
    std::cout << "sizeof(LogEntry): " << sizeof(LogEntry) << std::endl;
    std::cout << "sizeof(Value): " << sizeof(Value) << std::endl;
    std::cout << "minLogSize: " << minLogSize << std::endl;
    std::cout << "maxLogSize: " << maxLogSize << std::endl;

    std::map<int64_t, std::string> allFiles;
    DIR *dir;
    struct dirent *ent;
    uint32_t minSize = std::numeric_limits<uint32_t>::max();
    uint32_t maxSize = 0;
    uint64_t sumSize = 0;
    uint64_t count = 0;
    char buffer[2000];
    uint64_t timestamp = 0;
    uint32_t countPerSec = 0;
    uint64_t sumCountPerSec = 0;
    uint64_t numCountPerSec = 0;
    std::vector<uint32_t> sizeList;
//    sizeList.reserve(10000000000);
    std::vector<uint32_t> countPerSecList;
//    countPerSecList.reserve(5000000000);
    uint64_t numInvalid = 0;
    uint64_t numLinePerFile = 0;
    int64_t lastValidTime = 0;
    bool invalid = false;
    std::vector<std::pair<std::string, uint64_t>> filesHaveInvalidEntry;

    if ((dir = opendir(filesDir.c_str())) != NULL) {
        while ((ent = readdir(dir)) != NULL) {
            std::string name = ent->d_name;
            if (name == "." || name == "..")
                continue;
            std::size_t under = name.find("_");
            std::string timeString = name.substr(under + 1, std::string::npos);
            int64_t nanos = boost::lexical_cast<int64_t>(timeString);
            std::string fullName = filesDir;
            fullName.append(name);
            allFiles.insert({nanos, fullName});
        }
        closedir(dir);
    }

    // drop last file
    if (allFiles.size() > 1) {
        allFiles.erase(prev(allFiles.end()));
    }

    for (auto& pair : allFiles) {
        std::cout << std::endl << pair.second << " " << time_to_string(pair.first) << std::endl;
        std::ifstream dataFile(pair.second, std::ios::in|std::ios::binary);
        numInvalid = 0;
        numLinePerFile = 0;
        while(true) {
            dataFile.read(buffer, 12);
            if (!dataFile) {
                break;
            }
            Header& header = *reinterpret_cast<Header*>(buffer);

            if (header._size < minLogSize || header._size > maxLogSize) {
                ++numInvalid;
                dataFile.read(buffer, header._size);
                invalid = true;

                if (!dataFile) {
                    std::cout << "Invalid LogEntry" << std::endl;
                    break;
                }
//                std::cout << ", " << time_to_string(header._time);
//                LogEntry& entry = *reinterpret_cast<LogEntry*>(buffer);
//                std::cout << ", templateId: " << entry._templateId << ", nVar: " << (int)entry._numberOfValues << std::endl;
                continue;
            }

            if (invalid) {
                std::cout << "Last valid entry at: " << time_to_string(lastValidTime) << std::endl;
                invalid = false;
            }

            minSize = std::min<uint32_t>(minSize, header._size);
            maxSize = std::max<uint32_t>(maxSize, header._size);
            sumSize += header._size;
//            sizeList.emplace_back(header._size);
            lastValidTime = header._time;
            ++count;
            ++numLinePerFile;

            if (timestamp) {
                if ((header._time - timestamp) > 1000000000) {
                    sumCountPerSec += countPerSec;
                    ++numCountPerSec;
//                    countPerSecList.emplace_back(countPerSec);
                    countPerSec = 0;
                    timestamp = header._time;
                } else {
                    ++countPerSec;
                }
            } else {
                timestamp = header._time;
            }

            dataFile.read(buffer, header._size);

            if (!dataFile) {
                std::cout << "Invalid LogEntry" << std::endl;
                break;
            }
        }

        uint32_t avgSize = std::ceil(sumSize / count);
        std::cout << "count: " << count << ", lineInFile: " << numLinePerFile
                  << ", avgSize: " << avgSize
                  << ", avgCountPerSec: " << std::ceil(sumCountPerSec / numCountPerSec)
                  << ", invalidCount: " << numInvalid << std::endl;

        if (numInvalid) {
            filesHaveInvalidEntry.emplace_back(std::make_pair(pair.second, numInvalid));
        }
        dataFile.close();
    }

    if (!count) {
        std::cout << "No log found" << std::endl;
        return EXIT_FAILURE;
    }
    uint32_t avgSize = std::ceil(sumSize / count);
    uint32_t avgCountPerSec = std::ceil(sumCountPerSec / numCountPerSec);
    uint32_t msgPerPkg = std::ceil(1400 / (avgSize + 12));
//    std::sort(sizeList.begin(), sizeList.end());
//    std::sort(countPerSecList.begin(), countPerSecList.end());
    std::cout << std::endl;
    std::cout << "avgSize: " << avgSize /*<< ", medianSize: " << sizeList[(int)(sizeList.size() / 2)]*/ << std::endl;
    std::cout << "On average a package contains " << msgPerPkg << " line of log messages" << std::endl;
    std::cout << "avgCountPerSec: " << avgCountPerSec /*<< ", medianCountPerSec: " << countPerSecList[(int)(countPerSecList.size()/2)]*/ << std::endl;
    std::cout << "Estimated: " << std::ceil(avgCountPerSec / msgPerPkg) << " packages per second" << std::endl;
    std::cout << std::endl;

    for (const auto& pair : filesHaveInvalidEntry) {
        std::cout << pair.first << " has " << pair.second << " invalid entries" << std::endl;
    }
    return EXIT_SUCCESS;
}