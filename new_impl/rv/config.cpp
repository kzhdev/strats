#include "config.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace tachyon::trade::rv {
namespace {

using row = std::vector<std::string>;

[[noreturn]] void fail(const std::string& file, size_t line, const std::string& what) {
    throw std::runtime_error(file + ":" + std::to_string(line) + ": " + what);
}

std::string trim(const std::string& s) {
    const size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

row split(const std::string& line) {
    row out;
    std::string cell;
    std::istringstream stream(line);
    while (std::getline(stream, cell, ',')) out.push_back(trim(cell));
    if (!line.empty() && line.back() == ',') out.push_back({});
    return out;
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

const std::string& cell(const row& r, size_t i) {
    static const std::string empty;
    return i < r.size() ? r[i] : empty;
}

// ---- header-driven column resolution (column order differs between configs) ----
inline size_t col(const std::vector<std::string>& header, const std::string& name) {
    for (size_t i = 0; i < header.size(); ++i) {
        if (header[i] == name) return i;
    }
    return std::string::npos;
}

double dbl_at(const row& cells, const std::vector<std::string>& header, const std::string& name,
              const std::string& file, size_t line) {
    const size_t i = col(header, name);
    return i == std::string::npos ? 0.0 : as_double(file, line, cell(cells, i));
}

int64_t int_at(const row& cells, const std::vector<std::string>& header, const std::string& name,
               const std::string& file, size_t line, int64_t fallback = 0) {
    const size_t i = col(header, name);
    if (i == std::string::npos) return fallback;
    return as_int(file, line, cell(cells, i));
}

bool bool_at(const row& cells, const std::vector<std::string>& header, const std::string& name) {
    const size_t i = col(header, name);
    return i == std::string::npos ? false : is_true(cell(cells, i));
}

// ---- CSV with quoted fields -------------------------------------------------
// secdb's `details` column is JSON and therefore full of commas, so it is quoted
// ("{""legs"": ...}" / "{\"legs\": ...}"). A naive comma split shreds it and, worse,
// shifts every column after it (order_increment_qty, currency, ...). Handle RFC-4180
// quoting and `""`/`\"` escapes. Some secdb exports in this repo lose the outer quotes
// entirely; fold_cells() puts such a row back together (see load_secdb).
row split_quoted(const std::string& line) {
    row out;
    std::string cell_text;
    bool quoted = false;
    for (size_t i = 0; i < line.size(); ++i) {
        const char ch = line[i];
        if (quoted) {
            if (ch == '\\' && i + 1 < line.size() && line[i + 1] == '"') {
                cell_text.push_back('"');  // \" -> "  (JSON-style escape)
                ++i;
            } else if (ch == '"' && i + 1 < line.size() && line[i + 1] == '"') {
                cell_text.push_back('"');  // "" -> "  (CSV escape)
                ++i;
            } else if (ch == '"') {
                quoted = false;
            } else {
                cell_text.push_back(ch);
            }
        } else if (ch == '"' && cell_text.find_first_not_of(" \t") == std::string::npos) {
            cell_text.clear();
            quoted = true;
        } else if (ch == ',') {
            out.push_back(trim(cell_text));
            cell_text.clear();
        } else {
            cell_text.push_back(ch);
        }
    }
    out.push_back(trim(cell_text));
    return out;
}

// Re-join the cells an unquoted `details` field leaked into, so column positions after
// it stay meaningful. Only applied when the row has more cells than the header.
void fold_cells(row& cells, const std::vector<std::string>& header) {
    const size_t details = col(header, "details");
    if (details == std::string::npos || header.empty() || cells.size() <= header.size()) {
        return;
    }
    const size_t extra = cells.size() - header.size();
    std::string folded;
    for (size_t k = details; k <= details + extra && k < cells.size(); ++k) {
        if (k > details) folded.push_back(',');
        folded += cells[k];
    }
    cells.erase(cells.begin() + static_cast<long>(details),
                cells.begin() + static_cast<long>(details + extra + 1));
    cells.insert(cells.begin() + static_cast<long>(details), std::move(folded));
}

inline bool is_name_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

// Position of `key` used as a JSON object key (so `legs` does not match `eurex_nolegs`).
size_t find_key(const std::string& text, const std::string& key, size_t from = 0) {
    size_t pos = text.find(key, from);
    while (pos != std::string::npos) {
        const bool left = pos == 0 || !is_name_char(text[pos - 1]);
        const size_t after = pos + key.size();
        const bool right = after >= text.size() || !is_name_char(text[after]);
        if (left && right) return pos;
        pos = text.find(key, pos + 1);
    }
    return std::string::npos;
}

// Scalar value that follows `key` in one JSON object, with quotes and backslashes
// treated as noise (`{"sym": "BRN FMQ0022!"}`, `{"sym": "BRN FMQ0022!"}` both work).
bool json_scalar(const std::string& object, const std::string& key, std::string& out) {
    const size_t pos = find_key(object, key);
    if (pos == std::string::npos) return false;
    size_t i = pos + key.size();
    while (i < object.size() &&
           (object[i] == ' ' || object[i] == '\t' || object[i] == ':' || object[i] == '"' ||
            object[i] == '\\')) {
        ++i;
    }
    const size_t begin = i;
    while (i < object.size() && object[i] != ',' && object[i] != '}' && object[i] != ']' &&
           object[i] != '"' && object[i] != '\\') {
        ++i;
    }
    out = trim(object.substr(begin, i - begin));
    return !out.empty();
}

// `{"legs": [{"sym": "BRN FMQ0022!", "qty": 1}, {"sym": "BRN FMV0022!", "qty": -1}]}`
// -> two legs. Outrights carry `{}` and yield none. Tolerant by design: the exact key
// order and the quoting of the export do not matter (PLAN §12: secdb is the authority
// on legs, so a parser that silently returned none would be a trading error).
std::vector<secdb_leg> parse_details_legs(const std::string& details) {
    std::vector<secdb_leg> legs;
    const size_t legs_key = find_key(details, "legs");
    if (legs_key == std::string::npos) return legs;
    size_t open = details.find('[', legs_key);
    if (open == std::string::npos) return legs;

    // Walk the array splitting it into `{...}` objects (nesting-aware).
    size_t depth = 0;
    size_t begin = 0;
    for (size_t i = open; i < details.size(); ++i) {
        const char ch = details[i];
        if (ch == '{') {
            if (depth++ == 0) begin = i;
        } else if (ch == '}') {
            if (depth > 0 && --depth == 0) {
                const std::string object = details.substr(begin, i - begin + 1);
                std::string sym;
                std::string qty_text;
                if (json_scalar(object, "sym", sym) && json_scalar(object, "qty", qty_text)) {
                    secdb_leg leg;
                    leg.sym = sym;
                    try {
                        leg.qty = std::stoll(qty_text);
                    } catch (const std::exception&) {
                        continue;  // e.g. "1.0": not a quantity we can use
                    }
                    if (leg.qty != 0) legs.push_back(std::move(leg));
                }
            }
        } else if (ch == ']' && depth == 0) {
            break;
        }
    }
    return legs;
}

}  // namespace

double rv_model_cfg::width_multiplier(shift_column shift) const {
    const auto it = global_columns.find("RVWidthMultiplier");
    if (it == global_columns.end() || it->second.empty()) {
        return 1.0;
    }
    const size_t idx = static_cast<size_t>(shift);
    return idx < it->second.size() ? it->second[idx] : it->second.back();
}

rv_model_cfg rv_model_cfg::load(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("rv_model_cfg: cannot open " + path);
    }

    // Sections of a Viper model CSV. Only the RV-relevant ones are parsed; the others
    // are skipped as a block so their contract-named rows are not read as data.
    const std::vector<std::string> known = {
        "PricingMatrix",  "FactorConfigs", "RVConfigs",      "PricingConfigs",
        "LeanMatrix",     "GlobalConfigs", "MMConfigs",      "ASConfigs",
        "StackerConfigs", "PNLConfigs",    "NegativeScalpCheckerConfigs"};

    rv_model_cfg cfg;
    std::string section;
    std::vector<std::string> header;  // column names of the current section
    std::string line;
    size_t line_no = 0;

    while (std::getline(in, line)) {
        ++line_no;
        if (line.empty() || line[0] == '#') continue;
        row cells = split(line);
        const std::string& first = cell(cells, 0);
        if (first.empty()) continue;  // separator row

        if (std::find(known.begin(), known.end(), first) != known.end()) {
            section = first;
            header = cells;  // column names for this section
            if (section == "PricingMatrix") {  // this row doubles as the header
                cfg.pricing_contracts.clear();
                for (size_t i = 1; i < cells.size() && !cells[i].empty(); ++i) {
                    cfg.pricing_contracts.push_back(cells[i]);
                }
            }
            continue;
        }
        if (section.empty()) continue;
        if (section == "MMConfigs" || section == "ASConfigs" || section == "StackerConfigs" ||
            section == "PNLConfigs" || section == "NegativeScalpCheckerConfigs") {
            continue;
        }

        if (section == "PricingMatrix") {
            std::vector<double> values;
            for (size_t i = 1; i < cells.size(); ++i) {
                if (cells[i].empty()) break;
                values.push_back(as_double(path, line_no, cells[i]));
            }
            if (!values.empty()) {
                cfg.pricing_matrix.push_back(std::move(values));
            }
            continue;
        }

        if (section == "FactorConfigs") {
            rv_vector_cfg v;
            v.name = first;
            v.range = dbl_at(cells, header, "range", path, line_no);
            v.max_pos = dbl_at(cells, header, "maxPos", path, line_no);
            v.ltf = dbl_at(cells, header, "ltf", path, line_no);
            v.ema = bool_at(cells, header, "EMA");
            v.ema_length_secs =
                static_cast<uint32_t>(int_at(cells, header, "EMALength(secs)", path, line_no));
            v.ema_on = static_cast<int32_t>(int_at(cells, header, "EMAOnTime", path, line_no, -1));
            v.ema_off = static_cast<int32_t>(int_at(cells, header, "EMAOffTime", path, line_no, -1));
            v.set_to_market = bool_at(cells, header, "SetToMarket");
            v.chart_decimals = static_cast<uint32_t>(
                int_at(cells, header, "ChartNumberOfDecimals", path, line_no));
            v.hedged = bool_at(cells, header, "Hedged");
            cfg.vectors.push_back(v);
            continue;
        }

        if (section == "RVConfigs") {
            rv_contract_cfg c;
            c.name = first;
            c.quote_external = bool_at(cells, header, "quoteExternal");
            c.hedge_tolerance = dbl_at(cells, header, "hedgeTolerance", path, line_no);
            c.commission = dbl_at(cells, header, "commission", path, line_no);
            c.width = dbl_at(cells, header, "RVWidth", path, line_no);
            c.pct_stf_move = dbl_at(cells, header, "%stfMove", path, line_no);
            c.max_ticks_through_market =
                int_at(cells, header, "maxTicksThroughMarket", path, line_no);
            c.static_quote = bool_at(cells, header, "staticQuote");
            c.quote_size = static_cast<uint32_t>(int_at(cells, header, "quoteSize", path, line_no));
            cfg.rv_contracts.push_back(c);
            continue;
        }

        if (section == "PricingConfigs") {
            rv_pricing_cfg p;
            p.name = first;
            p.weight = dbl_at(cells, header, "weights", path, line_no);
            p.weighted_mid = bool_at(cells, header, "weightedMid");
            p.established_level = int_at(cells, header, "establishedLevel", path, line_no);
            p.pricing_multiplier = dbl_at(cells, header, "pricingMultiplier", path, line_no);
            p.quantity_multiplier = dbl_at(cells, header, "quantityMultiplier", path, line_no);
            p.sweep_ticks = static_cast<int32_t>(int_at(cells, header, "sweepTicks", path, line_no));
            p.sweep_qty = int_at(cells, header, "sweepQty", path, line_no);
            p.max_ticks_wide = int_at(cells, header, "maxTicksWide", path, line_no, -1);
            p.as_ext_commission = dbl_at(cells, header, "asExtCommission", path, line_no);
            if (p.pricing_multiplier == 0.0) p.pricing_multiplier = 1.0;  // column absent
            if (p.quantity_multiplier == 0.0) p.quantity_multiplier = 1.0;
            cfg.pricing_configs.push_back(p);
            continue;
        }

        if (section == "LeanMatrix") {
            std::vector<int32_t> row_values;
            for (size_t i = 1; i < cells.size(); ++i) {
                if (cells[i].empty()) break;
                row_values.push_back(static_cast<int32_t>(as_int(path, line_no, cells[i])));
            }
            if (!row_values.empty()) {
                cfg.leans.emplace_back(first, std::move(row_values));
            }
            continue;
        }

        if (section == "GlobalConfigs") {
            std::vector<double> values;
            for (size_t i = 1; i < cells.size(); ++i) {
                if (cells[i].empty()) break;
                values.push_back(as_double(path, line_no, cells[i]));
            }
            if (!values.empty()) {
                cfg.globals[first] = values.front();
                cfg.global_columns[first] = std::move(values);
            }
            continue;
        }
    }

    if (cfg.pricing_contracts.empty() || cfg.vectors.empty()) {
        throw std::runtime_error("rv_model_cfg: " + path + " has no PricingMatrix/FactorConfigs");
    }
    if (cfg.pricing_matrix.size() != cfg.vectors.size()) {
        throw std::runtime_error("rv_model_cfg: " + path +
                                 " pricing matrix rows != vector count");
    }
    return cfg;
}

secdb_table load_secdb(const std::string& path) {
    secdb_table table;
    std::ifstream in(path);
    if (!in) {
        return table;  // optional: callers may pass tick overrides instead
    }
    std::string line;
    size_t line_no = 0;
    size_t sym_col = 0, tick_col = 0, incr_col = 0, venue_col = 0;
    std::vector<std::string> header;
    while (std::getline(in, line)) {
        ++line_no;
        if (line.empty()) continue;
        row cells = split_quoted(line);
        if (header.empty()) {
            header = cells;
            for (size_t i = 0; i < cells.size(); ++i) {
                if (cells[i] == "sym") sym_col = i;
                if (cells[i] == "tick") tick_col = i;
                if (cells[i] == "order_increment_qty") incr_col = i;
                if (cells[i] == "activate") venue_col = i;
            }
            continue;
        }
        fold_cells(cells, header);
        const std::string& sym = cell(cells, sym_col);
        const std::string& tick = cell(cells, tick_col);
        if (sym.empty() || tick.empty()) continue;
        secdb_row r;
        r.tick = as_double(path, line_no, tick);
        const int64_t incr = as_int(path, line_no, cell(cells, incr_col));
        r.order_increment_qty = incr > 0 ? incr : 1;
        const std::string& venue = cell(cells, venue_col);
        r.venue = venue == "CME"     ? 0u
                  : venue == "ICE"   ? 1u
                  : venue == "EUREX" ? 2u
                  : venue == "JPX"   ? 3u
                                     : 4u;
        // Spread legs. secdb is the authoritative source (PLAN §12): a name splitter
        // cannot resolve ICE's "BRN FMQ0022!" legs or EUREX's "efFGBMU1:Z1" shorthand.
        r.legs = parse_details_legs(cell(cells, col(header, "details")));
        table[sym] = std::move(r);
    }
    return table;
}

std::unordered_map<std::string, double> load_secdb_ticks(const std::string& path) {
    std::unordered_map<std::string, double> ticks;
    for (const auto& [sym, row_] : load_secdb(path)) {
        ticks.emplace(sym, row_.tick);
    }
    return ticks;
}

}  // namespace tachyon::trade::rv
