#include <Eigen/Dense>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

#if defined(_MSC_VER)
#define VIPER_BENCHMARK_NOINLINE __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
#define VIPER_BENCHMARK_NOINLINE __attribute__((noinline))
#else
#define VIPER_BENCHMARK_NOINLINE
#endif

using Clock = std::chrono::steady_clock;

struct Options {
    std::size_t dimension = 6;
    std::size_t mock_updates = 4096;
    std::size_t updates = 2'000'000;
    std::size_t warmup_updates = 20'000;
    std::size_t runs = 7;
    std::uint64_t seed = 0x5EED5EEDULL;
};

struct RunResult {
    std::chrono::nanoseconds elapsed;
    double checksum;
};

class CurrentMatrix {
public:
    explicit CurrentMatrix(std::size_t dimension)
        : dimension_(dimension)
        , rows_(dimension) {
        for (auto& row : rows_) {
            row = std::make_unique<double[]>(dimension_);
        }
    }

    double getValue(std::size_t row, std::size_t column) const noexcept {
        return rows_[row][column];
    }

    void setValue(std::size_t row, std::size_t column, double value) noexcept {
        rows_[row][column] = value;
    }

private:
    std::size_t dimension_;
    std::vector<std::unique_ptr<double[]>> rows_;
};

using EigenPricingMatrix = Eigen::Matrix<double, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>;

struct Inputs {
    CurrentMatrix current_pricing_matrix;
    EigenPricingMatrix eigen_pricing_matrix;
    Eigen::VectorXd eigen_pricing_multipliers;
    std::vector<double> pricing_multipliers;
    std::vector<double> mock_price_updates;
    std::size_t dimension;
    std::size_t mock_update_count;

    explicit Inputs(const Options& options)
        : current_pricing_matrix(options.dimension)
        , eigen_pricing_matrix(static_cast<Eigen::Index>(options.dimension), static_cast<Eigen::Index>(options.dimension))
        , eigen_pricing_multipliers(static_cast<Eigen::Index>(options.dimension))
        , pricing_multipliers(options.dimension)
        , mock_price_updates(options.dimension * options.mock_updates)
        , dimension(options.dimension)
        , mock_update_count(options.mock_updates) {
        populatePricingInputs();
        populateMockPriceUpdates(options.seed);
    }

    const double* priceUpdate(std::size_t update_index) const noexcept {
        const std::size_t offset = (update_index % mock_update_count) * dimension;
        return mock_price_updates.data() + offset;
    }

private:
    void populatePricingInputs() {
        for (std::size_t row = 0; row < dimension; ++row) {
            for (std::size_t column = 0; column < dimension; ++column) {
                // A deterministic, dense, diagonally dominant pricing matrix.
                const double off_diagonal = 0.05 * std::sin(static_cast<double>((row + 1) * (column + 3)));
                const double value = (row == column ? 1.0 : 0.0) + off_diagonal;
                current_pricing_matrix.setValue(row, column, value);
                eigen_pricing_matrix(static_cast<Eigen::Index>(row), static_cast<Eigen::Index>(column)) = value;
            }

            const double multiplier = 0.75 + (0.5 * static_cast<double>(row + 1) / static_cast<double>(dimension));
            pricing_multipliers[row] = multiplier;
            eigen_pricing_multipliers(static_cast<Eigen::Index>(row)) = multiplier;
        }
    }

    static std::uint64_t nextRandom(std::uint64_t& state) noexcept {
        // xorshift64*: deterministic and deliberately outside the timed section.
        state ^= state >> 12U;
        state ^= state << 25U;
        state ^= state >> 27U;
        return state * 2685821657736338717ULL;
    }

    void populateMockPriceUpdates(std::uint64_t seed) {
        std::vector<double> prices(dimension);
        for (std::size_t contract = 0; contract < dimension; ++contract) {
            prices[contract] = 100.0 + (1.75 * static_cast<double>(contract));
        }

        std::uint64_t random_state = seed;
        for (std::size_t update = 0; update < mock_update_count; ++update) {
            for (std::size_t contract = 0; contract < dimension; ++contract) {
                const std::uint64_t random = nextRandom(random_state);
                const std::int64_t signed_tick = static_cast<std::int64_t>((random >> 11U) % 2001U) - 1000;
                prices[contract] += static_cast<double>(signed_tick) * 0.00001;
                mock_price_updates[(update * dimension) + contract] = prices[contract];
            }
        }
    }
};

// This is a direct transcription of RelativeValue::calculateVectorMarketPrices'
// two arithmetic loops. The production Matrix stores each row separately, so this
// benchmark deliberately preserves that storage pattern for the baseline.
VIPER_BENCHMARK_NOINLINE void calculateCurrent(const CurrentMatrix& pricing_matrix,
                                                const double* overdetermined_prices,
                                                const double* pricing_multipliers,
                                                std::size_t dimension,
                                                double* factor_prices) {
    for (std::size_t factor = 0; factor < dimension; ++factor) {
        double sum = 0.0;
        for (std::size_t outright = 0; outright < dimension; ++outright) {
            sum += pricing_matrix.getValue(factor, outright)
                   * overdetermined_prices[outright]
                   * pricing_multipliers[outright];
        }
        factor_prices[factor] = sum;
    }
}

class EigenCalculator {
public:
    explicit EigenCalculator(const Inputs& inputs)
        : pricing_matrix_(inputs.eigen_pricing_matrix)
        , pricing_multipliers_(inputs.eigen_pricing_multipliers)
        , scaled_prices_(static_cast<Eigen::Index>(inputs.dimension))
        , factor_prices_(static_cast<Eigen::Index>(inputs.dimension)) {
    }

    // Both Eigen vectors are preallocated. The timed path has no allocations.
    VIPER_BENCHMARK_NOINLINE void calculate(const double* overdetermined_prices) {
        const Eigen::Map<const Eigen::VectorXd> prices(
            overdetermined_prices,
            pricing_multipliers_.size());
        scaled_prices_.array() = prices.array() * pricing_multipliers_.array();
        factor_prices_.noalias() = pricing_matrix_ * scaled_prices_;
    }

    const double* factorPrices() const noexcept {
        return factor_prices_.data();
    }

private:
    EigenPricingMatrix pricing_matrix_;
    Eigen::VectorXd pricing_multipliers_;
    Eigen::VectorXd scaled_prices_;
    Eigen::VectorXd factor_prices_;
};

[[noreturn]] void usageAndExit(const std::string& message = {}) {
    if (!message.empty()) {
        std::cerr << "Error: " << message << "\n\n";
    }

    std::cerr
        << "Usage: relative_value_vector_market_price_benchmark [options]\n"
        << "  --dimension N       Factors and pricing contracts (default: 6)\n"
        << "  --mock-updates N    Deterministic price snapshots to cycle through (default: 4096)\n"
        << "  --updates N         Timed calculations per run (default: 2000000)\n"
        << "  --warmup N          Untimed calculations before measurement (default: 20000)\n"
        << "  --runs N            Alternating current/Eigen timed runs; median is reported (default: 7)\n"
        << "  --seed N            Deterministic mock-price seed (default: 1592614637)\n"
        << "  --help              Show this message\n";
    std::exit(message.empty() ? EXIT_SUCCESS : EXIT_FAILURE);
}

std::size_t parseSize(std::string_view option, const char* value) {
    try {
        std::size_t consumed = 0;
        const auto parsed = std::stoull(value, &consumed);
        if (value[consumed] != '\0' || parsed == 0) {
            usageAndExit(std::string(option) + " must be a positive integer");
        }
        if (parsed > std::numeric_limits<std::size_t>::max()) {
            usageAndExit(std::string(option) + " is too large");
        }
        return static_cast<std::size_t>(parsed);
    } catch (const std::exception&) {
        usageAndExit(std::string(option) + " must be a positive integer");
    }
}

Options parseOptions(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view option(argv[index]);
        if (option == "--help") {
            usageAndExit();
        }
        if (++index >= argc) {
            usageAndExit(std::string(option) + " requires a value");
        }

        const std::size_t value = parseSize(option, argv[index]);
        if (option == "--dimension") {
            options.dimension = value;
        } else if (option == "--mock-updates") {
            options.mock_updates = value;
        } else if (option == "--updates") {
            options.updates = value;
        } else if (option == "--warmup") {
            options.warmup_updates = value;
        } else if (option == "--runs") {
            options.runs = value;
        } else if (option == "--seed") {
            options.seed = value;
        } else {
            usageAndExit("unknown option " + std::string(option));
        }
    }
    return options;
}

void verifyEquivalent(const Inputs& inputs, EigenCalculator& eigen_calculator) {
    std::vector<double> current_factor_prices(inputs.dimension);
    const double* prices = inputs.priceUpdate(0);
    calculateCurrent(inputs.current_pricing_matrix, prices, inputs.pricing_multipliers.data(), inputs.dimension,
                     current_factor_prices.data());
    eigen_calculator.calculate(prices);

    constexpr double relative_tolerance = 1e-12;
    for (std::size_t factor = 0; factor < inputs.dimension; ++factor) {
        const double current = current_factor_prices[factor];
        const double eigen = eigen_calculator.factorPrices()[factor];
        const double error = std::abs(current - eigen);
        const double scale = std::max({1.0, std::abs(current), std::abs(eigen)});
        if (error > relative_tolerance * scale) {
            throw std::runtime_error("Eigen result differs from the current calculation at factor "
                                     + std::to_string(factor));
        }
    }
}

void warmCurrent(const Inputs& inputs, std::size_t warmup_updates, std::vector<double>& factor_prices) {
    for (std::size_t update = 0; update < warmup_updates; ++update) {
        calculateCurrent(inputs.current_pricing_matrix, inputs.priceUpdate(update), inputs.pricing_multipliers.data(),
                         inputs.dimension, factor_prices.data());
    }
}

void warmEigen(const Inputs& inputs, std::size_t warmup_updates, EigenCalculator& eigen_calculator) {
    for (std::size_t update = 0; update < warmup_updates; ++update) {
        eigen_calculator.calculate(inputs.priceUpdate(update));
    }
}

RunResult benchmarkCurrent(const Inputs& inputs, std::size_t updates, std::vector<double>& factor_prices) {
    double checksum = 0.0;
    const auto start = Clock::now();
    for (std::size_t update = 0; update < updates; ++update) {
        calculateCurrent(inputs.current_pricing_matrix, inputs.priceUpdate(update), inputs.pricing_multipliers.data(),
                         inputs.dimension, factor_prices.data());
        checksum += factor_prices[(update * 17U) % inputs.dimension];
    }
    return {std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start), checksum};
}

RunResult benchmarkEigen(const Inputs& inputs, std::size_t updates, EigenCalculator& eigen_calculator) {
    double checksum = 0.0;
    const auto start = Clock::now();
    for (std::size_t update = 0; update < updates; ++update) {
        eigen_calculator.calculate(inputs.priceUpdate(update));
        checksum += eigen_calculator.factorPrices()[(update * 17U) % inputs.dimension];
    }
    return {std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start), checksum};
}

double medianNanosecondsPerUpdate(const std::vector<RunResult>& results, std::size_t updates) {
    std::vector<double> samples;
    samples.reserve(results.size());
    for (const RunResult& result : results) {
        samples.push_back(static_cast<double>(result.elapsed.count()) / static_cast<double>(updates));
    }
    std::sort(samples.begin(), samples.end());
    const std::size_t middle = samples.size() / 2;
    return samples.size() % 2 == 0 ? (samples[middle - 1] + samples[middle]) / 2.0 : samples[middle];
}

void printResult(std::string_view name, double nanoseconds_per_update) {
    const double updates_per_second = 1'000'000'000.0 / nanoseconds_per_update;
    std::cout << std::left << std::setw(27) << name
              << std::right << std::setw(12) << std::fixed << std::setprecision(2) << nanoseconds_per_update << " ns/update"
              << std::setw(14) << std::setprecision(2) << (updates_per_second / 1'000'000.0) << " M updates/s\n";
}

} // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parseOptions(argc, argv);
        const Inputs inputs(options);
        EigenCalculator eigen_calculator(inputs);
        std::vector<double> current_factor_prices(options.dimension);

        verifyEquivalent(inputs, eigen_calculator);
        warmCurrent(inputs, options.warmup_updates, current_factor_prices);
        warmEigen(inputs, options.warmup_updates, eigen_calculator);

        std::vector<RunResult> current_results;
        std::vector<RunResult> eigen_results;
        current_results.reserve(options.runs);
        eigen_results.reserve(options.runs);

        // Alternate order so one implementation does not always receive the same
        // CPU frequency/cache state advantage.
        for (std::size_t run = 0; run < options.runs; ++run) {
            if (run % 2U == 0U) {
                current_results.push_back(benchmarkCurrent(inputs, options.updates, current_factor_prices));
                eigen_results.push_back(benchmarkEigen(inputs, options.updates, eigen_calculator));
            } else {
                eigen_results.push_back(benchmarkEigen(inputs, options.updates, eigen_calculator));
                current_results.push_back(benchmarkCurrent(inputs, options.updates, current_factor_prices));
            }
        }

        const double current_ns = medianNanosecondsPerUpdate(current_results, options.updates);
        const double eigen_ns = medianNanosecondsPerUpdate(eigen_results, options.updates);

        std::cout << "RelativeValue vector-market-price benchmark\n"
                  << "  dimension: " << options.dimension << " factors x " << options.dimension << " pricing contracts\n"
                  << "  mock price snapshots: " << options.mock_updates << "\n"
                  << "  timed updates per run: " << options.updates << ", runs: " << options.runs << " (median reported)\n"
                  << "  correctness: passed (relative tolerance 1e-12)\n\n";
        printResult("Current nested loops", current_ns);
        printResult("Eigen RowMajor mat-vec", eigen_ns);
        std::cout << "\nEigen speedup: " << std::fixed << std::setprecision(2) << (current_ns / eigen_ns) << "x\n";

        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "Benchmark failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
