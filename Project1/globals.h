#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <deque>
#include <memory>
#include <iostream>

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

// === Forward declarations ===
class Process;

// === Instruction Interface ===
class Instruction {
public:
    virtual ~Instruction() = default;
    virtual void execute(Process& p) = 0;
    virtual std::string toString() const = 0;
};

// === Process Class ===
class Process {
public:
    std::string name;
    int pid;
    ProcessState state;
    std::vector<std::shared_ptr<Instruction>> instructions;
    int pc = 0;
    std::vector<std::string> logs;
    std::unordered_map<std::string, int> variables;
    int sleep_counter = 0;
    int quantum_used = 0;
    bool needs_cpu = true;

    Process() : pid(-1), state(ProcessState::READY) {}
};

// === CPUCore Class ===
class CPUCore {
public:
    int id;
    Process* running = nullptr;
    int quantum_left = 0;

    CPUCore() : id(-1) {}
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
// Changed to return shared_ptr<Instruction>
std::vector<std::shared_ptr<Instruction>> generateDummyInstructions(int count);
Process* findProcess(const std::string& name);
std::vector<std::string> tokenize(const std::string& input);

// Helper to parse string to instruction
std::shared_ptr<Instruction> parseInstruction(const std::string& line);
