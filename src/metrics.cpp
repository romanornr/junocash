// Copyright (c) 2016-2023 The Zcash developers
// Copyright (c) 2025 Juno Cash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

#include "metrics.h"

#include "chainparams.h"
#include "init.h"
#include "checkpoints.h"
#include "main.h"
#include "miner.h"
#include "rpc/server.h"
#include "timedata.h"
#include "ui_interface.h"
#include "util/system.h"
#include "util/time.h"
#include "util/moneystr.h"
#include "util/strencodings.h"
#include "wallet/wallet.h"
#include "crypto/randomx_wrapper.h"

#include <boost/range/irange.hpp>
#include <boost/thread.hpp>
#include <boost/thread/synchronized_value.hpp>

#include <optional>
#include <string>
#include <iostream>
#include <limits>
#include <fstream>
#include <iomanip>
#include <algorithm>
#include <sstream>
#include <cstdio>
#include <vector>
#include <set>
#ifdef WIN32
#include <io.h>
#include <wincon.h>
#include <conio.h>
#else
#include <sys/ioctl.h>
#include <poll.h>
#include <termios.h>
#endif
#include <unistd.h>

// Box-drawing characters (UTF-8)
static const char* BOX_HORIZONTAL = "\xe2\x94\x80";      // ─ (U+2500)
static const char* BOX_VERTICAL = "\xe2\x94\x82";        // │ (U+2502)
static const char* BOX_TOP_LEFT = "\xe2\x94\x8c";        // ┌ (U+250C)
static const char* BOX_TOP_RIGHT = "\xe2\x94\x90";       // ┐ (U+2510)
static const char* BOX_BOTTOM_LEFT = "\xe2\x94\x94";     // └ (U+2514)
static const char* BOX_BOTTOM_RIGHT = "\xe2\x94\x98";    // ┘ (U+2518)
static const char* BOX_VERTICAL_RIGHT = "\xe2\x94\x9c";  // ├ (U+251C)
static const char* BOX_VERTICAL_LEFT = "\xe2\x94\xa4";   // ┤ (U+2524)
static const char* BOX_PROGRESS_FILLED = "\xe2\x96\x88"; // █ (U+2588)
static const char* BOX_PROGRESS_EMPTY = "\xe2\x96\x91";  // ░ (U+2591)
static const char* SYMBOL_CHECK = "\xe2\x9c\x93";        // ✓ (U+2713)
static const char* SYMBOL_CROSS = "\xe2\x9c\x97";        // ✗ (U+2717)

void AtomicTimer::start()
{
    std::unique_lock<std::mutex> lock(mtx);
    if (threads < 1) {
        start_time = GetTime();
    }
    ++threads;
}

void AtomicTimer::stop()
{
    std::unique_lock<std::mutex> lock(mtx);
    // Ignore excess calls to stop()
    if (threads > 0) {
        --threads;
        if (threads < 1) {
            int64_t time_span = GetTime() - start_time;
            total_time += time_span;
        }
    }
}


void AtomicTimer::zeroize()
{
    std::unique_lock<std::mutex> lock(mtx);
    // only zeroize it if there's no more threads (same semantics as start())
    if (threads < 1) {
        start_time = 0;
        total_time = 0;
    }
}

bool AtomicTimer::running()
{
    std::unique_lock<std::mutex> lock(mtx);
    return threads > 0;
}

uint64_t AtomicTimer::threadCount()
{
    std::unique_lock<std::mutex> lock(mtx);
    return threads;
}

double AtomicTimer::rate(const AtomicCounter& count)
{
    std::unique_lock<std::mutex> lock(mtx);
    int64_t duration = total_time;
    if (threads > 0) {
        // Timer is running, so get the latest count
        duration += GetTime() - start_time;
    }
    return duration > 0 ? (double)count.get() / duration : 0;
}

static CCriticalSection cs_metrics;

static boost::synchronized_value<int64_t> nNodeStartTime;
static boost::synchronized_value<int64_t> nNextRefresh;
AtomicCounter transactionsValidated;
AtomicCounter ehSolverRuns;
AtomicCounter solutionTargetChecks;
static AtomicCounter minedBlocks;
AtomicTimer miningTimer;
std::atomic<size_t> nSizeReindexed(0);   // valid only during reindex
std::atomic<size_t> nFullSizeToReindex(1);   // valid only during reindex

static boost::synchronized_value<std::list<uint256>> trackedBlocks;

static boost::synchronized_value<std::list<std::string>> messageBox;
static boost::synchronized_value<std::string> initMessage;
static bool loaded = false;

// Benchmarking mode globals
std::atomic<bool> benchmarkMode(false);
std::atomic<int> benchmarkCurrentThreads(1);
std::atomic<int> benchmarkMaxThreads(0);
std::atomic<bool> benchmarkTestLight(true);
std::atomic<bool> benchmarkTestFast(true);
std::atomic<bool> benchmarkTestHugepages(true);
static std::atomic<int64_t> benchmarkStartTime(0);
static std::atomic<int> benchmarkStartHeight(0);  // Track starting block height for benchmark duration logging
static std::atomic<double> benchmarkAccumulatedHashrate(0.0);
static std::atomic<int> benchmarkSampleCount(0);
static std::atomic<bool> benchmarkWarmingUp(false);  // Track if currently in warmup phase
static const int BENCHMARK_DURATION_SECONDS = 20; // Mine for 20 seconds per thread count
static boost::thread* benchmarkThread = nullptr;
static std::atomic<bool> benchmarkAutoApply(false); // Auto-apply optimal threads to config

// Regular mining warmup tracking
static std::atomic<int64_t> miningStartTime(0);  // Track when mining started for warmup detection
static const int MINING_WARMUP_SECONDS = 10;  // Show warmup status for first 10 seconds

// Store benchmark results
struct BenchmarkResult {
    int threads;
    std::string mode;  // "Light", "Fast", or "Fast+Hugepages"
    double hashrate;
    int samples;
};
static std::vector<BenchmarkResult> benchmarkResults;

extern int64_t GetNetworkHashPS(int lookup, int height);

void TrackMinedBlock(uint256 hash)
{
    LOCK(cs_metrics);
    minedBlocks.increment();
    trackedBlocks->push_back(hash);
}

void MarkStartTime()
{
    *nNodeStartTime = GetTime();
}

int64_t GetUptime()
{
    return GetTime() - *nNodeStartTime;
}

// Check if hugepages are enabled and available
static bool IsHugepagesEnabled()
{
#ifndef WIN32
    // Check 1GB hugepages first
    std::ifstream hugepages_1g("/sys/kernel/mm/hugepages/hugepages-1048576kB/free_hugepages");
    if (hugepages_1g.is_open()) {
        int free_1g = 0;
        hugepages_1g >> free_1g;
        if (free_1g > 0) {
            return true;
        }
    }

    // Check 2MB hugepages
    std::ifstream hugepages_2m("/sys/kernel/mm/hugepages/hugepages-2048kB/free_hugepages");
    if (hugepages_2m.is_open()) {
        int free_2m = 0;
        hugepages_2m >> free_2m;
        if (free_2m > 0) {
            return true;
        }
    }
#endif
    return false;
}

double GetLocalSolPS()
{
    return miningTimer.rate(solutionTargetChecks);
}

std::string WhichNetwork()
{
    if (GetBoolArg("-regtest", false))
        return "regtest";
    if (GetBoolArg("-testnet", false))
        return "testnet";
    return "mainnet";
}

int EstimateNetHeight(const Consensus::Params& params, int currentHeadersHeight, int64_t currentHeadersTime)
{
    int64_t now = GetTime();
    if (currentHeadersTime >= now) {
        return currentHeadersHeight;
    }

    int estimatedHeight = currentHeadersHeight + (now - currentHeadersTime) / params.PoWTargetSpacing(currentHeadersHeight);

    int blossomActivationHeight = params.vUpgrades[Consensus::UPGRADE_NU6_1].nActivationHeight;
    if (currentHeadersHeight >= blossomActivationHeight || estimatedHeight <= blossomActivationHeight) {
        return ((estimatedHeight + 5) / 10) * 10;
    }

    int numPreBlossomBlocks = blossomActivationHeight - currentHeadersHeight;
    int64_t preBlossomTime = numPreBlossomBlocks * params.PoWTargetSpacing(blossomActivationHeight - 1);
    int64_t blossomActivationTime = currentHeadersTime + preBlossomTime;
    if (blossomActivationTime >= now) {
        return blossomActivationHeight;
    }

    int netheight =  blossomActivationHeight + (now - blossomActivationTime) / params.PoWTargetSpacing(blossomActivationHeight);
    return ((netheight + 5) / 10) * 10;
}

void TriggerRefresh()
{
    *nNextRefresh = GetTime();
    // Ensure that the refresh has started before we return
    MilliSleep(200);
}

static bool metrics_ThreadSafeMessageBox(const std::string& message,
                                      const std::string& caption,
                                      unsigned int style)
{
    // The SECURE flag has no effect in the metrics UI.
    style &= ~CClientUIInterface::SECURE;

    std::string strCaption;
    // Check for usage of predefined caption
    switch (style) {
    case CClientUIInterface::MSG_ERROR:
        strCaption += _("Error");
        break;
    case CClientUIInterface::MSG_WARNING:
        strCaption += _("Warning");
        break;
    case CClientUIInterface::MSG_INFORMATION:
        strCaption += _("Information");
        break;
    default:
        strCaption += caption; // Use supplied caption (can be empty)
    }

    boost::strict_lock_ptr<std::list<std::string>> u = messageBox.synchronize();
    u->push_back(strCaption + ": " + message);
    if (u->size() > 5) {
        u->pop_back();
    }

    TriggerRefresh();
    return false;
}

static bool metrics_ThreadSafeQuestion(const std::string& /* ignored interactive message */, const std::string& message, const std::string& caption, unsigned int style)
{
    return metrics_ThreadSafeMessageBox(message, caption, style);
}

static void metrics_InitMessage(const std::string& message)
{
    *initMessage = message;
}

void ConnectMetricsScreen()
{
    uiInterface.ThreadSafeMessageBox.disconnect_all_slots();
    uiInterface.ThreadSafeMessageBox.connect(metrics_ThreadSafeMessageBox);
    uiInterface.ThreadSafeQuestion.disconnect_all_slots();
    uiInterface.ThreadSafeQuestion.connect(metrics_ThreadSafeQuestion);
    uiInterface.InitMessage.disconnect_all_slots();
    uiInterface.InitMessage.connect(metrics_InitMessage);
}

std::string DisplayDuration(int64_t duration, DurationFormat format)
{
    int64_t days =  duration / (24 * 60 * 60);
    int64_t hours = (duration - (days * 24 * 60 * 60)) / (60 * 60);
    int64_t minutes = (duration - (((days * 24) + hours) * 60 * 60)) / 60;
    int64_t seconds = duration - (((((days * 24) + hours) * 60) + minutes) * 60);

    std::string strDuration;
    if (format == DurationFormat::REDUCED) {
        if (days > 0) {
            strDuration = strprintf(_("%d days"), days);
        } else if (hours > 0) {
            strDuration = strprintf(_("%d hours"), hours);
        } else if (minutes > 0) {
            strDuration = strprintf(_("%d minutes"), minutes);
        } else {
            strDuration = strprintf(_("%d seconds"), seconds);
        }
    } else {
        if (days > 0) {
            strDuration = strprintf(_("%d days, %d hours, %d minutes, %d seconds"), days, hours, minutes, seconds);
        } else if (hours > 0) {
            strDuration = strprintf(_("%d hours, %d minutes, %d seconds"), hours, minutes, seconds);
        } else if (minutes > 0) {
            strDuration = strprintf(_("%d minutes, %d seconds"), minutes, seconds);
        } else {
            strDuration = strprintf(_("%d seconds"), seconds);
        }
    }
    return strDuration;
}

std::string DisplaySize(size_t value)
{
    double coef = 1.0;
    if (value < 1024.0 * coef)
        return strprintf(_("%d Bytes"), value);
    coef *= 1024.0;
    if (value < 1024.0 * coef)
        return strprintf(_("%.2f KiB"), value / coef);
    coef *= 1024.0;
    if (value < 1024.0 * coef)
        return strprintf(_("%.2f MiB"), value / coef);
    coef *= 1024.0;
    if (value < 1024.0 * coef)
        return strprintf(_("%.2f GiB"), value / coef);
    coef *= 1024.0;
    return strprintf(_("%.2f TiB"), value / coef);
}

std::string DisplayHashRate(double value)
{
    double coef = 1.0;
    if (value < 1000.0 * coef)
        return strprintf(_("%.3f H/s"), value);
    coef *= 1000.0;
    if (value < 1000.0 * coef)
        return strprintf(_("%.3f kH/s"), value / coef);
    coef *= 1000.0;
    if (value < 1000.0 * coef)
        return strprintf(_("%.3f MH/s"), value / coef);
    coef *= 1000.0;
    if (value < 1000.0 * coef)
        return strprintf(_("%.3f GH/s"), value / coef);
    coef *= 1000.0;
    return strprintf(_("%.3f TH/s"), value / coef);
}

std::optional<int64_t> SecondsLeftToNextEpoch(const Consensus::Params& params, int currentHeight)
{
    auto nextHeight = NextActivationHeight(currentHeight, params);
    if (nextHeight) {
        return (nextHeight.value() - currentHeight) * params.PoWTargetSpacing(nextHeight.value() - 1);
    } else {
        return std::nullopt;
    }
}

struct MetricsStats {
    int height;
    int64_t currentHeadersHeight;
    int64_t currentHeadersTime;
    size_t connections;
    int64_t netsolps;
};

MetricsStats loadStats()
{
    int height;
    int64_t currentHeadersHeight;
    int64_t currentHeadersTime;
    size_t connections;
    int64_t netsolps;

    {
        LOCK(cs_main);
        height = chainActive.Height();
        currentHeadersHeight = pindexBestHeader ? pindexBestHeader->nHeight: -1;
        currentHeadersTime = pindexBestHeader ? pindexBestHeader->nTime : 0;
        netsolps = GetNetworkHashPS(120, -1);
    }
    {
        LOCK(cs_vNodes);
        connections = vNodes.size();
    }

    return MetricsStats {
        height,
        currentHeadersHeight,
        currentHeadersTime,
        connections,
        netsolps
    };
}

// ============================================================================
// Beautiful UI Helper Functions
// ============================================================================

// Calculate visible length of string (excluding ANSI escape codes)
// Counts UTF-8 characters, not bytes
static size_t visibleLength(const std::string& str) {
    size_t len = 0;
    bool inEscape = false;
    for (size_t i = 0; i < str.length(); ) {
        unsigned char c = str[i];

        if (c == '\e') {
            inEscape = true;
            i++;
        } else if (inEscape && c == 'm') {
            inEscape = false;
            i++;
        } else if (!inEscape) {
            // Count this as one character and skip UTF-8 continuation bytes
            len++;
            // UTF-8: if byte starts with 11xxxxxx, count following 10xxxxxx bytes
            if ((c & 0x80) == 0) {
                i++; // ASCII (0xxxxxxx)
            } else if ((c & 0xE0) == 0xC0) {
                i += 2; // 2-byte UTF-8 (110xxxxx 10xxxxxx)
            } else if ((c & 0xF0) == 0xE0) {
                i += 3; // 3-byte UTF-8 (1110xxxx 10xxxxxx 10xxxxxx)
            } else if ((c & 0xF8) == 0xF0) {
                i += 4; // 4-byte UTF-8 (11110xxx 10xxxxxx 10xxxxxx 10xxxxxx)
            } else {
                i++; // Invalid UTF-8, just skip
            }
        } else {
            i++;
        }
    }
    return len;
}

// Draw a horizontal line with optional title
static void drawLine(const std::string& title, const char* left, const char* right, const char* fill, int width = 72) {
    std::cout << left;
    if (!title.empty()) {
        int titleLen = title.length() + 2; // +2 for spaces
        int leftPad = (width - titleLen) / 2;
        int rightPad = width - titleLen - leftPad;
        for (int i = 0; i < leftPad; i++) std::cout << fill;
        std::cout << " \e[1;37m" << title << "\e[0m ";
        for (int i = 0; i < rightPad; i++) std::cout << fill;
    } else {
        for (int i = 0; i < width; i++) std::cout << fill;
    }
    std::cout << right << std::endl;
}

// Draw top border of box
static void drawBoxTop(const std::string& title = "", int width = 78) {
    drawLine(title, BOX_TOP_LEFT, BOX_TOP_RIGHT, BOX_HORIZONTAL, width);
}

// Draw bottom border of box
static void drawBoxBottom(int width = 78) {
    drawLine("", BOX_BOTTOM_LEFT, BOX_BOTTOM_RIGHT, BOX_HORIZONTAL, width);
}

// Draw a data row inside a box with label and value
static void drawRow(const std::string& label, const std::string& value, int width = 78) {
    int labelLen = visibleLength(label);
    int valueLen = visibleLength(value);
    int padding = width - labelLen - valueLen - 2; // -2 for the two spaces (after | and before |)

    std::cout << BOX_VERTICAL << " \e[1;36m" << label << "\e[0m";
    for (int i = 0; i < padding; i++) std::cout << " ";
    std::cout << "\e[1;33m" << value << "\e[0m " << BOX_VERTICAL << std::endl;
}

// Draw a centered text line in a box
static void drawCentered(const std::string& text, const std::string& color = "", int width = 78) {
    int textLen = visibleLength(text);
    int padding = (width - textLen) / 2;
    int rightPad = width - textLen - padding;

    std::cout << BOX_VERTICAL;
    for (int i = 0; i < padding; i++) std::cout << " ";
    if (!color.empty()) std::cout << color;
    std::cout << text;
    if (!color.empty()) std::cout << "\e[0m";
    for (int i = 0; i < rightPad; i++) std::cout << " ";
    std::cout << BOX_VERTICAL << std::endl;
}

// Draw a progress bar
static void drawProgressBar(int percent, int width = 74) {
    int filled = (percent * width) / 100;
    std::cout << BOX_VERTICAL << " \e[1;32m";
    for (int i = 0; i < filled; i++) std::cout << BOX_PROGRESS_FILLED;
    std::cout << "\e[0;32m";
    for (int i = filled; i < width; i++) std::cout << BOX_PROGRESS_EMPTY;
    std::cout << "\e[0m " << BOX_VERTICAL << std::endl;
}

int printStats(MetricsStats stats, bool isScreen, bool mining)
{
    int lines = 0;
    const Consensus::Params& params = Params().GetConsensus();
    auto localsolps = GetLocalSolPS();

    // Network Status Box
    drawBoxTop("NETWORK STATUS");
    lines++;

    // Syncing or synced status
    if (IsInitialBlockDownload(Params().GetConsensus())) {
        if (fReindex) {
            int downloadPercent = nSizeReindexed * 100 / nFullSizeToReindex;
            drawRow("Status", strprintf("Reindexing (%d%%)", downloadPercent));
            lines++;
            drawRow("Progress", strprintf("%s / %s",
                DisplaySize(nSizeReindexed), DisplaySize(nFullSizeToReindex)));
            lines++;
            drawRow("Blocks", strprintf("%d", stats.height));
            lines++;

            if (isScreen) {
                drawProgressBar(downloadPercent);
                lines++;
            }
        } else {
            int nHeaders = stats.currentHeadersHeight < 0 ? 0 : stats.currentHeadersHeight;
            int netheight = stats.currentHeadersHeight == -1 || stats.currentHeadersTime == 0 ?
                0 : EstimateNetHeight(params, stats.currentHeadersHeight, stats.currentHeadersTime);
            if (netheight < nHeaders) netheight = nHeaders;
            if (netheight <= 0) netheight = 1;
            int downloadPercent = stats.height * 100 / netheight;

            drawRow("Status", strprintf("\e[1;33mSYNCING\e[0m (%d%%)", downloadPercent));
            lines++;
            drawRow("Block Height", strprintf("%d / %d", stats.height, netheight));
            lines++;

            // Network Difficulty
            double difficulty = GetNetworkDifficulty(chainActive.Tip());
            drawRow("Network Difficulty", strprintf("%.6f", difficulty));
            lines++;

            if (isScreen) {
                drawProgressBar(downloadPercent);
                lines++;
            }
        }
    } else {
        drawRow("Status", "\e[1;32m● SYNCHRONIZED\e[0m");
        lines++;
        drawRow("Block Height", strprintf("%d", stats.height));
        lines++;
    }

    // Network Difficulty
    double difficulty = GetNetworkDifficulty(chainActive.Tip());
    drawRow("Network Difficulty", strprintf("%.6f", difficulty));
    lines++;

    // Network info
    auto secondsLeft = SecondsLeftToNextEpoch(params, stats.height);
    if (secondsLeft) {
        auto nextHeight = NextActivationHeight(stats.height, params).value();
        auto nextBranch = NextEpoch(stats.height, params).value();
        drawRow("Next Upgrade", strprintf("%s at %d (~%s)",
            NetworkUpgradeInfo[nextBranch].strName, nextHeight,
            DisplayDuration(secondsLeft.value(), DurationFormat::REDUCED)));
    } else {
        drawRow("Next Upgrade", "None scheduled");
    }
    lines++;

    drawRow("Connections", strprintf("%d", stats.connections));
    lines++;
    drawRow("Network Hash", DisplayHashRate(stats.netsolps));
    lines++;

    if (mining && miningTimer.running()) {
        drawRow("Your Hash Rate", DisplayHashRate(localsolps));
        lines++;
    }

    drawBoxBottom();
    lines++;
    std::cout << std::endl;
    lines++;

    return lines;
}


// Forward declarations for donation helper functions
static int getCurrentDonationPercentage();
static std::string getCurrentDonationAddress();

int printWalletStatus()
{
    int lines = 0;

    // Wallet Balance Box
    drawBoxTop("WALLET");
    lines++;

    if (pwalletMain) {
        CAmount immature = pwalletMain->GetImmatureBalance(std::nullopt);
        CAmount mature = pwalletMain->GetBalance(std::nullopt);
        std::string units = Params().CurrencyUnits();

        drawRow("Mature Balance", strprintf("%s %s", FormatMoney(mature), units.c_str()));
        lines++;
        drawRow("Immature Balance", strprintf("%s %s", FormatMoney(immature), units.c_str()));
        lines++;

        // Show blocks mined if any
        int blocksMined = minedBlocks.get();
        if (blocksMined > 0) {
            int orphaned = 0;
            {
                LOCK2(cs_main, cs_metrics);
                boost::strict_lock_ptr<std::list<uint256>> u = trackedBlocks.synchronize();

                // Update orphaned block count
                std::list<uint256>::iterator it = u->begin();
                while (it != u->end()) {
                    auto hash = *it;
                    if (mapBlockIndex.count(hash) > 0 &&
                            chainActive.Contains(mapBlockIndex[hash])) {
                        it++;
                    } else {
                        it = u->erase(it);
                    }
                }

                orphaned = blocksMined - u->size();
            }

            drawRow("Blocks Mined", strprintf("%d (orphaned: %d)", blocksMined, orphaned));
            lines++;
        }
    } else {
        drawRow("Status", "Wallet not loaded");
        lines++;
    }

    drawBoxBottom();
    lines++;
    std::cout << std::endl;
    lines++;

    return lines;
}

int printMiningStatus(bool mining)
{
#ifdef ENABLE_MINING
    int lines = 0;

    // Mining Status Box
    drawBoxTop("MINING");
    lines++;

    if (mining) {
        auto nThreads = miningTimer.threadCount();
        if (nThreads > 0) {
            // Check if in warmup phase (first 10 seconds after mining starts, or hashrate is 0)
            int64_t startTime = miningStartTime.load();
            bool isWarmingUp = false;
            if (startTime > 0) {
                int64_t elapsedTime = GetTime() - startTime;
                double currentHashrate = GetLocalSolPS();
                isWarmingUp = (elapsedTime < MINING_WARMUP_SECONDS && currentHashrate == 0);
            }

            if (isWarmingUp) {
                drawRow("Status", strprintf("\e[1;33m● WARMING UP\e[0m - %d threads", nThreads));
            } else {
                drawRow("Status", strprintf("\e[1;32m● ACTIVE\e[0m - %d threads", nThreads));
            }
            lines++;

            // Show block reward
            int nHeight = chainActive.Height() + 1; // Next block to be mined
            CAmount blockReward = Params().GetConsensus().GetBlockSubsidy(nHeight);
            drawRow("Block Reward", FormatMoney(blockReward));
            lines++;

            // Show RandomX mining mode
            bool isFastMode = RandomX_IsFastMode();
            bool hugepagesInUse = RandomX_IsUsingHugepages();
            std::string miningMode;

            if (isFastMode) {
                if (hugepagesInUse) {
                    miningMode = "\e[1;32mFast + Hugepages\e[0m";
                } else {
                    miningMode = "\e[1;36mFast\e[0m";
                }
            } else {
                miningMode = "\e[1;33mLight\e[0m";
            }

            drawRow("Mining Mode", miningMode);
            lines++;

            // Show benchmark status if active
            if (benchmarkMode.load()) {
                int currentThread = benchmarkCurrentThreads.load();
                int maxThread = benchmarkMaxThreads.load();

                std::string benchStatus;
                if (benchmarkWarmingUp.load()) {
                    benchStatus = strprintf("\e[1;33m● WARMING UP\e[0m - Thread %d/%d",
                        currentThread, maxThread);
                } else {
                    double currentHashrate = GetLocalSolPS();
                    benchStatus = strprintf("\e[1;36m● RUNNING\e[0m - Thread %d/%d (%.1f H/s)",
                        currentThread, maxThread, currentHashrate);
                }
                drawRow("Benchmark", benchStatus);
                lines++;
            }
        } else {
            bool fvNodesEmpty;
            {
                LOCK(cs_vNodes);
                fvNodesEmpty = vNodes.empty();
            }
            if (fvNodesEmpty) {
                drawRow("Status", "\e[1;33m○ PAUSED\e[0m - Waiting for connections");
            } else if (IsInitialBlockDownload(Params().GetConsensus())) {
                drawRow("Status", "\e[1;33m○ PAUSED\e[0m - Downloading blocks");
            } else {
                drawRow("Status", "\e[1;33m○ PAUSED\e[0m - Processing");
            }
            lines++;
        }

        // Show donation status if active
        int donationPct = getCurrentDonationPercentage();
        if (donationPct > 0) {
            std::string donationAddr = getCurrentDonationAddress();
            if (!donationAddr.empty()) {
                std::string shortAddr;
                if (donationAddr.length() > 20) {
                    shortAddr = donationAddr.substr(0, 10) + "..." + donationAddr.substr(donationAddr.length() - 6);
                } else {
                    shortAddr = donationAddr;
                }
                drawRow("Donations", strprintf("\e[1;35m%d%%\e[0m → %s", donationPct, shortAddr.c_str()));
            } else {
                drawRow("Donations", strprintf("\e[1;35m%d%%\e[0m → \e[1;31mNO ADDRESS SET\e[0m", donationPct));
            }
            lines++;
        }
    } else {
        drawRow("Status", "\e[1;31m○ INACTIVE\e[0m");
        lines++;
    }

    drawBoxBottom();
    lines++;
    std::cout << std::endl;
    lines++;

    // Controls Box
    drawBoxTop("CONTROLS");
    lines++;

    if (mining) {
        // Get current thread count (use benchmark current threads if in benchmark mode)
        int nThreads;
        if (benchmarkMode.load()) {
            nThreads = benchmarkCurrentThreads.load();
        } else {
            nThreads = GetArg("-genproclimit", 1);
        }

        std::string controls = strprintf("\e[1;37m[M]\e[0m Mining: \e[1;32mON\e[0m  \e[1;37m[T]\e[0m Threads: %d", nThreads);

        int donationPct = getCurrentDonationPercentage();
        if (donationPct > 0) {
            // Donations are ON, show current state and controls
            controls += strprintf("  \e[1;37m[D]\e[0m Donations: \e[1;35mON (%d%%)\e[0m  \e[1;37m[P]\e[0m Change %%", donationPct);
        } else {
            // Donations are OFF, show current state
            controls += "  \e[1;37m[D]\e[0m Donations: \e[1;31mOFF\e[0m";
        }

        controls += "  \e[1;37m[B]\e[0m Benchmark  \e[1;37m[Q]\e[0m Quit";
        drawCentered(controls);
    } else {
        drawCentered("\e[1;37m[M]\e[0m Mining: \e[1;31mOFF\e[0m  \e[1;37m[Q]\e[0m Quit");
    }
    lines++;

    drawBoxBottom();
    lines++;

    return lines;
#else // ENABLE_MINING
    return 0;
#endif // !ENABLE_MINING
}


int printMetrics(size_t cols, bool mining)
{
    // Number of lines that are always displayed
    int lines = 2;

    // Calculate and display uptime
    std::string duration = DisplayDuration(GetUptime(), DurationFormat::FULL);

    std::string strDuration = strprintf(_("Uptime: %s"), duration);
    std::cout << strDuration << std::endl;
    lines += (strDuration.size() / cols);

    if (mining && loaded) {
        std::cout << "- " << strprintf(_("You have completed %d RandomX hashes."), ehSolverRuns.get()) << std::endl;
        lines++;
    }
    std::cout << std::endl;

    return lines;
}

int printMessageBox(size_t cols)
{
    boost::strict_lock_ptr<std::list<std::string>> u = messageBox.synchronize();

    if (u->size() == 0) {
        return 0;
    }

    int lines = 2 + u->size();
    std::cout << _("Messages:") << std::endl;
    for (auto it = u->cbegin(); it != u->cend(); ++it) {
        auto msg = FormatParagraph(*it, cols, 2);
        std::cout << "- " << msg << std::endl;
        // Handle newlines and wrapped lines
        size_t i = 0;
        size_t j = 0;
        while (j < msg.size()) {
            i = msg.find('\n', j);
            if (i == std::string::npos) {
                i = msg.size();
            } else {
                // Newline
                lines++;
            }
            j = i + 1;
        }
    }
    std::cout << std::endl;
    return lines;
}

int printInitMessage()
{
    if (loaded) {
        return 0;
    }

    std::string msg = *initMessage;
    std::cout << _("Node is starting up:") << " " << msg << std::endl;
    std::cout << std::endl;

    if (msg == _("Done loading")) {
        loaded = true;
    }

    return 2;
}

#ifdef WIN32
bool enableVTMode()
{
    // Set output mode to handle virtual terminal sequences
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut == INVALID_HANDLE_VALUE) {
        return false;
    }

    DWORD dwMode = 0;
    if (!GetConsoleMode(hOut, &dwMode)) {
        return false;
    }

    dwMode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    if (!SetConsoleMode(hOut, dwMode)) {
        return false;
    }

    // Enable UTF-8 output for box-drawing characters (Windows 10 1903+)
    SetConsoleOutputCP(CP_UTF8);

    return true;
}
#endif

// Helper function to check for keyboard input without blocking
static int checkKeyPress()
{
#ifdef WIN32
    if (_kbhit()) {
        return _getch();
    }
    return 0;
#else
    struct pollfd fds;
    fds.fd = STDIN_FILENO;
    fds.events = POLLIN;

    int ret = poll(&fds, 1, 0);  // 0 timeout = non-blocking
    if (ret > 0 && (fds.revents & POLLIN)) {
        char c;
        if (read(STDIN_FILENO, &c, 1) == 1) {
            return c;
        }
    }
    return 0;
#endif
}

// Terminal mode management for input prompts
#ifndef WIN32
static struct termios orig_termios;
static bool termios_saved = false;

static void disableRawMode()
{
    if (termios_saved) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    }
}

static void enableRawMode()
{
    if (!termios_saved) {
        tcgetattr(STDIN_FILENO, &orig_termios);
        termios_saved = true;
        atexit(disableRawMode);
    }

    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ICANON | ECHO);  // Disable canonical mode and echo
    raw.c_cc[VMIN] = 0;   // Non-blocking read
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

static void enableCanonicalMode()
{
    if (termios_saved) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    }
}
#endif

// Get current donation percentage
static int getCurrentDonationPercentage()
{
    return GetArg("-donationpercentage", 0);
}

// Get current donation address
static std::string getCurrentDonationAddress()
{
    std::string donationAddr = GetArg("-donationaddress", "");

    // Use default address from chain params if not specified
    if (donationAddr.empty()) {
        donationAddr = Params().GetDefaultDonationAddress();
    }

    return donationAddr;
}

// Update donation percentage
static void updateDonationPercentage(int percentage)
{
    if (percentage < 0 || percentage > 100) {
        return;  // Invalid range
    }

    mapArgs["-donationpercentage"] = itostr(percentage);

    if (percentage > 0) {
        std::string devAddress = getCurrentDonationAddress();
        LogPrintf("User set donation to %d%% (address: %s)\n", percentage, devAddress);
    } else {
        LogPrintf("User disabled donations\n");
    }
}

// Toggle donation on/off
static void toggleDonation()
{
    int current = getCurrentDonationPercentage();
    if (current > 0) {
        // Turn off
        updateDonationPercentage(0);
    } else {
        // Check if donation address is available (will use defaults on mainnet/testnet)
        std::string donationAddr = getCurrentDonationAddress();
        if (donationAddr.empty()) {
            // This can only happen on regtest without explicit address
            LogPrintf("Cannot enable donations: no -donationaddress configured (regtest requires explicit address)\n");
            return;
        }
        // Turn on with default 5%
        updateDonationPercentage(5);
    }
}

// Prompt user for donation percentage (with fixed screen positioning)
static void promptForPercentage(int screenHeight)
{
#ifndef WIN32
    enableCanonicalMode();
#endif

    // Use dedicated input area at bottom of screen (3 rows from bottom)
    int inputRow = screenHeight - 2;

    // Clear input area
    std::cout << "\e[" << inputRow << ";1H\e[K";
    std::cout << "Enter donation percentage (0-100): " << std::flush;

    std::string input;
    std::getline(std::cin, input);

    try {
        int percentage = std::stoi(input);
        if (percentage >= 0 && percentage <= 100) {
            updateDonationPercentage(percentage);
            std::cout << "\e[" << inputRow << ";1H\e[K";  // Clear and reposition
            if (percentage == 0) {
                std::cout << SYMBOL_CHECK << " Donations disabled" << std::flush;
            } else {
                std::cout << SYMBOL_CHECK << " Donation set to " << percentage << "%" << std::flush;
            }
        } else {
            std::cout << "\e[" << inputRow << ";1H\e[K";
            std::cout << SYMBOL_CROSS << " Invalid percentage (must be 0-100)" << std::flush;
        }
    } catch (...) {
        std::cout << "\e[" << inputRow << ";1H\e[K";
        std::cout << SYMBOL_CROSS << " Invalid input (not a number)" << std::flush;
    }

    // Brief pause to show confirmation, then clear
    MilliSleep(800);
    std::cout << "\e[" << inputRow << ";1H\e[K" << std::flush;

#ifndef WIN32
    enableRawMode();
#endif
}

// Toggle mining on/off
static void toggleMining()
{
    bool currentlyMining = GetBoolArg("-gen", false);
    mapArgs["-gen"] = currentlyMining ? "0" : "1";

    int nThreads = GetArg("-genproclimit", 1);
    GenerateBitcoins(!currentlyMining, nThreads, Params());

    if (!currentlyMining) {
        miningStartTime = GetTime();  // Track start time for warmup display
        LogPrintf("User enabled mining with %d threads\n", nThreads);
    } else {
        miningStartTime = 0;  // Clear start time when mining stops
        LogPrintf("User disabled mining\n");
    }
}

// Store original thread count before benchmark
static std::atomic<int> benchmarkOriginalThreads(0);

// Write optimal thread count to config file
static bool writeOptimalThreadsToConfig(int threads, const std::string& mode = "")
{
    try {
        // Get config file path
        boost::filesystem::path configPath = GetConfigFile(GetArg("-conf", BITCOIN_CONF_FILENAME));

        // Read existing config file and filter out old benchmark settings
        std::vector<std::string> existingLines;
        std::ifstream configFileRead(configPath.string());
        if (configFileRead.is_open()) {
            std::string line;
            bool skipNextLines = false;
            while (std::getline(configFileRead, line)) {
                // Skip benchmark comment and subsequent settings
                if (line.find("# Optimal configuration determined by benchmark") != std::string::npos) {
                    skipNextLines = true;
                    continue;
                }

                // Skip old benchmark-related settings
                if (skipNextLines) {
                    if (line.find("randomxfastmode=") == 0 ||
                        line.find("randomxhugepages=") == 0 ||
                        line.find("genproclimit=") == 0 ||
                        line.find("gen=") == 0 ||
                        line.find("# Best mode:") == 0 ||
                        line.empty()) {
                        continue;  // Skip these lines
                    }
                    skipNextLines = false;  // Resume keeping lines
                }

                // Also remove any standalone duplicates from earlier runs
                if (line.find("randomxfastmode=") == 0 ||
                    line.find("randomxhugepages=") == 0 ||
                    line.find("genproclimit=") == 0 ||
                    line.find("gen=") == 0) {
                    continue;  // Skip duplicates
                }

                existingLines.push_back(line);
            }
            configFileRead.close();
        }

        // Write back the filtered config plus new benchmark settings
        std::ofstream configFile(configPath.string(), std::ios::trunc);
        if (!configFile.is_open()) {
            LogPrintf("ERROR: Could not open config file for writing: %s\n", configPath.string());
            return false;
        }

        // Write existing non-benchmark lines
        for (const auto& line : existingLines) {
            configFile << line << "\n";
        }

        // Write the optimal settings with timestamp
        configFile << "\n# Optimal configuration determined by benchmark on "
                   << DateTimeStrFormat("%Y-%m-%d %H:%M:%S", GetTime()) << "\n";

        if (!mode.empty()) {
            configFile << "# Best mode: " << mode << "\n";

            // Set RandomX mode flags based on best mode
            if (mode == "Fast" || mode == "Fast+Hugepages") {
                configFile << "randomxfastmode=1\n";
            } else {
                configFile << "randomxfastmode=0\n";
            }

            if (mode == "Fast+Hugepages") {
                configFile << "randomxhugepages=1\n";
            } else {
                configFile << "randomxhugepages=0\n";
            }
        }

        configFile << "genproclimit=" << threads << "\n";
        configFile << "gen=1\n";  // Enable mining on startup
        configFile.close();

        LogPrintf("Wrote optimal config (mode=%s, threads=%d, gen=1) to: %s\n", mode, threads, configPath.string());
        return true;
    } catch (const std::exception& e) {
        LogPrintf("ERROR: Failed to write to config file: %s\n", e.what());
        return false;
    }
}

// Prompt user for which modes to benchmark
static void promptBenchmarkModes(int screenHeight, bool& testLight, bool& testFast, bool& testHugepages)
{
#ifndef WIN32
    enableCanonicalMode();
#endif

    // Use dedicated input area at bottom of screen
    int inputRow = screenHeight - 2;

    // Clear input area and show prompt
    std::cout << "\e[" << inputRow << ";1H\e[K";
    std::cout << "Benchmark modes - [A]ll [L]ight [F]ast [H]ugepages [FH]ast+Hugepages (default: A): " << std::flush;

    std::string input;
    std::getline(std::cin, input);

    // Convert to uppercase
    std::transform(input.begin(), input.end(), input.begin(), ::toupper);

    // Determine which modes to test
    testLight = false;
    testFast = false;
    testHugepages = false;

    if (input.empty() || input == "A") {
        testLight = true;
        testFast = true;
        testHugepages = true;
        std::cout << "\e[" << inputRow << ";1H\e[K";
        std::cout << SYMBOL_CHECK << " Will test all modes: Light, Fast, Fast+Hugepages" << std::flush;
    } else if (input == "L") {
        testLight = true;
        std::cout << "\e[" << inputRow << ";1H\e[K";
        std::cout << SYMBOL_CHECK << " Will test Light mode only" << std::flush;
    } else if (input == "F") {
        testFast = true;
        std::cout << "\e[" << inputRow << ";1H\e[K";
        std::cout << SYMBOL_CHECK << " Will test Fast mode only" << std::flush;
    } else if (input == "H") {
        testHugepages = true;
        std::cout << "\e[" << inputRow << ";1H\e[K";
        std::cout << SYMBOL_CHECK << " Will test Fast+Hugepages mode only" << std::flush;
    } else if (input == "FH") {
        testFast = true;
        testHugepages = true;
        std::cout << "\e[" << inputRow << ";1H\e[K";
        std::cout << SYMBOL_CHECK << " Will test Fast and Fast+Hugepages modes" << std::flush;
    } else {
        // Invalid input, default to all
        testLight = true;
        testFast = true;
        testHugepages = true;
        std::cout << "\e[" << inputRow << ";1H\e[K";
        std::cout << SYMBOL_CHECK << " Invalid choice, will test all modes" << std::flush;
    }

    MilliSleep(1000);  // Brief pause to show the confirmation
}

// Prompt user whether to auto-apply optimal threads after benchmark
static bool promptBenchmarkAutoApply(int screenHeight)
{
#ifndef WIN32
    enableCanonicalMode();
#endif

    // Use dedicated input area at bottom of screen (3 rows from bottom)
    int inputRow = screenHeight - 2;

    // Clear input area and show prompt
    std::cout << "\e[" << inputRow << ";1H\e[K";
    std::cout << "Apply optimal thread count to junocashd.conf and start mining after benchmark? (Y/N): " << std::flush;

    std::string input;
    std::getline(std::cin, input);

    bool autoApply = false;
    if (!input.empty()) {
        char choice = std::toupper(input[0]);
        if (choice == 'Y') {
            autoApply = true;
            std::cout << "\e[" << inputRow << ";1H\e[K";
            std::cout << SYMBOL_CHECK << " Will auto-apply optimal thread count after benchmark" << std::flush;
        } else {
            std::cout << "\e[" << inputRow << ";1H\e[K";
            std::cout << SYMBOL_CHECK << " Benchmark results will be saved to file only" << std::flush;
        }
    } else {
        std::cout << "\e[" << inputRow << ";1H\e[K";
        std::cout << SYMBOL_CHECK << " Benchmark results will be saved to file only" << std::flush;
    }

    // Brief pause to show confirmation
    MilliSleep(800);
    std::cout << "\e[" << inputRow << ";1H\e[K" << std::flush;

#ifndef WIN32
    enableRawMode();
#endif

    return autoApply;
}

// Toggle benchmark mode on/off
static void toggleBenchmark(int screenHeight)
{
    if (benchmarkMode.load()) {
        // Stop benchmark
        LogPrintf("Stopping benchmark mode\n");
        benchmarkMode = false;

        if (benchmarkThread != nullptr) {
            benchmarkThread->interrupt();
            benchmarkThread->join();
            delete benchmarkThread;
            benchmarkThread = nullptr;
        }

        // Restore original mining state (unless auto-apply is enabled)
        if (!benchmarkAutoApply.load()) {
            int originalThreads = benchmarkOriginalThreads.load();
            if (originalThreads > 0) {
                LogPrintf("Restoring mining with %d threads\n", originalThreads);
                mapArgs["-genproclimit"] = std::to_string(originalThreads);
                GenerateBitcoins(true, originalThreads, Params());
            }
        } else {
            LogPrintf("Auto-apply was enabled, keeping benchmark-determined thread count\n");
        }

        // Trigger screen refresh after stopping benchmark
        TriggerRefresh();
    } else {
        // Start benchmark
        bool mining = GetBoolArg("-gen", false);
        if (!mining) {
            LogPrintf("Benchmark requires mining to be enabled. Enable mining first with [M]\n");
            return;
        }

        // Ask user which modes to test
        bool testLight, testFast, testHugepages;
        promptBenchmarkModes(screenHeight, testLight, testFast, testHugepages);
        benchmarkTestLight = testLight;
        benchmarkTestFast = testFast;
        benchmarkTestHugepages = testHugepages;

        // Ask user if they want to auto-apply results
        bool autoApply = promptBenchmarkAutoApply(screenHeight);
        benchmarkAutoApply = autoApply;

        LogPrintf("Starting benchmark mode (auto-apply: %s)\n", autoApply ? "yes" : "no");

        // Save current thread count to restore later
        int currentThreads = GetArg("-genproclimit", boost::thread::hardware_concurrency());
        if (currentThreads == -1 || currentThreads == 0) {
            currentThreads = boost::thread::hardware_concurrency();
        }
        benchmarkOriginalThreads = currentThreads;

        // Stop current mining before benchmark starts
        LogPrintf("Stopping current mining to begin benchmark\n");
        GenerateBitcoins(false, 0, Params());

        // Determine max threads - always test all available threads
        // Do NOT use currentThreads here, as that's just what was configured
        int maxThreads = boost::thread::hardware_concurrency();
        LogPrintf("Benchmark will test 1 to %d threads (all available)\n", maxThreads);
        benchmarkMaxThreads = maxThreads;
        benchmarkMode = true;
        benchmarkResults.clear();

        // Start benchmark thread
        benchmarkThread = new boost::thread(&ThreadBenchmarkMining);
    }
}

// Prompt user for number of mining threads (with fixed screen positioning)
static void promptForThreads(int screenHeight)
{
#ifndef WIN32
    enableCanonicalMode();
#endif

    // Use dedicated input area at bottom of screen (3 rows from bottom)
    int inputRow = screenHeight - 2;

    // Clear input area
    std::cout << "\e[" << inputRow << ";1H\e[K";
    std::cout << "Enter number of mining threads (1-" << boost::thread::hardware_concurrency() << ", or -1 for all cores): " << std::flush;

    std::string input;
    std::getline(std::cin, input);

    try {
        int threads = std::stoi(input);
        int maxThreads = boost::thread::hardware_concurrency();

        if (threads == -1) {
            threads = maxThreads;
        }

        if (threads >= 1 && threads <= maxThreads) {
            mapArgs["-genproclimit"] = itostr(threads);

            // Restart mining with new thread count if currently mining
            bool currentlyMining = GetBoolArg("-gen", false);
            if (currentlyMining) {
                // Stop mining first
                GenerateBitcoins(false, 0, Params());
                MilliSleep(500);  // Wait for threads to stop

                // Reset counters and timer for accurate hashrate with new thread count
                ehSolverRuns.value.store(0);
                solutionTargetChecks.value.store(0);
                miningTimer.zeroize();

                // Restart mining with new thread count
                GenerateBitcoins(true, threads, Params());
                miningStartTime = GetTime();  // Track start time for warmup display
                LogPrintf("User set mining threads to %d (mining restarted, counters reset)\n", threads);
            } else {
                LogPrintf("User set mining threads to %d (will apply when mining starts)\n", threads);
            }
            std::cout << "\e[" << inputRow << ";1H\e[K";  // Clear and reposition
            std::cout << SYMBOL_CHECK << " Mining threads set to " << threads << std::flush;
        } else {
            std::cout << "\e[" << inputRow << ";1H\e[K";
            std::cout << SYMBOL_CROSS << " Invalid thread count (must be 1-" << maxThreads << ")" << std::flush;
        }
    } catch (...) {
        std::cout << "\e[" << inputRow << ";1H\e[K";
        std::cout << SYMBOL_CROSS << " Invalid input (not a number)" << std::flush;
    }

    // Brief pause to show confirmation, then clear
    MilliSleep(800);
    std::cout << "\e[" << inputRow << ";1H\e[K" << std::flush;

#ifndef WIN32
    enableRawMode();
#endif
}

// Get system information for benchmarking
static std::string GetCPUInfo()
{
#ifndef WIN32
    std::ifstream cpuinfo("/proc/cpuinfo");
    std::string line;
    std::string model_name = "Unknown";
    std::string vendor_id = "Unknown";

    while (std::getline(cpuinfo, line)) {
        if (line.find("model name") != std::string::npos) {
            size_t pos = line.find(":");
            if (pos != std::string::npos) {
                model_name = line.substr(pos + 2);
            }
        } else if (line.find("vendor_id") != std::string::npos) {
            size_t pos = line.find(":");
            if (pos != std::string::npos) {
                vendor_id = line.substr(pos + 2);
            }
        }
        if (model_name != "Unknown" && vendor_id != "Unknown") {
            break;
        }
    }
    return vendor_id + " - " + model_name;
#else
    return "Windows CPU";
#endif
}

static std::string GetCPUVendor()
{
#ifndef WIN32
    std::ifstream cpuinfo("/proc/cpuinfo");
    std::string line;

    while (std::getline(cpuinfo, line)) {
        if (line.find("vendor_id") != std::string::npos) {
            size_t pos = line.find(":");
            if (pos != std::string::npos) {
                std::string vendor = line.substr(pos + 2);
                // Clean up vendor string for filename
                vendor.erase(std::remove_if(vendor.begin(), vendor.end(), ::isspace), vendor.end());
                return vendor;
            }
        }
    }
#endif
    return "Unknown";
}

static std::string GetCPUModel()
{
#ifndef WIN32
    std::ifstream cpuinfo("/proc/cpuinfo");
    std::string line;

    while (std::getline(cpuinfo, line)) {
        if (line.find("model name") != std::string::npos) {
            size_t pos = line.find(":");
            if (pos != std::string::npos) {
                std::string model = line.substr(pos + 2);
                // Clean up and extract key parts for filename
                // Remove extra spaces and special characters
                model.erase(std::remove_if(model.begin(), model.end(),
                    [](char c) { return !std::isalnum(c) && c != '-'; }), model.end());
                // Limit length
                if (model.length() > 30) {
                    model = model.substr(0, 30);
                }
                return model;
            }
        }
    }
#endif
    return "Unknown";
}

static std::string GetMemoryInfo()
{
#ifndef WIN32
    std::ifstream meminfo("/proc/meminfo");
    std::string line;
    std::stringstream info;

    long totalMem = 0;
    long freeMem = 0;
    long availableMem = 0;
    int hugepages_2m_total = 0;
    int hugepages_2m_free = 0;
    int hugepages_1g_total = 0;
    int hugepages_1g_free = 0;

    // Read system memory
    while (std::getline(meminfo, line)) {
        if (line.find("MemTotal:") != std::string::npos) {
            sscanf(line.c_str(), "MemTotal: %ld kB", &totalMem);
        } else if (line.find("MemFree:") != std::string::npos) {
            sscanf(line.c_str(), "MemFree: %ld kB", &freeMem);
        } else if (line.find("MemAvailable:") != std::string::npos) {
            sscanf(line.c_str(), "MemAvailable: %ld kB", &availableMem);
        }
    }

    info << "Total: " << (totalMem / 1024) << " MB, ";
    info << "Available: " << (availableMem / 1024) << " MB";

    // Check hugepages
    std::ifstream hp_2m_total("/sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages");
    if (hp_2m_total.is_open()) {
        hp_2m_total >> hugepages_2m_total;
    }

    std::ifstream hp_2m_free("/sys/kernel/mm/hugepages/hugepages-2048kB/free_hugepages");
    if (hp_2m_free.is_open()) {
        hp_2m_free >> hugepages_2m_free;
    }

    std::ifstream hp_1g_total("/sys/kernel/mm/hugepages/hugepages-1048576kB/nr_hugepages");
    if (hp_1g_total.is_open()) {
        hp_1g_total >> hugepages_1g_total;
    }

    std::ifstream hp_1g_free("/sys/kernel/mm/hugepages/hugepages-1048576kB/free_hugepages");
    if (hp_1g_free.is_open()) {
        hp_1g_free >> hugepages_1g_free;
    }

    if (hugepages_1g_total > 0) {
        info << "\n  1GB Hugepages: " << hugepages_1g_total << " total, " << hugepages_1g_free << " free (" << (hugepages_1g_total * 1024) << " MB)";
    }

    if (hugepages_2m_total > 0) {
        info << "\n  2MB Hugepages: " << hugepages_2m_total << " total, " << hugepages_2m_free << " free (" << (hugepages_2m_total * 2) << " MB)";
    }

    return info.str();
#else
    return "Windows Memory";
#endif
}

static int GetCPUCores()
{
#ifndef WIN32
    std::ifstream cpuinfo("/proc/cpuinfo");
    std::string line;
    int cores = 0;

    while (std::getline(cpuinfo, line)) {
        if (line.find("processor") != std::string::npos && line.find(":") != std::string::npos) {
            cores++;
        }
    }

    return cores > 0 ? cores : 1;
#else
    return 1;
#endif
}

static std::string GetMotherboardName()
{
#ifndef WIN32
    // Try to read motherboard name from DMI
    std::string boardName = "Unknown";
    std::string boardVendor = "Unknown";

    // Try board name
    std::ifstream boardNameFile("/sys/devices/virtual/dmi/id/board_name");
    if (boardNameFile.is_open()) {
        std::getline(boardNameFile, boardName);
        // Remove trailing whitespace
        boardName.erase(boardName.find_last_not_of(" \n\r\t") + 1);
    }

    // Try board vendor
    std::ifstream boardVendorFile("/sys/devices/virtual/dmi/id/board_vendor");
    if (boardVendorFile.is_open()) {
        std::getline(boardVendorFile, boardVendor);
        // Remove trailing whitespace
        boardVendor.erase(boardVendor.find_last_not_of(" \n\r\t") + 1);
    }

    // Combine vendor and name if both available
    if (boardVendor != "Unknown" && boardName != "Unknown") {
        return boardVendor + " " + boardName;
    } else if (boardName != "Unknown") {
        return boardName;
    } else if (boardVendor != "Unknown") {
        return boardVendor;
    }

    return "Unknown";
#else
    return "Windows";
#endif
}

static std::string GetMotherboardNameForFilename()
{
#ifndef WIN32
    std::string mbName = GetMotherboardName();

    // Clean up for filename - remove special characters, keep only alphanumeric and dash
    mbName.erase(std::remove_if(mbName.begin(), mbName.end(),
        [](char c) { return !std::isalnum(c) && c != '-'; }), mbName.end());

    // Limit length
    if (mbName.length() > 30) {
        mbName = mbName.substr(0, 30);
    }

    return mbName;
#else
    return "Windows";
#endif
}

// Benchmark thread - automatically tests different thread counts
void ThreadBenchmarkMining()
{
    LogPrintf("Starting mining benchmark mode\n");

    // Get mode selection from global atomics (set by toggleBenchmark)
    bool testLight = benchmarkTestLight.load();
    bool testFast = benchmarkTestFast.load();
    bool testHugepages = benchmarkTestHugepages.load();

    // Get system information
    std::string cpuVendor = GetCPUVendor();
    std::string cpuModel = GetCPUModel();
    std::string cpuInfo = GetCPUInfo();
    std::string memInfo = GetMemoryInfo();
    std::string motherboardName = GetMotherboardName();
    std::string motherboardNameClean = GetMotherboardNameForFilename();
    int cpuCores = GetCPUCores();

    // Create filename with motherboard, CPU info and date
    std::string dateStr = DateTimeStrFormat("%Y%m%d-%H%M%S", GetTime());
    std::string filename = strprintf("benchmark-%s-%s-%s-%s.txt", motherboardNameClean, cpuVendor, cpuModel, dateStr);

    // Open benchmark log file
    boost::filesystem::path benchmarkLogPath = GetDataDir() / filename;
    std::ofstream benchmarkLog(benchmarkLogPath.string(), std::ios::out);

    if (!benchmarkLog.is_open()) {
        LogPrintf("ERROR: Could not open %s for writing\n", filename);
        return;
    }

    LogPrintf("Benchmark results will be saved to: %s\n", benchmarkLogPath.string());

    // Capture starting block height for benchmark duration tracking
    {
        LOCK(cs_main);
        if (chainActive.Tip()) {
            benchmarkStartHeight = chainActive.Tip()->nHeight;
        } else {
            benchmarkStartHeight = 0;
        }
    }

    // Get mining mode information
    bool fastMode = GetBoolArg("-randomxfastmode", false);
    bool hugepagesEnabled = IsHugepagesEnabled();
    std::string randomxMode = fastMode ? "Fast (2GB dataset)" : "Light (256MB dataset)";
    std::string hugepagesStatus = hugepagesEnabled ? "ENABLED" : "DISABLED";

    // Print configuration to console
    LogPrintf("===========================================\n");
    LogPrintf("Benchmark Configuration:\n");
    LogPrintf("  Motherboard: %s\n", motherboardName);
    LogPrintf("  CPU: %s\n", cpuInfo);
    LogPrintf("  CPU Cores: %d\n", cpuCores);
    LogPrintf("  Thread Range: 1 to %d\n", benchmarkMaxThreads.load());
    LogPrintf("  Duration per test: %d seconds (+ 2s warmup)\n", BENCHMARK_DURATION_SECONDS);
    LogPrintf("  RandomX Mode: %s\n", randomxMode);
    LogPrintf("  Hugepages: %s\n", hugepagesStatus);
    if (!hugepagesEnabled) {
        LogPrintf("  WARNING: Hugepages disabled - performance may be reduced by 5-10%%\n");
    }
    if (!fastMode) {
        LogPrintf("  INFO: Using Light mode - consider -randomxfastmode for ~30%% better performance\n");
    }
    LogPrintf("===========================================\n");

    // Reset hash counters for accurate benchmarking
    LogPrintf("Resetting hash counters for accurate measurements\n");
    ehSolverRuns.value.store(0);
    solutionTargetChecks.value.store(0);
    miningTimer.zeroize();

    // Write header with system information
    benchmarkLog << "========================================\n";
    benchmarkLog << "JunoCash Mining Benchmark\n";
    benchmarkLog << "========================================\n";
    benchmarkLog << "Date: " << DateTimeStrFormat("%Y-%m-%d %H:%M:%S", GetTime()) << "\n";
    benchmarkLog << "Version: " << FormatFullVersion() << "\n";
    benchmarkLog << "Network: " << Params().NetworkIDString() << "\n";
    benchmarkLog << "\n";
    benchmarkLog << "========================================\n";
    benchmarkLog << "System Configuration\n";
    benchmarkLog << "========================================\n";
    benchmarkLog << "Motherboard: " << motherboardName << "\n";
    benchmarkLog << "CPU: " << cpuInfo << "\n";
    benchmarkLog << "CPU Cores: " << cpuCores << "\n";
    benchmarkLog << "Memory: " << memInfo << "\n";
    benchmarkLog << "\n";
    benchmarkLog << "========================================\n";
    benchmarkLog << "Benchmark Parameters\n";
    benchmarkLog << "========================================\n";
    benchmarkLog << "Thread Count Range: 1 to " << benchmarkMaxThreads.load() << "\n";
    benchmarkLog << "Duration per test: " << BENCHMARK_DURATION_SECONDS << " seconds (+ 2s warmup per test)\n";
    benchmarkLog << "RandomX Mode: " << randomxMode << "\n";
    benchmarkLog << "Hugepages: " << hugepagesStatus << "\n";
    if (!hugepagesEnabled) {
        benchmarkLog << "  Note: Hugepages disabled - hashrate may be 5-10% lower than with hugepages\n";
    }
    if (!fastMode) {
        benchmarkLog << "  Note: Light mode - Fast mode (-randomxfastmode) provides ~30% better performance\n";
    }
    benchmarkLog << "\n";
    benchmarkLog << "========================================\n";
    benchmarkLog << "Results\n";
    benchmarkLog << "========================================\n";
    benchmarkLog << std::flush;

    // Define the 3 modes to test
    struct TestMode {
        std::string name;
        bool fastMode;
        bool hugepages;
    };

    std::vector<TestMode> modes;
    if (testLight) {
        modes.push_back({"Light", false, false});
    }
    if (testFast) {
        modes.push_back({"Fast", true, false});
    }
    if (testHugepages) {
        modes.push_back({"Fast+Hugepages", true, true});
    }

    try {
        // Test each mode
        for (const auto& mode : modes) {
            LogPrintf("\n========================================\n");
            LogPrintf("Testing Mode: %s\n", mode.name);
            LogPrintf("========================================\n");

            benchmarkLog << "\n========================================\n";
            benchmarkLog << "Mode: " << mode.name << "\n";
            benchmarkLog << "========================================\n";
            benchmarkLog << std::flush;

            // Change RandomX mode without shutting down
            // Mining threads will automatically recreate their VMs with new settings
            LogPrintf("Switching to %s mode...\n", mode.name);
            RandomX_ChangeMode(mode.fastMode, mode.hugepages);

            // Test each thread count in this mode
            for (int threadCount = 1; threadCount <= benchmarkMaxThreads.load(); threadCount++) {
                benchmarkCurrentThreads = threadCount;

                LogPrintf("Benchmark: Testing %s mode with %d thread(s)...\n", mode.name, threadCount);
                benchmarkLog << "\n  Thread Count: " << threadCount << "\n";
                benchmarkLog << std::flush;

                // Stop mining before changing thread count
                GenerateBitcoins(false, 0, Params());
                MilliSleep(500);  // Wait for threads to stop

                // Reset hash counters and timer for this thread count test
                ehSolverRuns.value.store(0);
                solutionTargetChecks.value.store(0);
                miningTimer.zeroize();

                // Start mining with new thread count
                GenerateBitcoins(true, threadCount, Params());

                // Reset benchmark stats for this test
                benchmarkAccumulatedHashrate = 0.0;
                benchmarkSampleCount = 0;
                benchmarkStartTime = GetTime();

                // Wait for mining to produce first hashes (RandomX can be slow to start)
                benchmarkWarmingUp = true;
                int64_t warmupTimeout = GetTime() + 10;  // Reduced from 30s to 10s
                while (GetTime() < warmupTimeout) {
                    MilliSleep(1000);
                    double warmupHashrate = GetLocalSolPS();

                    if (warmupHashrate > 0) {
                        LogPrintf("Mining active, hashrate detected: %.2f H/s\n", warmupHashrate);
                        break;
                    }
                    boost::this_thread::interruption_point();
                }
                benchmarkWarmingUp = false;

                if (GetLocalSolPS() == 0) {
                    LogPrintf("WARNING: No hashrate detected after warmup period\n");
                }

                // Mine for the specified duration and collect samples
                int64_t endTime = GetTime() + BENCHMARK_DURATION_SECONDS;
                while (GetTime() < endTime) {
                    boost::this_thread::interruption_point();

                    // Sample hashrate every second
                    MilliSleep(1000);

                    double currentHashrate = GetLocalSolPS();
                    if (currentHashrate > 0) {
                        benchmarkAccumulatedHashrate = benchmarkAccumulatedHashrate.load() + currentHashrate;
                        benchmarkSampleCount++;
                    }
                }

                // Calculate average
                double avgHashrate = 0.0;
                if (benchmarkSampleCount > 0) {
                    avgHashrate = benchmarkAccumulatedHashrate.load() / benchmarkSampleCount.load();
                }

                // Log results
                LogPrintf("Benchmark: %s mode, %d thread(s) = %.2f H/s (avg over %d samples)\n",
                         mode.name, threadCount, avgHashrate, benchmarkSampleCount.load());

                benchmarkLog << "    Average Hashrate: " << std::fixed << std::setprecision(2) << avgHashrate << " H/s\n";
                benchmarkLog << "    Samples: " << benchmarkSampleCount.load() << "\n";
                benchmarkLog << std::flush;

                // Store result
                BenchmarkResult result;
                result.threads = threadCount;
                result.mode = mode.name;
                result.hashrate = avgHashrate;
                result.samples = benchmarkSampleCount.load();
                benchmarkResults.push_back(result);
            }
        }

        // Find best configuration overall
        int bestThreads = 1;
        std::string bestMode = "Light";
        double bestHashrate = 0.0;
        for (const auto& result : benchmarkResults) {
            if (result.hashrate > bestHashrate) {
                bestHashrate = result.hashrate;
                bestThreads = result.threads;
                bestMode = result.mode;
            }
        }

        // Write summary
        benchmarkLog << "\n========================================\n";
        benchmarkLog << "Summary\n";
        benchmarkLog << "========================================\n";
        benchmarkLog << "Best Configuration: " << bestMode << " mode, " << bestThreads << " threads @ " << std::fixed << std::setprecision(2) << bestHashrate << " H/s\n";

        // Group results by mode
        benchmarkLog << "\nHashrate by mode and thread count:\n";
        for (const auto& testMode : modes) {
            benchmarkLog << "\n  " << testMode.name << " Mode:\n";
            for (const auto& result : benchmarkResults) {
                if (result.mode == testMode.name) {
                    benchmarkLog << "    " << result.threads << " threads: " << std::fixed << std::setprecision(2) << result.hashrate << " H/s\n";
                }
            }
        }

        // Write footer
        benchmarkLog << "\n========================================\n";
        benchmarkLog << "Benchmark Complete\n";
        benchmarkLog << "Finished: " << DateTimeStrFormat("%Y-%m-%d %H:%M:%S", GetTime()) << "\n";
        benchmarkLog << "========================================\n";
        benchmarkLog << std::flush;

        // Get ending block height
        int endHeight = 0;
        {
            LOCK(cs_main);
            if (chainActive.Tip()) {
                endHeight = chainActive.Tip()->nHeight;
            }
        }

        LogPrintf("Benchmark complete! Best: %s mode, %d threads @ %.2f H/s\n", bestMode, bestThreads, bestHashrate);
        LogPrintf("Benchmark ran from block height %d to %d\n", benchmarkStartHeight, endHeight);
        LogPrintf("Results saved to %s\n", benchmarkLogPath.string());

        benchmarkLog << "Starting block height: " << benchmarkStartHeight << "\n";
        benchmarkLog << "Ending block height: " << endHeight << "\n";

        // Apply optimal configuration if user requested
        if (benchmarkAutoApply.load() && bestThreads > 0) {
            LogPrintf("Auto-applying optimal configuration: %s mode, %d threads\n", bestMode, bestThreads);

            // Write to config file
            if (writeOptimalThreadsToConfig(bestThreads, bestMode)) {
                // Update runtime settings
                mapArgs["-genproclimit"] = std::to_string(bestThreads);

                // Set RandomX mode flags
                if (bestMode == "Fast" || bestMode == "Fast+Hugepages") {
                    mapArgs["-randomxfastmode"] = "1";
                } else {
                    mapArgs["-randomxfastmode"] = "0";
                }

                if (bestMode == "Fast+Hugepages") {
                    mapArgs["-randomxhugepages"] = "1";
                } else {
                    mapArgs["-randomxhugepages"] = "0";
                }

                // Reinitialize RandomX with best mode
                GenerateBitcoins(false, 0, Params());  // Stop mining first
                RandomX_Shutdown();
                RandomX_Init(bestMode == "Fast" || bestMode == "Fast+Hugepages",
                            bestMode == "Fast+Hugepages");

                // Exit benchmark mode BEFORE starting normal mining
                benchmarkMode = false;

                // Start mining with optimal configuration
                LogPrintf("Starting mining with optimal configuration\n");
                GenerateBitcoins(true, bestThreads, Params());
                miningStartTime = GetTime();  // Track start time for warmup display

                benchmarkLog << "\nAuto-applied optimal settings:\n";
                benchmarkLog << "  - Updated junocashd.conf with best mode: " << bestMode << "\n";
                benchmarkLog << "  - Updated junocashd.conf with genproclimit=" << bestThreads << "\n";
                benchmarkLog << "  - Started mining with " << bestMode << " mode, " << bestThreads << " threads\n";
                benchmarkLog << std::flush;
            } else {
                LogPrintf("WARNING: Failed to write to config file, but starting mining anyway\n");

                // Update runtime settings
                mapArgs["-genproclimit"] = std::to_string(bestThreads);
                if (bestMode == "Fast" || bestMode == "Fast+Hugepages") {
                    mapArgs["-randomxfastmode"] = "1";
                } else {
                    mapArgs["-randomxfastmode"] = "0";
                }
                if (bestMode == "Fast+Hugepages") {
                    mapArgs["-randomxhugepages"] = "1";
                } else {
                    mapArgs["-randomxhugepages"] = "0";
                }

                // Reinitialize and start mining
                GenerateBitcoins(false, 0, Params());
                RandomX_Shutdown();
                RandomX_Init(bestMode == "Fast" || bestMode == "Fast+Hugepages",
                            bestMode == "Fast+Hugepages");

                // Exit benchmark mode BEFORE starting normal mining
                benchmarkMode = false;

                GenerateBitcoins(true, bestThreads, Params());
                miningStartTime = GetTime();  // Track start time for warmup display
            }
        }

    } catch (boost::thread_interrupted&) {
        // User stopped benchmark early - still write summary
        benchmarkLog << "\n========================================\n";
        benchmarkLog << "Benchmark Interrupted by User\n";
        benchmarkLog << "========================================\n";

        if (!benchmarkResults.empty()) {
            // Find best from partial results
            int bestThreads = 1;
            std::string bestMode = "Light";
            double bestHashrate = 0.0;
            for (const auto& result : benchmarkResults) {
                if (result.hashrate > bestHashrate) {
                    bestHashrate = result.hashrate;
                    bestThreads = result.threads;
                    bestMode = result.mode;
                }
            }

            benchmarkLog << "Partial Results (tested " << benchmarkResults.size() << " configurations):\n";
            benchmarkLog << "Best so far: " << bestMode << " mode, " << bestThreads << " threads @ " << std::fixed << std::setprecision(2) << bestHashrate << " H/s\n\n";

            // Group partial results by mode
            benchmarkLog << "Results by mode:\n";
            std::set<std::string> testedModes;
            for (const auto& result : benchmarkResults) {
                testedModes.insert(result.mode);
            }
            for (const auto& modeName : testedModes) {
                benchmarkLog << "  " << modeName << " Mode:\n";
                for (const auto& result : benchmarkResults) {
                    if (result.mode == modeName) {
                        benchmarkLog << "    " << result.threads << " threads: " << std::fixed << std::setprecision(2) << result.hashrate << " H/s\n";
                    }
                }
            }

            // Get ending block height
            int endHeight = 0;
            {
                LOCK(cs_main);
                if (chainActive.Tip()) {
                    endHeight = chainActive.Tip()->nHeight;
                }
            }

            LogPrintf("Benchmark interrupted. Partial results: Best %s mode, %d threads @ %.2f H/s\n", bestMode, bestThreads, bestHashrate);
            LogPrintf("Benchmark ran from block height %d to %d (interrupted)\n", benchmarkStartHeight, endHeight);

            benchmarkLog << "Starting block height: " << benchmarkStartHeight << "\n";
            benchmarkLog << "Ending block height: " << endHeight << " (interrupted)\n";

            // Apply optimal configuration if user requested (even with partial results)
            if (benchmarkAutoApply.load() && bestThreads > 0) {
                LogPrintf("Auto-applying best partial result: %s mode, %d threads\n", bestMode, bestThreads);

                // Write to config file
                if (writeOptimalThreadsToConfig(bestThreads, bestMode)) {
                    // Update runtime settings
                    mapArgs["-genproclimit"] = std::to_string(bestThreads);
                    if (bestMode == "Fast" || bestMode == "Fast+Hugepages") {
                        mapArgs["-randomxfastmode"] = "1";
                    } else {
                        mapArgs["-randomxfastmode"] = "0";
                    }
                    if (bestMode == "Fast+Hugepages") {
                        mapArgs["-randomxhugepages"] = "1";
                    } else {
                        mapArgs["-randomxhugepages"] = "0";
                    }

                    // Reinitialize RandomX and start mining
                    GenerateBitcoins(false, 0, Params());
                    RandomX_Shutdown();
                    RandomX_Init(bestMode == "Fast" || bestMode == "Fast+Hugepages",
                                bestMode == "Fast+Hugepages");

                    // Exit benchmark mode BEFORE starting normal mining
                    benchmarkMode = false;

                    LogPrintf("Starting mining with %s mode, %d threads (from partial results)\n", bestMode, bestThreads);
                    GenerateBitcoins(true, bestThreads, Params());
                    miningStartTime = GetTime();  // Track start time for warmup display

                    benchmarkLog << "\nAuto-applied partial results:\n";
                    benchmarkLog << "  - Updated junocashd.conf with best mode: " << bestMode << "\n";
                    benchmarkLog << "  - Updated junocashd.conf with genproclimit=" << bestThreads << "\n";
                    benchmarkLog << "  - Started mining with " << bestMode << " mode, " << bestThreads << " threads\n";
                    benchmarkLog << std::flush;
                } else {
                    LogPrintf("WARNING: Failed to write to config file, but starting mining anyway\n");
                    mapArgs["-genproclimit"] = std::to_string(bestThreads);
                    if (bestMode == "Fast" || bestMode == "Fast+Hugepages") {
                        mapArgs["-randomxfastmode"] = "1";
                    } else {
                        mapArgs["-randomxfastmode"] = "0";
                    }
                    if (bestMode == "Fast+Hugepages") {
                        mapArgs["-randomxhugepages"] = "1";
                    } else {
                        mapArgs["-randomxhugepages"] = "0";
                    }
                    GenerateBitcoins(false, 0, Params());
                    RandomX_Shutdown();
                    RandomX_Init(bestMode == "Fast" || bestMode == "Fast+Hugepages",
                                bestMode == "Fast+Hugepages");

                    // Exit benchmark mode BEFORE starting normal mining
                    benchmarkMode = false;

                    GenerateBitcoins(true, bestThreads, Params());
                    miningStartTime = GetTime();  // Track start time for warmup display
                }
            }
        } else {
            benchmarkLog << "No results collected yet\n";
            LogPrintf("Benchmark interrupted with no results\n");
        }

        benchmarkLog << "\nFinished: " << DateTimeStrFormat("%Y-%m-%d %H:%M:%S", GetTime()) << "\n";
        benchmarkLog << "========================================\n";
        benchmarkLog << std::flush;
    }

    // Stop mining before exiting benchmark (unless auto-apply started mining)
    if (!benchmarkAutoApply.load()) {
        LogPrintf("Benchmark ending, stopping mining\n");
        GenerateBitcoins(false, 0, Params());
    } else {
        LogPrintf("Benchmark ending, mining continues with optimal threads\n");

        // Reset counters and timer for accurate hashrate display after benchmark
        LogPrintf("Resetting hashrate counters for post-benchmark mining\n");
        MilliSleep(500);  // Wait for mining to stabilize
        ehSolverRuns.value.store(0);
        solutionTargetChecks.value.store(0);
        miningTimer.zeroize();
    }

    benchmarkLog.close();
    benchmarkMode = false;

    // Trigger screen refresh to clear benchmark status
    TriggerRefresh();
}

void ThreadShowMetricsScreen()
{
    // Determine whether we should render a persistent UI or rolling metrics
    bool isTTY = isatty(STDOUT_FILENO);
    bool isScreen = GetBoolArg("-metricsui", isTTY);
    int64_t nRefresh = GetArg("-metricsrefreshtime", isTTY ? 1 : 600);

    // Header is 6 lines: box top + 3 centered lines + box bottom + blank line
    const int HEADER_LINES = 7;  // Position to start content (row 7, line after header)

    if (isScreen) {
#ifdef WIN32
        enableVTMode();
#else
        // Enable raw mode for non-blocking keyboard input
        if (isTTY) {
            enableRawMode();
        }
#endif

        // Initial screen setup: clear and draw header once
        std::cout << "\e[2J\e[H" << std::flush;  // Clear screen and move to home
        drawBoxTop("");
        drawCentered("Juno Cash", "\e[1;33m");
        drawCentered("Privacy Money for All", "\e[1;36m");
        drawCentered(FormatFullVersion() + " - " + WhichNetwork() + " - RandomX", "\e[0;37m");
        drawBoxBottom();
        std::cout << std::endl;
    }

    while (true) {
        // Number of lines that are always displayed
        int lines = 0;
        int cols = 80;
        int rows = 24;  // Default terminal height

        // Get current window size
        if (isTTY) {
#ifdef WIN32
            CONSOLE_SCREEN_BUFFER_INFO csbi;
            if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi) != 0) {
                cols = csbi.srWindow.Right - csbi.srWindow.Left + 1;
                rows = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
            }
#else
            struct winsize w;
            w.ws_col = 0;
            w.ws_row = 0;
            if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) != -1) {
                if (w.ws_col != 0) cols = w.ws_col;
                if (w.ws_row != 0) rows = w.ws_row;
            }
#endif
        }

        // Lock and fetch stats before erasing the screen, in case we block.
        std::optional<MetricsStats> metricsStats;
        if (loaded) {
            metricsStats = loadStats();
        }

        if (isScreen) {
            // Move to position after header (row 7) and clear rest of screen
            std::cout << "\e[" << HEADER_LINES << ";1H\e[J" << std::flush;
        }

        // Miner status
#ifdef ENABLE_MINING
        bool mining = GetBoolArg("-gen", false);
#else
        bool mining = false;
#endif

        if (loaded) {
            lines += printStats(metricsStats.value(), isScreen, mining);
            lines += printWalletStatus();
            lines += printMiningStatus(mining);
        }
        lines += printMetrics(cols, mining);
        lines += printMessageBox(cols);
        lines += printInitMessage();

        if (isScreen) {
            // Explain how to exit (no newline - avoid scrolling)
            std::cout << "[";
#ifdef WIN32
            std::cout << _("'junocash-cli.exe stop' to exit");
#else
            std::cout << _("Press Ctrl+C to exit");
#endif
            std::cout << "] [" << _("Set 'showmetrics=0' to hide") << "]" << std::flush;
            lines++; // Count the exit message line
        } else {
            // Print delineator
            std::cout << "----------------------------------------" << std::endl;
        }

        *nNextRefresh = GetTime() + nRefresh;
        while (GetTime() < *nNextRefresh) {
            boost::this_thread::interruption_point();

            // Check for keyboard input
            if (isScreen && isTTY) {
                int key = checkKeyPress();
                if (key == 'Q' || key == 'q') {
                    // Quit the daemon gracefully
                    std::cout << std::endl << "Shutting down, please wait..." << std::endl << std::endl;
                    StartShutdown();
                    return;
                } else if (key == 'M' || key == 'm') {
                    toggleMining();
                    break;  // Force screen refresh
                } else if (key == 'T' || key == 't') {
                    // Only allow changing threads if mining or on non-main network
                    if (mining || Params().NetworkIDString() != "main") {
                        promptForThreads(rows);
                        break;  // Force screen refresh
                    }
                } else if (key == 'B' || key == 'b') {
                    // Toggle benchmark mode
                    if (mining) {
                        toggleBenchmark(rows);
                        break;  // Force screen refresh
                    }
                } else if (mining) {
                    // Donation controls only available when mining
                    if (key == 'D' || key == 'd') {
                        toggleDonation();
                        break;  // Force screen refresh
                    } else if (key == 'P' || key == 'p') {
                        // Only allow changing percentage if donations are active
                        int currentPct = getCurrentDonationPercentage();
                        if (currentPct > 0) {
                            promptForPercentage(rows);
                            break;  // Force screen refresh
                        }
                    }
                }
            }

            MilliSleep(200);
        }

        // Screen will be redrawn from home position at start of next loop
        // No need to reposition cursor
    }
}
