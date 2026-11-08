#include "config.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace tachyon::trade::mm {
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

bool is_true(const std::string& s) {
    return s == "TRUE" || s == "True" || s == "true" || s == "1";
}

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

// RFC-4180 quoting. secdb's `details` column is JSON full of commas
// (config/backtest/secdb.cme.csv:11), and a naive comma split would shift every column
// after it — including `order_increment_qty`, which is the lot size.
row split_quoted(const std::string& line) {
    row out;
    std::string text;
    bool quoted = false;
    for (size_t i = 0; i < line.size(); ++i) {
        const char ch = line[i];
        if (quoted) {
            if (ch == '\\' && i + 1 < line.size() && line[i + 1] == '"') {
                text.push_back('"');  // \" -> "
                ++i;
            } else if (ch == '"' && i + 1 < line.size() && line[i + 1] == '"') {
                text.push_back('"');  // "" -> "
                ++i;
            } else if (ch == '"') {
                quoted = false;
            } else {
                text.push_back(ch);
            }
        } else if (ch == '"' && text.find_first_not_of(" \t") == std::string::npos) {
            text.clear();
            quoted = true;
        } else if (ch == ',') {
            out.push_back(trim(text));
            text.clear();
        } else {
            text.push_back(ch);
        }
    }
    out.push_back(trim(text));
    return out;
}

// Section titles used by the model CSV. A row starting with one of these ends the section
// being parsed, together with a blank line.
bool is_section_title(const std::string& s) {
    static const char* kTitles[] = {"PricingMatrix", "FactorConfigs", "RVConfigs",
                                    "PricingConfigs", "LeanMatrix",   "MMConfigs",
                                    "ASConfigs",     "StackerConfigs", "GlobalConfigs",
                                    "ManualFills",   "PathConfigs",   "TraderLogin"};
    for (const char* title : kTitles) {
        if (s == title) return true;
    }
    return false;
}

bool all_empty(const row& cells) {
    for (const auto& c : cells) {
        if (!c.empty()) return false;
    }
    return true;
}

// ---- header-driven column resolution (column order differs between configs) ----
size_t col(const row& header, const std::string& name) {
    for (size_t i = 0; i < header.size(); ++i) {
        if (header[i] == name) return i;
    }
    return std::string::npos;
}

double dbl_at(const row& cells, const row& header, const std::string& name, const std::string& file,
              size_t line) {
    const size_t i = col(header, name);
    return i == std::string::npos ? 0.0 : as_double(file, line, cell(cells, i));
}

int64_t int_at(const row& cells, const row& header, const std::string& name, const std::string& file,
               size_t line, int64_t fallback = 0) {
    const size_t i = col(header, name);
    if (i == std::string::npos) return fallback;
    return as_int(file, line, cell(cells, i));
}

bool bool_at(const row& cells, const row& header, const std::string& name) {
    const size_t i = col(header, name);
    return i == std::string::npos ? false : is_true(cell(cells, i));
}

std::string upper(std::string s) {
    for (char& ch : s) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    return s;
}

}  // namespace

mm_model_cfg mm_model_cfg::load(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error(path + ": cannot open");
    }

    std::string line;
    size_t line_no = 0;
    row header;

    // Find the MMConfigs header row. Everything before it belongs to other sections.
    while (std::getline(in, line)) {
        ++line_no;
        row cells = split_quoted(line);
        if (!cells.empty() && cells[0] == "MMConfigs") {
            header = std::move(cells);
            break;
        }
    }
    if (header.empty()) {
        throw std::runtime_error(path + ": no MMConfigs section");
    }

    mm_model_cfg cfg;
    while (std::getline(in, line)) {
        ++line_no;
        row cells = split_quoted(line);
        if (all_empty(cells)) break;
        if (is_section_title(cells[0])) break;

        const std::string& name = cell(cells, 0);
        if (name.empty()) break;

        mm_contract_cfg c;
        c.name = name;
        c.enabled = bool_at(cells, header, "enabled");
        c.quote_external = bool_at(cells, header, "quoteExternal");
        c.quote_size = static_cast<uint32_t>(int_at(cells, header, "quoteSize", path, line_no));
        c.quote_gap = static_cast<uint32_t>(int_at(cells, header, "quoteGap", path, line_no));
        c.pct_tob_max_quote = dbl_at(cells, header, "quotePctTob", path, line_no);
        c.width_pct[0] = dbl_at(cells, header, "AsiaWidthPercent", path, line_no);
        c.width_pct[1] = dbl_at(cells, header, "EuroWidthPercent", path, line_no);
        c.width_pct[2] = dbl_at(cells, header, "USWidthPercent", path, line_no);
        c.width_pct[3] = dbl_at(cells, header, "SafeModeWidthPercent", path, line_no);
        c.hedge_offset = dbl_at(cells, header, "hedgeOffset", path, line_no);
        c.join_size = static_cast<uint32_t>(int_at(cells, header, "joinSize", path, line_no));
        c.leave_size = static_cast<uint32_t>(int_at(cells, header, "leaveSize", path, line_no));
        c.payup_qty = static_cast<uint32_t>(int_at(cells, header, "payupQty", path, line_no));
        c.payup_orders = static_cast<uint32_t>(int_at(cells, header, "payupOrders", path, line_no));
        c.payup_multiple = dbl_at(cells, header, "payupMultiple", path, line_no);
        c.max_pos_internal =
            static_cast<uint32_t>(int_at(cells, header, "maxPosInternal", path, line_no));
        c.max_pos_external =
            static_cast<uint32_t>(int_at(cells, header, "maxPosExternal", path, line_no));
        c.commission = dbl_at(cells, header, "commission", path, line_no);
        c.min_internal_price_change =
            dbl_at(cells, header, "minInternalPriceChange", path, line_no);
        // EstLevelEMA is the initial long-run average of the established level.
        c.est_level_ema = dbl_at(cells, header, "EstLevelEMA", path, line_no);
        c.est_level_ema_seconds =
            static_cast<uint32_t>(int_at(cells, header, "EstLevelEMASeconds", path, line_no));
        c.est_level_pct_ema = dbl_at(cells, header, "EstLevel%EMA", path, line_no);
        c.max_ticks_wide =
            static_cast<int32_t>(int_at(cells, header, "maxTicksWide", path, line_no, -1));
        c.worsen_payup_pct = dbl_at(cells, header, "MMWorsenPayUpCondPct", path, line_no);
        c.queue_importance =
            static_cast<uint32_t>(int_at(cells, header, "queueImportance", path, line_no));
        c.internal_position =
            static_cast<int32_t>(int_at(cells, header, "internalPosition", path, line_no));
        c.external_position =
            static_cast<int32_t>(int_at(cells, header, "externalPosition", path, line_no));

        cfg.contracts.push_back(std::move(c));
    }

    return cfg;
}

secdb_table load_secdb(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error(path + ": cannot open");
    }

    std::string line;
    if (!std::getline(in, line)) {
        throw std::runtime_error(path + ": empty file");
    }
    const row header = split_quoted(line);
    const size_t sym_col = col(header, "sym");
    const size_t tick_col = col(header, "tick");
    const size_t activate_col = col(header, "activate");
    const size_t lot_col = col(header, "order_increment_qty");
    if (sym_col == std::string::npos || tick_col == std::string::npos) {
        throw std::runtime_error(path + ": no `sym`/`tick` column");
    }

    secdb_table table;
    size_t line_no = 1;
    while (std::getline(in, line)) {
        ++line_no;
        const row cells = split_quoted(line);
        const std::string& sym = cell(cells, sym_col);
        if (sym.empty()) continue;

        secdb_row r;
        r.tick = as_double(path, line_no, cell(cells, tick_col));
        if (lot_col != std::string::npos) {
            const int64_t lot = as_int(path, line_no, cell(cells, lot_col));
            r.order_increment_qty = lot > 0 ? lot : 1;  // legacy would divide by zero
        }
        if (activate_col != std::string::npos) {
            const std::string venue_name = upper(cell(cells, activate_col));
            if (venue_name == "CME") {
                r.venue_ = venue::cme;
            } else if (venue_name == "ICE") {
                r.venue_ = venue::ice;
            } else if (venue_name == "EUREX") {
                r.venue_ = venue::eurex;
            } else if (venue_name == "B3") {
                r.venue_ = venue::b3;
            } else {
                r.venue_ = venue::other;
            }
        }
        table.emplace(sym, r);
    }

    return table;
}

model_inputs build_model_inputs(const mm_model_cfg& cfg,
                                const secdb_table& secdb,
                                double cycles_per_second,
                                uint32_t current_shift,
                                const std::unordered_map<std::string, double>& tick_overrides) {
    model_inputs out;
    out.cycles_per_second = cycles_per_second;
    out.current_shift = current_shift < SHIFT_COUNT ? current_shift : 0;
    out.contracts.reserve(cfg.contracts.size());

    for (const mm_contract_cfg& row_cfg : cfg.contracts) {
        contract_input in;
        in.name = row_cfg.name;

        double tick_dollars = 0.0;
        bool have_tick = false;
        const auto it = secdb.find(row_cfg.name);
        if (it != secdb.end()) {
            tick_dollars = it->second.tick;
            in.lot = static_cast<qty_t>(it->second.order_increment_qty > 0
                                            ? it->second.order_increment_qty
                                            : 1);
            in.venue = static_cast<uint32_t>(it->second.venue_);
            have_tick = tick_dollars > 0.0;
        }
        if (!have_tick) {
            const auto ov = tick_overrides.find(row_cfg.name);
            if (ov != tick_overrides.end()) {
                tick_dollars = ov->second;
                have_tick = tick_dollars > 0.0;
            }
        }
        if (!have_tick) {
            throw std::runtime_error(row_cfg.name +
                                     ": no secdb row with a `tick` value and no tick override");
        }
        in.tick_size = to_mm_price(tick_dollars);

        in.enabled = row_cfg.enabled;
        in.quote_external = row_cfg.quote_external;
        in.quote_size = row_cfg.quote_size;
        in.quote_gap = row_cfg.quote_gap;
        in.pct_tob_max_quote = row_cfg.pct_tob_max_quote;
        for (uint32_t s = 0; s < SHIFT_COUNT; ++s) in.width_pct[s] = row_cfg.width_pct[s];
        in.hedge_offset = row_cfg.hedge_offset;
        in.join_size = row_cfg.join_size;
        in.leave_size = row_cfg.leave_size;
        in.payup_qty = row_cfg.payup_qty;
        in.payup_orders = row_cfg.payup_orders;
        in.payup_multiple = row_cfg.payup_multiple;
        in.max_pos_internal = row_cfg.max_pos_internal;
        in.max_pos_external = row_cfg.max_pos_external;
        in.commission = row_cfg.commission;
        in.min_internal_price_change = row_cfg.min_internal_price_change;
        in.est_level_ema = row_cfg.est_level_ema;
        in.est_level_ema_seconds = row_cfg.est_level_ema_seconds;
        in.est_level_pct_ema = row_cfg.est_level_pct_ema;
        in.max_ticks_wide = row_cfg.max_ticks_wide;
        in.worsen_payup_pct = row_cfg.worsen_payup_pct;
        in.queue_importance = row_cfg.queue_importance;
        in.internal_position = row_cfg.internal_position;
        in.external_position = row_cfg.external_position;

        out.contracts.push_back(std::move(in));
    }

    return out;
}

}  // namespace tachyon::trade::mm
