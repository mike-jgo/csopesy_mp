#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <deque>

// === Enums ===
enum class ConsoleMode { MAIN, PROCESS };
enum class ProcessState { READY, RUNNING, SLEEPING, FINISHED };

// === Config structure ===
struct Config {
    int num_cpu = 0;
    std::string scheduler;
    int quantum_cycles = 0;
    int batch_process_freq = 0;
    int min_ins = 0;
    int max_ins = 0;
    int delays_per_exec = 0;
    bool loaded = false;
};

// === Process structure ===
struct Process {
    std::string name;
    int pid;
    ProcessState state;
    std::vector<std::string> instructions;
    int pc = 0;
    std::vector<std::string> logs;
    std::unordered_map<std::string, int> variables;
    int sleep_counter = 0;
    int quantum_used = 0;
    bool needs_cpu = true;
};

// === Core structure ===
struct CPUCore {
    int id;
    Process* running = nullptr;
    int quantum_left = 0;
};

// === Shared globals ===
extern std::mutex io_mutex;
extern std::mutex processTableMutex;
extern bool initialized;
extern Config systemConfig;
extern ConsoleMode mode;
extern std::deque<Process> processTable;
extern std::vector<CPUCore> cpuCores;
extern int nextPID;
extern std::string current_process;
extern unsigned long long global_tick;
extern size_t rrCursor;


// === Function declarations ===
void inputLoop();
void initializeCommand();
void handleScreenCommand(const std::vector<std::string>& args);
void schedulerStartCommand();
void schedulerStopCommand();
void reportUtilCommand();
void processSmiCommand();
bool loadConfigFile(const std::string& filename);
bool generateDefaultConfig(const std::string& filename);
std::vector<std::string> generateDummyInstructions(int count);
Process* findProcess(const std::string& name);
std::vector<std::string> tokenize(const std::string& input);
