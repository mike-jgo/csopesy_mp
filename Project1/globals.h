#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <memory>  // for std::unique_ptr

// Forward declaration to break circular dependency
class Instruction;

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
    int pid = 0;
    ProcessState state = ProcessState::READY;
    std::vector<std::unique_ptr<Instruction>> instructions;
    int pc = 0;
    std::vector<std::string> logs;
    std::unordered_map<std::string, int> variables;
    int sleep_counter = 0;
    int quantum_used = 0;
    bool needs_cpu = true;

    // Explicitly declare move constructor and assignment
    Process(Process&& other) noexcept
        : name(std::move(other.name)),
        pid(other.pid),
        state(other.state),
        instructions(std::move(other.instructions)),
        pc(other.pc),
        logs(std::move(other.logs)),
        variables(std::move(other.variables)),
        sleep_counter(other.sleep_counter),
        quantum_used(other.quantum_used),
        needs_cpu(other.needs_cpu) {
    }

    Process& operator=(Process&& other) noexcept {
        if (this != &other) {
            name = std::move(other.name);
            pid = other.pid;
            state = other.state;
            instructions = std::move(other.instructions);
            pc = other.pc;
            logs = std::move(other.logs);
            variables = std::move(other.variables);
            sleep_counter = other.sleep_counter;
            quantum_used = other.quantum_used;
            needs_cpu = other.needs_cpu;
        }
        return *this;
    }

    // Disable copy
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;

    Process() = default;
};


// === Core structure ===
struct Core {
    Process* cur = nullptr;
    int q_left = 0;
    int delay_left = 0;
};

// === Shared globals ===
extern std::mutex io_mutex;
extern bool initialized;
extern Config systemConfig;
extern ConsoleMode mode;
extern std::vector<Process> processTable;
extern int nextPID;
extern std::string current_process;
extern unsigned long long global_tick;

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
