#include "config.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace tachyon::trade::as {
namespace {

[[noreturn]] void fail(const std::string& file, size_t line, const std::string& what) {
    throw std::runtime_error(file + ":" + std::to_string(line) + ": " + what);
}

std::string trim(const std::string& s) {
    const size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::vector<std::string> split(const std::string& line) {
    std::vector<std::string> out;
    std::string cell_text;
    std::istringstream stream(line);
    while (std::getline(stream, cell_text, ',')) out.push_back(trim(cell_text));
    if (!line.empty() && line.back() == ',') out.push_back({});
    return out;
}

// legacy trimEnd: drop trailing empty cells, so an all-commas row becomes empty.
void trim_end(std::vector<std::string>& v) {
    while (!v.empty() && v.back().empty()) v.pop_back();
}

bool is_true(const std::string& s) { return s == "TRUE" || s == "True" || s == "true" || s == "1"; }

double as_double(const std::string& file, size_t line, const std::string& s) {
    if (s.empty()) return 0.0;
    try {
        return std::stod(s);
    } catch (const std::exception&) {
        fail(file, line, "expected a number, got '" + s + "'");
    }
}

int64_t as_int(const std::string& file, size_t line, const std::string& s) {
    if (s.empty()) return 0;
    try {
        return std::stoll(s);
    } catch (const std::exception&) {
        fail(file, line, "expected an integer, got '" + s + "'");
    }
}

const std::string& cell(const std::vector<std::string>& r, size_t i) {
    static const std::string empty;
    return i < r.size() ? r[i] : empty;
}

size_t col(const std::vector<std::string>& header, const std::string& name) {
    for (size_t i = 0; i < header.size(); ++i) {
        if (header[i] == name) return i;
    }
    return std::string::npos;
}

// Section markers of a Viper model CSV, mirroring rv_model_cfg::load so that a row naming
// another section terminates the ASConfigs block instead of being read as data.
bool is_section_marker(const std::string& first) {
    static const char* kNames[] = {"PricingMatrix",  "FactorConfigs", "RVConfigs",
                                   "PricingConfigs", "LeanMatrix",    "GlobalConfigs",
                                   "MMConfigs",      "ASConfigs",     "StackerConfigs",
                                   "PNLConfigs",     "NegativeScalpCheckerConfigs"};
    for (const char* name : kNames) {
        if (first == name) return true;
    }
    return false;
}

}  // namespace

namespace {

// Reads the ASConfigs rows (header-driven, so a column reorder is harmless).
std::vector<as_contract_cfg> load_as_configs(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("as_model_cfg: cannot open " + path);

    std::vector<as_contract_cfg> out;
    std::vector<std::string> header;
    bool in_section = false;
    std::string line;
    size_t line_no = 0;

    while (std::getline(in, line)) {
        ++line_no;
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> cells = split(line);
        trim_end(cells);
        const std::string first = cell(cells, 0);
        if (first.empty()) {  // separator row ends the section
            in_section = false;
            continue;
        }
        if (is_section_marker(first)) {
            in_section = first == "ASConfigs";
            if (in_section) header = cells;
            continue;
        }
        if (!in_section) continue;

        const size_t i_size = col(header, "quoteSize");
        if (i_size == std::string::npos) {
            fail(path, line_no, "ASConfigs header has no quoteSize column");
        }
        as_contract_cfg c;
        c.name = first;
        // The canonical section (ViperConfig.h:923-933) starts with `enabled`. Older model
        // files predate that column (config/minimal/BRN_small_model.csv); such a row means
        // "always enabled", so an absent column defaults to true rather than silently
        // disabling a contract the file expects to trade.
        const size_t i_enabled = col(header, "enabled");
        c.enabled = i_enabled == std::string::npos ? true : is_true(cell(cells, i_enabled));
        c.quote_external = is_true(cell(cells, col(header, "quoteExternal")));
        c.quote_size = as_int(path, line_no, cell(cells, i_size));
        c.edge = as_double(path, line_no, cell(cells, col(header, "edge")));
        c.hedge_offset = as_double(path, line_no, cell(cells, col(header, "hedgeOffset")));
        c.max_ticks_through_market =
            as_double(path, line_no, cell(cells, col(header, "maxTicksThroughMarket")));
        out.push_back(std::move(c));
    }
    if (out.empty()) {
        throw std::runtime_error(path + ": no ASConfigs rows");
    }
    return out;
}

// Reads the paths CSV: repeating blocks of
//   <quoting contract>,<lean contract>,...,Commission,Enabled
//   <path label>,<multiplier>,...,<commission>,<enabled>
// terminated by an all-empty (all-comma) row. Mirrors legacy loadArbieSpreaderPaths,
// including that the two trailing columns must literally be `Commission` and `Enabled`.
void load_paths(const std::string& path, std::vector<as_contract_cfg>& contracts) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("as_model_cfg: cannot open " + path);

    auto find_contract = [&contracts](const std::string& name) -> as_contract_cfg* {
        for (auto& c : contracts) {
            if (c.name == name) return &c;
        }
        return nullptr;
    };

    std::string line;
    size_t line_no = 0;

    while (std::getline(in, line)) {
        ++line_no;
        std::vector<std::string> header = split(line);
        trim_end(header);
        if (header.empty()) continue;  // blank lines between/after blocks

        as_contract_cfg* owner = find_contract(cell(header, 0));
        if (!owner) {
            fail(path, line_no, "paths block for '" + cell(header, 0) + "' has no ASConfigs row");
        }
        if (header.size() < 3) {
            fail(path, line_no, "paths header needs at least one lean column");
        }

        const size_t commission_col = header.size() - 2;
        const size_t enabled_col = header.size() - 1;
        if (header[commission_col] != "Commission") {
            fail(path, line_no, "expected 'Commission', got '" + header[commission_col] + "'");
        }
        if (header[enabled_col] != "Enabled") {
            fail(path, line_no, "expected 'Enabled', got '" + header[enabled_col] + "'");
        }

        while (std::getline(in, line)) {
            ++line_no;
            std::vector<std::string> values = split(line);
            trim_end(values);
            if (values.empty()) break;  // end of this block
            if (values.size() != header.size()) {
                fail(path, line_no, "path row has " + std::to_string(values.size()) +
                                        " cells, header has " + std::to_string(header.size()));
            }

            as_path_cfg p;
            p.name = cell(values, 0);
            for (size_t i = 1; i < commission_col; ++i) {
                const int64_t multiplier = as_int(path, line_no, values[i]);
                if (multiplier != 0) {
                    p.leans.push_back({header[i], static_cast<int32_t>(multiplier)});
                }
            }
            p.commission = as_double(path, line_no, values[commission_col]);
            p.enabled = is_true(values[enabled_col]);
            owner->paths.push_back(std::move(p));
        }
    }
}

}  // namespace

double as_model_cfg::edge_multiplier_for(rv::shift_column shift) const {
    const auto it = rv.global_columns.find("ASEdgeMultiplier");
    if (it == rv.global_columns.end() || it->second.empty()) {
        return 1.0;
    }
    // GlobalConfigs columns are (Value, Asia, Europe, US, SafeMode): legacy fills
    // GlobalConfig::_shifts from column 2 onward (ViperConfig.h:738-741) and `Shifts` starts
    // at Asia (Common.h:170), so a shift's value is at index shift + 1. A row that carries no
    // shift column at all falls back to its Value column.
    const size_t idx = static_cast<size_t>(shift) + 1;
    return idx < it->second.size() ? it->second[idx] : it->second.front();
}

as_model_cfg as_model_cfg::load(const std::string& model_csv, const std::string& paths_csv,
                                const std::string& secdb_csv, rv::shift_column shift) {
    as_model_cfg cfg;
    cfg.rv = rv::rv_model_cfg::load(model_csv);  // the shared sections
    cfg.secdb = rv::load_secdb(secdb_csv);
    cfg.as_contracts = load_as_configs(model_csv);
    load_paths(paths_csv, cfg.as_contracts);
    cfg.edge_multiplier = cfg.edge_multiplier_for(shift);
    return cfg;
}

}  // namespace tachyon::trade::as
