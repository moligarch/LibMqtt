#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <atomic>
#include <mutex>
#include <vector>
#include <csignal>
#include <windows.h>
#include "LibMqtt/MqttClient.h"


#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#endif

// Enable ANSI escape codes on Windows consoles (no-op on other platforms)
static void enable_ansi_console() {
#ifdef _WIN32
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut == INVALID_HANDLE_VALUE) return;
    DWORD dwMode = 0;
    if (!GetConsoleMode(hOut, &dwMode)) return;
    // ENABLE_VIRTUAL_TERMINAL_PROCESSING = 0x0004
    dwMode |= 0x0004;
    SetConsoleMode(hOut, dwMode);
#endif
}

// Move cursor up `n` lines (ANSI CSI)
static void move_cursor_up(int n) {
    if (n <= 0) return;
    printf("\x1b[%dA", n);
}


// Typedefs for the DLL functions (from your sample)
typedef void(__cdecl* OnJsonEventCallback)(int eventid, const std::string);
typedef void(__cdecl* SetOnJsonEventCallbackFunc)(OnJsonEventCallback cb);
typedef void(__cdecl* StartFunc)(UINT64 sources);
typedef BOOL(__cdecl* InitializeFunc)();
typedef void(__cdecl* StopFunc)();


static std::atomic<uint64_t> g_messages_sent{ 0 };       // incremented when publish requested
static std::atomic<uint64_t> g_messages_confirmed{ 0 };  // incremented when publish is confirmed

struct Trampoline {
    static MqttClient* clientPtr;
    static std::string publishTopic;
    static QoS publishQoS;
    static bool publishRetained;

    static void __cdecl cb(int /*id*/, const std::string json) {
        if (clientPtr) {
            clientPtr->Publish(publishTopic, json, publishQoS, publishRetained);
            ++g_messages_sent;
        }
    }
};

// Static member definitions
MqttClient* Trampoline::clientPtr = nullptr;
std::string Trampoline::publishTopic = "";
QoS Trampoline::publishQoS = QoS::AtMostOnce;
bool Trampoline::publishRetained = false;



std::atomic<bool> g_running(true);

// Signal handler for CTRL+C (SIGINT)
void handle_sigint(int sig) {
    g_running = false; // Set the flag to false to stop the loop
}

// --- crude fast JSON Sequence extractor ---
// Expects "... "Sequence":12345, ..." and returns 0 on failure.
static uint64_t extract_sequence(const std::string& json) {
    const char* s = json.c_str();
    const char* found = strstr(s, "\"Sequence\"");
    if (!found) found = strstr(s, "\"sequence\"");
    if (!found) return 0;
    // find colon
    const char* colon = strchr(found, ':');
    if (!colon) return 0;
    // skip spaces
    const char* p = colon + 1;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
    // parse number
    uint64_t val = 0;
    bool seen = false;
    while (*p >= '0' && *p <= '9') {
        seen = true;
        val = val * 10 + uint64_t(*p - '0');
        ++p;
    }
    if (!seen) return 0;
    return val;
}

// Wait up to timeout_sec for client to become connected.
// Returns true if connected within timeout, false otherwise.
static bool wait_for_connection(MqttClient& client, int timeout_sec = 5) {
    for (int i = 0; i < timeout_sec * 10; ++i) {
        if (client.IsConnected()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

// ----- Publisher mode: loads DLL and forwards JSON to MQTT -----
int run_publisher(const std::string& broker, const std::string& clientId, const std::string& topic, int qos, bool retained, const std::string& dllPath, bool useMockData) {
    try {
        MqttClient client(broker, clientId);
        client.Connect();
        if (!wait_for_connection(client, 5)) {
            std::cerr << "Publisher: cannot connect to broker: " << broker << std::endl;
            return 1;
        }
        std::cout << "Publisher connected. Topic=" << topic << " qos=" << qos << " retained=" << retained << std::endl;

        // ----- DLL load (replace previous unique_ptr-based code) -----
        HMODULE lib = LoadLibraryA(dllPath.c_str());
        if (!lib) {
            std::cerr << "Failed to load DLL: " << dllPath << " (error=" << GetLastError() << ")\n";
            return 1;
        }

        auto setCallback = (SetOnJsonEventCallbackFunc)GetProcAddress(lib, "SetOnJsonEventCallback");
        auto start = (StartFunc)GetProcAddress(lib, "Start");
        auto initialize = (InitializeFunc)GetProcAddress(lib, "Initialize");
        auto stop = (StopFunc)GetProcAddress(lib, "Stop");
        if (!setCallback || !start || !initialize || !stop) {
            std::cerr << "DLL missing required exports.\n";
            FreeLibrary(lib);
            return 1;
        }

        // configure trampoline with our client and publish params
        Trampoline::clientPtr = &client;
        Trampoline::publishTopic = topic;
        Trampoline::publishQoS = static_cast<QoS>(qos);
        Trampoline::publishRetained = retained;

        // register the trampoline callback with the DLL
        setCallback(&Trampoline::cb);

        // init and start the DLL monitoring
        if (!initialize()) {
            std::cerr << "DLL initialize failed\n";
            FreeLibrary(lib);
            return 1;
        }
        UINT64 sources = 2 | 4 | 8;
        // Start a background reporter thread
        std::atomic<bool> reporter_running{ true };
        std::thread reporter([&]() {
            uint64_t last_total = g_messages_sent.load();
            const int interval = 1; // seconds
            std::cout << "Publishing from DLL. Press Ctrl-C to stop.\n";
            enable_ansi_console();
            // print header area once
            std::cout << "\n--- PUBLISHER LIVE REPORT ---\n\n";
            while (reporter_running && g_running) {
                std::this_thread::sleep_for(std::chrono::seconds(interval));
                uint64_t now_total = g_messages_sent.load();
                uint64_t confirmed = g_messages_confirmed.load();
                uint64_t delta = now_total - last_total;
                last_total = now_total;
                uint64_t rate = delta / interval;
                // move cursor up and overwrite (2 lines)
                move_cursor_up(3);
                printf("--- PUBLISHER LIVE REPORT ---\n");
                printf("Total Sent:      %llu\n", (unsigned long long)now_total);
                printf("Confirmed (ack): %llu  | Rate: %llu msg/sec\n\n", (unsigned long long)confirmed, (unsigned long long)rate);
                fflush(stdout);
            }
            });
        start(sources);

        while (g_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        stop();
        FreeLibrary(lib);
        reporter_running = false;
        if (reporter.joinable()) reporter.join();
        return 0;
    }
    catch (const std::exception& ex) {
        std::cerr << "Publisher exception: " << ex.what() << std::endl;
        return 1;
    }
}

// ----- Subscriber mode: receives and validates using Sequence field -----
int run_subscriber(const std::string& broker, const std::string& clientId, const std::string& topic, int qos, int reportIntervalSec) {
    MqttClient client(broker, clientId);
    std::atomic<uint64_t> total_received{ 0 };
    std::atomic<uint64_t> missing_messages{ 0 };
    std::atomic<uint64_t> out_of_order{ 0 };
    std::atomic<uint64_t> highest_seq{ 0 };
    std::atomic<uint64_t> last_seen{ 0 };
    std::mutex mtx;

    client.SetCallback([&](const std::string& t, const std::string& payload) {
        (void)t;
        uint64_t seq = extract_sequence(payload);
        total_received++;
        if (seq == 0) {
            // malformed or no sequence - count as out_of_order for now
            out_of_order++;
            return;
        }
        uint64_t prev = last_seen.load();
        if (prev == 0) {
            last_seen = seq;
        }
        else {
            if (seq > prev + 1) {
                missing_messages += (seq - prev - 1);
            }
            else if (seq <= prev) {
                out_of_order++;
            }
            last_seen = seq;
        }
        if (seq > highest_seq) highest_seq = seq;
        });

    client.Connect();
    if (!wait_for_connection(client, 5)) {
        std::cerr << "Subscriber: cannot connect to broker: " << broker << std::endl;
        return 1;
    }

    client.Subscribe(topic, static_cast<QoS>(qos));
    std::cout << "Subscriber listening: topic=" << topic << " qos=" << qos << std::endl;
    enable_ansi_console();

    const int REPORT_LINES = 8; // must match lines printed in the report
    // initialize counters for throughput calc
    uint64_t last_total = total_received.load();
    auto start_time = std::chrono::steady_clock::now();

    // print an initial empty report block (we will overwrite this in-place)
    std::cout << "\n--- LIVE VALIDATION REPORT ---\n";
    for (int i = 0; i < REPORT_LINES - 1; ++i) std::cout << "\n";
    std::cout << std::flush;

    double highest_rate = 0;
    // main reporting loop
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::seconds(reportIntervalSec));

        // compute metrics
        uint64_t now_total = total_received.load();
        uint64_t delta = now_total - last_total;
        last_total = now_total;
        double secs = double(reportIntervalSec);
        double rate = double(delta) / secs;
        uint64_t mm = missing_messages.load();
        uint64_t oo = out_of_order.load();
        uint64_t high = highest_seq.load();
        uint64_t received = now_total;
        if (rate > highest_rate) highest_rate = rate;

        // move cursor back up to the start of the report and overwrite
        move_cursor_up(REPORT_LINES);
        printf("--- LIVE VALIDATION REPORT ---\n");
        printf("Messages Received (total): %llu\n", (unsigned long long)received);
        printf("Messages Missing (total):  %llu\n", (unsigned long long)mm);
        printf("Out-of-Order/Duplicates:   %llu\n", (unsigned long long)oo);
        printf("Current Throughput:        %llu msg/sec\n", (unsigned long long)static_cast<uint64_t>(rate));
        printf("Highest Sequence Seen:     %llu\n", (unsigned long long)high);
        printf("Highest Throughput:        %llu msg/sec\n", (unsigned long long)highest_rate);
        printf("-------------------------------\n");
        fflush(stdout);
    }

    client.Disconnect();
    return 0;
}

// ----- simple CLI parsing and main -----
int main(int argc, char** argv) {
    signal(SIGINT, handle_sigint);
    std::string mode = "publisher";
    std::string broker = "tcp://localhost:1883";
    std::string topic = "logs/sysmon";
    std::string clientId = "mqqt-test-client";
    std::string dllPath = "MonitorEvents.dll";
    int qos = 2;
    bool retained = false;
    int reportInterval = 2;
    bool mock = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--mode" && i + 1 < argc) mode = argv[++i];
        else if (a == "--broker" && i + 1 < argc) broker = argv[++i];
        else if (a == "--topic" && i + 1 < argc) topic = argv[++i];
        else if (a == "--client-id" && i + 1 < argc) clientId = argv[++i];
        else if (a == "--dll" && i + 1 < argc) dllPath = argv[++i];
        else if (a == "--qos" && i + 1 < argc) qos = atoi(argv[++i]);
        else if (a == "--retained" && i + 1 < argc) retained = (std::string(argv[++i]) == "true");
        else if (a == "--report-interval" && i + 1 < argc) reportInterval = atoi(argv[++i]);
        else if (a == "--mock") mock = true;
    }

    if (mode == "publisher") {
        return run_publisher(broker, clientId, topic, qos, retained, dllPath, mock);
    }
    else if (mode == "subscriber") {
        return run_subscriber(broker, clientId, topic, qos, reportInterval);
    }
    else {
        std::cerr << "Unknown mode: " << mode << " (use publisher|subscriber)\n";
        return 2;
    }
}
