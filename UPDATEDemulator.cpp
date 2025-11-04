#include "globals.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <ctime>
#include <stdexcept>
#include <regex>
#include <thread>
#include <chrono>
#include <atomic>
#include <iomanip>
#include <deque>
#include <algorithm>

// === Global variables ===
std::mutex io_mutex;
std::mutex processTableMutex;
bool initialized = false;
Config systemConfig;
ConsoleMode mode = ConsoleMode::MAIN;
std::deque<Process> processTable;
std::vector<CPUCore> cpuCores;
int nextPID = 1;
std::string current_process = "";
std::atomic<bool> autoCreateRunning{ false };
std::atomic<bool> schedulerRunning{ false };
std::thread schedulerThread;
unsigned long long global_tick = 0;

// === Forward declarations ===
void scheduler_loop_tick();

// === Utility ===
std::vector<std::string> tokenize(const std::string& input) 
{
    std::istringstream stream(input);
    std::vector<std::string> tokens;
    std::string token;
    while (stream >> token) tokens.push_back(token);
    return tokens;
}

std::string trim(const std::string& str) 
{
    const auto first = str.find_first_not_of(" \t\n\r");
    if (first == std::string::npos) return "";
    const auto last = str.find_last_not_of(" \t\n\r");
    return str.substr(first, last - first + 1);
}

// === Config handling ===
bool generateDefaultConfig(const std::string& filename) 
{
    std::ofstream file(filename);
    if (!file.is_open()) return false;
    file << "num-cpu 4\nscheduler rr\nquantum-cycles 2\nbatch-process-freq 3\nmin-ins 5\nmax-ins 10\ndelays-per-exec 1\n";
    file.close();
    return true;
}

bool loadConfigFile(const std::string& filename) 
{
    std::ifstream file(filename);
    if (!file.is_open()) {
        std::cout << "Config not found. Creating defaults...\n";
        if (!generateDefaultConfig(filename)) return false;
        file.open(filename);
    }
    std::string key, value;
    while (file >> key >> value) 
    {
        if (key == "num-cpu") systemConfig.num_cpu = std::stoi(value);
        else if (key == "scheduler") systemConfig.scheduler = value;
        else if (key == "quantum-cycles") systemConfig.quantum_cycles = std::stoi(value);
        else if (key == "batch-process-freq") systemConfig.batch_process_freq = std::stoi(value);
        else if (key == "min-ins") systemConfig.min_ins = std::stoi(value);
        else if (key == "max-ins") systemConfig.max_ins = std::stoi(value);
        else if (key == "delays-per-exec") systemConfig.delays_per_exec = std::stoi(value);
    }
    file.close();

    cpuCores.clear();
    cpuCores.resize(systemConfig.num_cpu);
    for (int i = 0; i < systemConfig.num_cpu; ++i)
        cpuCores[i].id = i;

    systemConfig.loaded = true;
    return true;
}

// === Process helpers ===
Process* findProcess(const std::string& name) 
{
    for (auto& p : processTable)
        if (p.name == name)
            return &p;
    return nullptr;
}

int resolveValue(Process& p, const std::string& token) 
{
    try { return std::stoi(token); }
    catch (...) {}
    if (!p.variables.count(token)) p.variables[token] = 0;
    return p.variables[token];
}

// === Instruction execution ===
void executeInstruction(Process& p) 
{
    if (p.state == ProcessState::FINISHED) return;
    if (p.pc >= p.instructions.size()) 
    {
        p.state = ProcessState::FINISHED;
        return;
    }

    std::string instr = p.instructions[p.pc];

    if (instr.rfind("DECLARE", 0) == 0) 
    {
        std::regex re(R"(DECLARE\((\w+),\s*(-?\d+)\))");
        std::smatch m; if (std::regex_match(instr, m, re))
            p.variables[m[1]] = std::stoi(m[2]);
    }
    else if (instr.rfind("ADD", 0) == 0) 
    {
        std::regex re(R"(ADD\((\w+),\s*([\w\-]+),\s*([\w\-]+)\))");
        std::smatch m;
        if (std::regex_match(instr, m, re)) {
            int v1 = resolveValue(p, m[2]), v2 = resolveValue(p, m[3]);
            p.variables[m[1]] = v1 + v2;
        }
    }
    else if (instr.rfind("SUBTRACT", 0) == 0) 
    {
        std::regex re(R"(SUBTRACT\((\w+),\s*([\w\-]+),\s*([\w\-]+)\))");
        std::smatch m;
        if (std::regex_match(instr, m, re)) {
            int v1 = resolveValue(p, m[2]), v2 = resolveValue(p, m[3]);
            p.variables[m[1]] = v1 - v2;
        }
    }
    else if (instr.rfind("PRINT", 0) == 0) 
    {
        std::regex re(R"(PRINT\('([^']+)'\))");
        std::smatch m;
        if (std::regex_match(instr, m, re))
            p.logs.push_back(m[1]);
    }
    else if (instr.rfind("SLEEP", 0) == 0) 
    {
        std::regex re(R"(SLEEP\((\d+)\))");
        std::smatch m;
        if (std::regex_match(instr, m, re)) 
        {
            p.sleep_counter = std::stoi(m[1]);
            p.state = ProcessState::SLEEPING;
        }
    }

    p.pc++;
    if (systemConfig.delays_per_exec > 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(systemConfig.delays_per_exec));

    if (p.pc >= p.instructions.size()) p.state = ProcessState::FINISHED;
}

// === Scheduler tick ===
void scheduler_loop_tick() 
{
    static size_t rrIndex = 0;
    global_tick++;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::lock_guard<std::mutex> lock(processTableMutex);
    // Wake up sleepers
    for (auto& p : processTable)
        if (p.state == ProcessState::SLEEPING && --p.sleep_counter <= 0)
            p.state = ProcessState::READY;

    // Assign processes to idle cores
    for (auto& core : cpuCores) 
    {
        if (!core.running || core.running->state == ProcessState::FINISHED)
            core.running = nullptr;
        if (!core.running) 
        {
            for (size_t i = 0; i < processTable.size(); ++i) 
            {
                size_t idx = (rrIndex + i) % processTable.size();
                if (processTable[idx].state == ProcessState::READY) 
                {
                    processTable[idx].state = ProcessState::RUNNING;
                    core.running = &processTable[idx];
                    core.quantum_left = systemConfig.quantum_cycles;
                    rrIndex = (idx + 1) % processTable.size();
                    break;
                }
            }
        }
    }

    // Execute running processes
    for (auto& core : cpuCores) 
    {
        if (core.running && core.running->state == ProcessState::RUNNING) 
        {
            executeInstruction(*core.running);
            if (--core.quantum_left <= 0) 
            {
                core.quantum_left = systemConfig.quantum_cycles;
                core.running->state = ProcessState::READY;
                core.running = nullptr;
            }
        }
    }

    // Auto-create
    if (autoCreateRunning.load() &&
        systemConfig.batch_process_freq > 0 &&
        global_tick % systemConfig.batch_process_freq == 0) 
    {
        Process p;
        p.name = "auto_p" + std::to_string(nextPID++);
        p.pid = nextPID - 1;
        p.state = ProcessState::READY;
        int count = rand() % (systemConfig.max_ins - systemConfig.min_ins + 1) + systemConfig.min_ins;
        p.instructions = { "DECLARE(x,1)", "ADD(x,x,1)", "PRINT('Hello world!')" };
        processTable.push_back(p);
    }
}

// === Commands ===
void initializeCommand() 
{
    if (systemConfig.loaded) { std::cout << "Already initialized.\n"; return; }
    if (!loadConfigFile("config.txt")) { std::cout << "Failed to load config.\n"; return; }
    initialized = true;
    std::cout << "Initialization complete. CPUs: " << systemConfig.num_cpu << "\n";
}

void ensureSchedulerActive() 
{
    if (!schedulerRunning.load() && initialized) 
    {
        schedulerRunning.store(true);
        schedulerThread = std::thread([]() 
        {
            while (schedulerRunning.load()) 
            {
                scheduler_loop_tick();
                bool allDone = true;
                {
                    std::lock_guard<std::mutex> lock(processTableMutex);
                    for (auto& p : processTable)
                        if (p.state != ProcessState::FINISHED)
                            allDone = false;
                }
                if (allDone && !autoCreateRunning.load())
                    schedulerRunning.store(false);
            }
        });
        schedulerThread.detach();
    }
}

void handleScreenCommand(const std::vector<std::string>& args) 
{
    if (!initialized) { std::cout << "Run initialize first.\n"; return; }
    if (args.size() == 1) {
        std::cout << "screen -s <name> | screen -r <name> | screen -ls\n";
        return;
    }

    std::string flag = args[1];
    if (flag == "-s" && args.size() >= 3) 
    {
        std::string name = args[2];
        Process p; p.name = name; p.pid = nextPID++; p.state = ProcessState::READY;
        p.instructions = { "DECLARE(a,1)", "ADD(a,a,1)", "PRINT('Hello world!')" };
        processTable.push_back(p);
        std::cout << "Created process " << name << "\n";
        ensureSchedulerActive();
        mode = ConsoleMode::PROCESS;
        current_process = name;
    }
    else if (flag == "-ls") 
    {
        int run = 0, ready = 0, sleep = 0, fin = 0;
        for (auto& p : processTable)
            switch (p.state) {
            case ProcessState::RUNNING: run++; break;
            case ProcessState::READY: ready++; break;
            case ProcessState::SLEEPING: sleep++; break;
            case ProcessState::FINISHED: fin++; break;
            }
        float util = (float)run / systemConfig.num_cpu * 100.0f;
        std::cout << "\n=== CPU UTILIZATION ===\n";
        std::cout << "CPU Utilization: " << util << "%\n";
        std::cout << "Cores Used: " << run << "/" << systemConfig.num_cpu << "\n";
        std::cout << "Ready: " << ready << " | Sleeping: " << sleep << " | Finished: " << fin << "\n";
        std::cout << "=========================\n";
    }
}

// === Input Loop ===
void inputLoop() 
{
    std::string input;
    while (true) {
        std::cout << (mode == ConsoleMode::MAIN ? "CSOPESY> " : current_process + "> ");
        std::getline(std::cin, input);
        if (input.empty()) continue;
        auto t = tokenize(input);
        if (t.empty()) continue;

        std::string cmd = t[0];
        if (mode == ConsoleMode::MAIN) 
        {
            if (cmd == "initialize") initializeCommand();
            else if (cmd == "screen") handleScreenCommand(t);
            else if (cmd == "scheduler-start") { autoCreateRunning = true; ensureSchedulerActive(); }
            else if (cmd == "scheduler-stop") autoCreateRunning = false;
            else if (cmd == "report-util") reportUtilCommand();
            else if (cmd == "exit") {
                schedulerRunning.store(false);
                autoCreateRunning.store(false);
                std::cout << "Shutting down...\n";
                break;
            }
            else std::cout << "Unknown command.\n";
        }
        else if (mode == ConsoleMode::PROCESS) 
        {
            if (cmd == "exit") 
            {
                mode = ConsoleMode::MAIN;
                current_process.clear();
            }
        }
    }
}
