#include "results/SummaryPrinter.hpp"
#include <iostream>
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <cmath>

std::string SummaryPrinter::format_latency(double us) const {
    if (us < 1000.0) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(0) << us << "us";
        return oss.str();
    } else if (us < 1'000'000.0) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(1) << (us / 1000.0) << "ms";
        return oss.str();
    } else {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << (us / 1'000'000.0) << "s";
        return oss.str();
    }
}

std::string SummaryPrinter::format_throughput(double ops_sec) const {
    std::ostringstream oss;
    if (ops_sec >= 1'000'000.0) {
        oss << std::fixed << std::setprecision(1) << (ops_sec / 1'000'000.0) << "M ops/s";
    } else if (ops_sec >= 1000.0) {
        oss << std::fixed << std::setprecision(1) << (ops_sec / 1000.0) << "K ops/s";
    } else {
        oss << std::fixed << std::setprecision(0) << ops_sec << " ops/s";
    }
    return oss.str();
}

// Padding a string to a fixed width for table alignment
static std::string pad(const std::string& s, int width) {
    if (static_cast<int>(s.size()) >= width) return s.substr(0, width);
    return s + std::string(width - s.size(), ' ');
}

// print_experiment_header

void SummaryPrinter::print_experiment_header(const std::string& experiment_name) {
    const std::string line(60, '=');
    std::cout << "\n" << line << "\n"
              << "  " << experiment_name << "\n"
              << line << "\n";
}

// print_separator
void SummaryPrinter::print_separator() {
    std::cout << std::string(60, '-') << "\n";
}

// print_result
// Single result printed immediately after an experiment finishes.
void SummaryPrinter::print_result(const BenchmarkResult& result) {
    std::cout
        << "  [" << result.database << "] "
        << result.experiment_name;

    if (!result.phase.empty()) {
        std::cout << " | " << result.phase;
    }
    if (!result.index_type.empty() && result.index_type != "none") {
        std::cout << " | index=" << result.index_type;
    }
    if (result.thread_count > 1) {
        std::cout << " | threads=" << result.thread_count;
    }
    std::cout << "\n";

    std::cout
        << "    ops="         << result.total_ops
        << "  time="          << std::fixed << std::setprecision(1)
                              << result.total_time_ms << "ms"
        << "  p50="           << format_latency(result.p50_us)
        << "  p95="           << format_latency(result.p95_us)
        << "  p99="           << format_latency(result.p99_us)
        << "  tput="          << format_throughput(result.throughput_ops_sec)
        << "\n";

    if (result.sstable_count > 0) {
        std::cout << "    sstables=" << result.sstable_count << "\n";
    }
    if (result.result_set_size > 0) {
        std::cout << "    avg_result_set=" << result.result_set_size << "\n";
    }
    if (!result.notes.empty()) {
        std::cout << "    notes: " << result.notes << "\n";
    }
}

// print_compaction_summary
// Before/after compaction side by side for EXP-06.
void SummaryPrinter::print_compaction_summary(const BenchmarkResult& before,
                                               const BenchmarkResult& after) {
    std::cout << "\n  [cassandra] Compaction Impact:\n";
    std::cout << "  " << std::string(56, '-') << "\n";
    std::cout << "  " << pad("", 20)
              << pad("p50", 10) << pad("p95", 10) << pad("p99", 12)
              << "SSTables\n";
    std::cout << "  " << std::string(56, '-') << "\n";

    std::cout << "  " << pad("Before compaction", 20)
              << pad(format_latency(before.p50_us), 10)
              << pad(format_latency(before.p95_us), 10)
              << pad(format_latency(before.p99_us), 12)
              << before.sstable_count << "\n";

    std::cout << "  " << pad("After compaction", 20)
              << pad(format_latency(after.p50_us), 10)
              << pad(format_latency(after.p95_us), 10)
              << pad(format_latency(after.p99_us), 12)
              << after.sstable_count << "\n";

    std::cout << "  " << std::string(56, '-') << "\n";

    // Show the improvement ratio
    if (before.p99_us > 0 && after.p99_us > 0) {
        double ratio = before.p99_us / after.p99_us;
        std::cout << "  p99 improvement after compaction: "
                  << std::fixed << std::setprecision(1) << ratio << "x\n";
    }
}

// print_write_vs_read_summary
// EXP-07 core output — write vs read side by side with ratio.
void SummaryPrinter::print_write_vs_read_summary(const BenchmarkResult& avg_write,
                                                   const BenchmarkResult& avg_read) {
    std::cout << "\n  [" << avg_write.database
              << "] Write vs Read — averaged over 3 runs\n";

    std::cout << "  " << std::string(66, '-') << "\n";
    std::cout << "  "
              << pad("Operation",   16)
              << pad("Throughput",  16)
              << pad("p50",         10)
              << pad("p95",         10)
              << pad("p99",         12)
              << "\n";
    std::cout << "  " << std::string(66, '-') << "\n";

    std::cout << "  "
              << pad("Write",                                  16)
              << pad(format_throughput(avg_write.throughput_ops_sec), 16)
              << pad(format_latency(avg_write.p50_us),         10)
              << pad(format_latency(avg_write.p95_us),         10)
              << pad(format_latency(avg_write.p99_us),         12)
              << "\n";

    std::cout << "  "
              << pad("Read (cold)",                            16)
              << pad(format_throughput(avg_read.throughput_ops_sec),  16)
              << pad(format_latency(avg_read.p50_us),          10)
              << pad(format_latency(avg_read.p95_us),          10)
              << pad(format_latency(avg_read.p99_us),          12)
              << "\n";

    std::cout << "  " << std::string(66, '-') << "\n";

    // Ratio row — the single most important number in the experiment.
    // > 1.0 means reads are faster than writes (PostgreSQL / MongoDB)
    // < 1.0 means writes are faster than reads (Cassandra — LSM behavior)
    if (avg_write.throughput_ops_sec > 0) {
        double ratio = avg_read.throughput_ops_sec / avg_write.throughput_ops_sec;
        std::cout << "  Read/Write throughput ratio: "
                  << std::fixed << std::setprecision(2) << ratio << "x";
        if (ratio > 1.0) {
            std::cout << "  (reads faster — B+Tree behavior)";
        } else {
            std::cout << "  (writes faster — LSM behavior)";
        }
        std::cout << "\n";
    }

    // Print per-run throughput so variance is visible
    if (!avg_write.notes.empty()) {
        std::cout << "  Write runs: " << avg_write.notes << "\n";
    }
    if (!avg_read.notes.empty()) {
        std::cout << "  Read  runs: " << avg_read.notes  << "\n";
    }
}

// print_final_summary
// Full comparison table at the end — grouped by experiment,
// all three databases side by side.
void SummaryPrinter::print_final_summary(const std::vector<BenchmarkResult>& results) {
    if (results.empty()) return;

    const std::string line(70, '=');
    std::cout << "\n" << line << "\n"
              << "  FINAL SUMMARY\n"
              << line << "\n";

    // Collect unique experiment names in order of first appearance
    std::vector<std::string> exp_names;
    for (const auto& r : results) {
        if (std::find(exp_names.begin(), exp_names.end(),
                      r.experiment_name) == exp_names.end()) {
            exp_names.push_back(r.experiment_name);
        }
    }

    // For each experiment, print all databases side by side
    for (const auto& exp_name : exp_names) {
        std::cout << "\n  " << exp_name << "\n";
        std::cout << "  "
                  << pad("Database",   12)
                  << pad("Operation",  12)
                  << pad("Phase",      20)
                  << pad("p99",        10)
                  << pad("Throughput", 16)
                  << "\n";
        std::cout << "  " << std::string(66, '-') << "\n";

        for (const auto& r : results) {
            if (r.experiment_name != exp_name) continue;
            std::cout << "  "
                      << pad(r.database,    12)
                      << pad(r.operation,   12)
                      << pad(r.phase.empty() ? r.index_type : r.phase, 20)
                      << pad(format_latency(r.p99_us),  10)
                      << pad(format_throughput(r.throughput_ops_sec), 16)
                      << "\n";
        }
    }

    std::cout << "\n" << line << "\n";
}
