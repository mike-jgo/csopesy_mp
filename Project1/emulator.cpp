#include "globals.h"
#include "Instruction.h"
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
#include <cctype>

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
size_t rrCursor = 0;

// === Forward declarations ===
void scheduler_loop_tick(bool hasActiveWork);

// === Utility functions ===
static std::string trim(const std::string& str) {
    const auto first = str.find_first_not_of(" \t\n\r");
    if (first == std::string::npos) return "";
    const auto last = str.find_last_not_of(" \t\n\r");
    return str.substr(first, last - first + 1);
}
std::vector<std::string> tokenize(const std::string& input) {
    std::vector<std::string> tokens;
    std::string current_token;
    bool in_quotes = false;
    
    for (size_t i = 0; i < input.length(); ++i) {
        char c = input[i];
        if (c == '"') {
            in_quotes = !in_quotes;
            current_token += c;
        } else if (std::isspace(c) && !in_quotes) {
            if (!current_token.empty()) {
                tokens.push_back(current_token);
                current_token.clear();
            }
        } else {
            current_token += c;
        }
    }
    if (!current_token.empty()) {
        tokens.push_back(current_token);
    }
    return tokens;
}

// === Config handling ===
bool generateDefaultConfig(const std::string& filename) {
    std::ofstream file(filename);
    if (!file.is_open()) {
        std::cout << "Error: Could not create " << filename << std::endl;
        return false;
    }

    file << "num-cpu 4\n";
    file << "scheduler rr\n";
    file << "quantum-cycles 2\n";
    file << "batch-process-freq 3\n";
    file << "min-ins 5\n";
    file << "max-ins 10\n";
    file << "delays-per-exec 1\n";
    file << "max-overall-mem 16384\n";
    file << "mem-per-frame 16\n";
    file << "min-mem-per-proc 4096\n";
    file << "max-mem-per-proc 4096\n";
    file.close();

    std::cout << "Default config.txt generated with safe defaults.\n";
    return true;
}

bool loadConfigFile(const std::string& filename) {
    std::ifstream file(filename);

    if (!file.is_open()) {
        std::cout << "Warning: " << filename << " not found.\n";
        std::cout << "Creating default configuration file...\n";

        if (!generateDefaultConfig(filename)) return false;

        file.open(filename);
        if (!file.is_open()) return false;
    }

    std::string key, value;
    while (file >> key >> value) {
        if (key == "num-cpu") systemConfig.num_cpu = std::stoi(value);
        else if (key == "scheduler") {
            systemConfig.scheduler = value;
            std::transform(systemConfig.scheduler.begin(), systemConfig.scheduler.end(),
                systemConfig.scheduler.begin(),
                [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        }
        else if (key == "quantum-cycles") systemConfig.quantum_cycles = std::stoi(value);
        else if (key == "batch-process-freq") systemConfig.batch_process_freq = std::stoi(value);
        else if (key == "min-ins") systemConfig.min_ins = std::stoi(value);
        else if (key == "max-ins") systemConfig.max_ins = std::stoi(value);
        else if (key == "delays-per-exec") systemConfig.delays_per_exec = std::stoi(value);
        else if (key == "max-overall-mem") systemConfig.max_overall_mem = std::stoul(value);
        else if (key == "mem-per-frame") systemConfig.mem_per_frame = std::stoul(value);
        else if (key == "min-mem-per-proc") systemConfig.min_mem_per_proc = std::stoul(value);
        else if (key == "max-mem-per-proc") systemConfig.max_mem_per_proc = std::stoul(value);
    }

    file.close();

    if (systemConfig.scheduler != "rr" && systemConfig.scheduler != "fcfs") {
        std::cout << "Warning: Unsupported scheduler '" << systemConfig.scheduler
            << "'. Defaulting to round-robin.\n";
        systemConfig.scheduler = "rr";
    }

    // Validate basic config
    if (systemConfig.num_cpu <= 0 || systemConfig.scheduler.empty()) {
        std::cout << "Invalid config. Regenerating defaults.\n";
        generateDefaultConfig(filename);
        return loadConfigFile(filename);
    }

    // Initialize CPU cores based on config
    cpuCores.clear();
    cpuCores.resize(systemConfig.num_cpu);
    for (int i = 0; i < systemConfig.num_cpu; ++i) {
        cpuCores[i].id = i;
        cpuCores[i].running = nullptr;
        cpuCores[i].quantum_left = 0;
    }

    systemConfig.loaded = true;
    std::cout << "Loaded " << systemConfig.num_cpu << " CPU cores.\n";
    std::cout << "Memory: " << systemConfig.max_overall_mem << " bytes (" 
              << systemConfig.mem_per_frame << " bytes/frame)\n";

    return true;
}


// === Process helpers ===
Process* findProcess(const std::string& name) {
    for (auto& p : processTable)
        if (p.name == name)
            return &p;
    return nullptr;
}

// Generate dummy instructions for a process 
std::vector<std::shared_ptr<Instruction>> generateDummyInstructions(int count, int memSize) {
    std::vector<std::shared_ptr<Instruction>> ins;
    static std::vector<std::string> pool = {
        "DECLARE(x, 5)",
        "DECLARE(y, 10)",
        "ADD(sum, x, y)",
        "SUBTRACT(diff, y, x)",
        "PRINT('Hello world!')",
        "PRINT('Value of sum: ' + sum)",
        "SLEEP(2)",
        "FOR([PRINT('Hello world!')], 2)",
        "WRITE(%ADDR%, 42)",   
        "READ(val, %ADDR%)",  
        "PRINT('Loaded value: ' + val)"
    };
    for (int i = 0; i < count; ++i) {
        std::string line = pool[rand() % pool.size()];
        
        // Replace %ADDR% with random address
        size_t pos = line.find("%ADDR%");
        if (pos != std::string::npos) {
            int randomAddr = rand() % memSize;
            line.replace(pos, 6, std::to_string(randomAddr));
        }

        auto inst = parseInstruction(line);
        if (inst) ins.push_back(inst);
    }
    return ins;
}

// Trace function
void logInstructionTrace(Process& p, const std::shared_ptr<Instruction>& instr) {
    std::ofstream trace("csopesy-trace.txt", std::ios::app);
    if (!trace.is_open()) return;

    // Real timestamp
    auto now = std::chrono::system_clock::now();
    std::time_t now_time = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
#ifdef _WIN32
    localtime_s(&tm_buf, &now_time);
#else
    localtime_r(&now_time, &tm_buf);
#endif

    std::ostringstream timestamp;
    timestamp << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S");

    // Process state as string
    std::string stateStr;
    switch (p.state) {
    case ProcessState::READY: stateStr = "READY"; break;
    case ProcessState::RUNNING: stateStr = "RUNNING"; break;
    case ProcessState::SLEEPING: stateStr = "SLEEPING"; break;
    case ProcessState::FINISHED: stateStr = "FINISHED"; break;
    }

    // Combined log entry
    trace << "[" << timestamp.str() << "] ";

    trace << "[Tick " << global_tick;
    if (systemConfig.scheduler == "rr" && systemConfig.quantum_cycles > 0) {
        int quantumPos = (p.pc % systemConfig.quantum_cycles) + 1;
        trace << " | Q" << quantumPos << "/" << systemConfig.quantum_cycles;
    }
    else if (systemConfig.scheduler == "fcfs") {
        trace << " | FCFS";
    }
    trace << "] "
        << p.name << " [PID " << p.pid << "] pc="
        << p.pc << "/" << p.instructions.size()
        << " -> " << instr->toString()
        << " | State=" << stateStr << "\n";
    trace.close();
}

// Ensure the scheduler thread is running
void ensureSchedulerActive() {
    if (!schedulerRunning.load() && initialized) {
        schedulerRunning.store(true);
        schedulerThread = std::thread([]() {
            while (schedulerRunning.load()) {

                bool shouldTick = false;
                {
                    std::lock_guard<std::mutex> lock(processTableMutex);
                    shouldTick = std::any_of(processTable.begin(), processTable.end(),
                        [](const Process& p) {
                            return p.state == ProcessState::READY ||
                                p.state == ProcessState::RUNNING ||
                                p.state == ProcessState::SLEEPING;
                        });
                }

                bool tickNow = shouldTick || autoCreateRunning.load();
                if (tickNow) {
                    scheduler_loop_tick(tickNow);
                }
                else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }

                bool shouldStop = false;
                {
                    std::lock_guard<std::mutex> lock(processTableMutex);
                    bool allFinished = !processTable.empty() &&
                        std::all_of(processTable.begin(), processTable.end(),
                            [](const Process& p) { return p.state == ProcessState::FINISHED; });

                    if (allFinished && !autoCreateRunning.load()) {
                        schedulerRunning.store(false);
                        shouldStop = true;
                    }
                }

                if (shouldStop) {
                    std::cout << "[Tick " << global_tick
                        << "] Scheduler halted (all processes finished).\n";
                    break;
                }
            }
            });
        schedulerThread.detach();
        std::cout << "Scheduler thread started.\n";
    }
}

// === COMMANDS ===
// initialize command
void initializeCommand() {
    if (systemConfig.loaded) {
        std::cout << "System already initialized.\n";
        return;
    }

    std::cout << "Initializing system from config.txt...\n";

    if (!loadConfigFile("config.txt")) {
        std::cout << "Initialization failed. Please check config.txt.\n";
        return;
    }

    initialized = true;
    std::cout << "Configuration loaded successfully:\n";
    std::cout << "  num-cpu: " << systemConfig.num_cpu << "\n";
    std::cout << "  scheduler: " << systemConfig.scheduler << "\n";
    std::cout << "  quantum-cycles: " << systemConfig.quantum_cycles << "\n";
    std::cout << "  batch-process-freq: " << systemConfig.batch_process_freq << "\n";
    std::cout << "  instruction range: " << systemConfig.min_ins << "-" << systemConfig.max_ins << "\n";
    std::cout << "  delays-per-exec: " << systemConfig.delays_per_exec << "\n";
    
    // Initialize Memory Manager
    size_t total_frames = systemConfig.max_overall_mem / systemConfig.mem_per_frame;
    memoryManager = std::make_unique<MemoryManager>(total_frames, systemConfig.mem_per_frame);
    
    std::cout << "  Memory Initialized: " << total_frames << " frames x " 
              << systemConfig.mem_per_frame << " bytes\n";

    std::cout << "System initialization complete.\n\n";
}


// screen
void handleScreenCommand(const std::vector<std::string>& args) {
    if (!initialized) {
        std::cout << "Error: System not initialized. Type 'initialize' first.\n";
        return;
    }
    if (args.size() == 1) {
        std::cout << "Usage:\n"
            << "  screen -s <process_name> <memory>\n"
            << "  screen -r <process_name>\n"
            << "  screen -ls\n";
        return;
    }
    std::string flag = args[1];

    // --- Create new process ---
    if (flag == "-s") {
        if (args.size() != 4) {
            std::cout << "Usage: screen -s <process_name> <memory>\n";
            return;
        }

        std::string name = args[2];
        int memory = 0;
        try {
            memory = std::stoi(args[3]);
        }
        catch (...) {
            std::cout << "Error: Invalid memory argument. Must be an integer.\n";
            return;
        }

        // Validate power of 2
        if (memory <= 0 || (memory & (memory - 1)) != 0) {
            std::cout << "Error: Memory must be a power of 2.\n";
            return;
        }

        // Validate range
        if (memory < systemConfig.min_mem_per_proc || memory > systemConfig.max_mem_per_proc) {
            std::cout << "invalid memory allocation\n";
            return;
        }

        Process newProc;
        newProc.name = name;
        newProc.state = ProcessState::READY;
        int insCount = rand() % (systemConfig.max_ins - systemConfig.min_ins + 1) + systemConfig.min_ins;
        newProc.instructions = generateDummyInstructions(insCount, memory);

        // Memory Allocation
        newProc.memory_required = memory;
        int pages = (memory + systemConfig.mem_per_frame - 1) / systemConfig.mem_per_frame;
        memoryManager->initializePageTable(newProc, pages);

        {
            std::lock_guard<std::mutex> lock(processTableMutex);
            if (findProcess(name)) {
                std::cout << "Process " << name << " already exists.\n";
                return;
            }
            newProc.pid = nextPID++;
            processTable.push_back(newProc);
        }
        
        std::cout << "Created new process: " << name << " (PID " << newProc.pid << ") with " << memory << " bytes.\n";
        std::cout << "Attached to process screen.\n";
        ensureSchedulerActive();

        mode = ConsoleMode::PROCESS;
        current_process = name;
    }
    // --- Create new process with instructions (-c) ---
    else if (flag == "-c") {
        if (args.size() != 5) {
            std::cout << "Usage: screen -c <process_name> <memory> \"<instructions>\"\n";
            return;
        }

        std::string name = args[2];
        int memory = 0;
        try {
            memory = std::stoi(args[3]);
        }
        catch (...) {
            std::cout << "Error: Invalid memory argument. Must be an integer.\n";
            return;
        }

        // Validate power of 2
        if (memory <= 0 || (memory & (memory - 1)) != 0) {
            std::cout << "Error: Memory must be a power of 2.\n";
            return;
        }

        // Validate range
        if (memory < systemConfig.min_mem_per_proc || memory > systemConfig.max_mem_per_proc) {
            std::cout << "invalid memory allocation\n";
            return;
        }

        std::string instrString = args[4];
        // Remove surrounding quotes if present
        if (instrString.size() >= 2 && instrString.front() == '"' && instrString.back() == '"') {
            instrString = instrString.substr(1, instrString.size() - 2);
        }

        std::vector<std::shared_ptr<Instruction>> parsedInstructions;
        std::stringstream ss(instrString);
        std::string segment;
        
        while (std::getline(ss, segment, ';')) {
            std::string trimmed = trim(segment);
            if (trimmed.empty()) continue;
            auto inst = parseInstruction(trimmed);
            if (!inst) {
                std::cout << "Invalid command: " << trimmed << "\n";
                return;
            }
            parsedInstructions.push_back(inst);
        }

        if (parsedInstructions.empty() || parsedInstructions.size() > 50) {
            std::cout << "invalid command\n";
            return;
        }

        Process newProc;
        newProc.name = name;
        newProc.state = ProcessState::READY;
        newProc.instructions = parsedInstructions;
        newProc.memory_required = memory;

        // Memory Allocation
        int pages = (memory + systemConfig.mem_per_frame - 1) / systemConfig.mem_per_frame;
        memoryManager->initializePageTable(newProc, pages);

        {
            std::lock_guard<std::mutex> lock(processTableMutex);
            if (findProcess(name)) {
                std::cout << "Process " << name << " already exists.\n";
                return;
            }
            newProc.pid = nextPID++;
            processTable.push_back(newProc);
        }

        std::cout << "Created new process: " << name << " (PID " << newProc.pid << ") with " << memory << " bytes and " << parsedInstructions.size() << " instructions.\n";
        std::cout << "Attached to process screen.\n";
        ensureSchedulerActive();

        mode = ConsoleMode::PROCESS;
        current_process = name;
    }
    // --- Reattach to an existing process ---
    else if (flag == "-r" && args.size() >= 3) {
        std::string name = args[2];
        bool found = false;
        bool finished = false;
        int pid = 0;
        {
            std::lock_guard<std::mutex> lock(processTableMutex);
            Process* p = findProcess(name);
            if (p) {
                found = true;
                pid = p->pid;
                finished = (p->state == ProcessState::FINISHED);
            }
        }

        if (!found) {
            std::cout << "Process " << name << " not found.\n";
            return;
        }

        if (finished) {
            std::cout << "Process " << name << " already finished.\n";
            return;
        }

        std::cout << "Reattached to process " << name << " (PID " << pid << ")\n";
        mode = ConsoleMode::PROCESS;
        current_process = name;
    }

    // --- List all processes ---
    else if (flag == "-ls") {
        std::deque<Process> snapshot;
        size_t rrCursorSnapshot = 0;
        {
            std::lock_guard<std::mutex> lock(processTableMutex);
            if (processTable.empty()) {
                std::cout << "No processes created.\n";
                return;
            }
            snapshot.assign(processTable.begin(), processTable.end());
            rrCursorSnapshot = rrCursor;
            if (!snapshot.empty()) {
                rrCursorSnapshot %= snapshot.size();
            }
        }

        int totalCores = systemConfig.num_cpu;
        int runningCount = 0, finishedCount = 0, readyCount = 0, sleepingCount = 0;

        for (const auto& p : snapshot) {
            switch (p.state) {
            case ProcessState::RUNNING:   runningCount++; break;
            case ProcessState::READY:     readyCount++; break;
            case ProcessState::SLEEPING:  sleepingCount++; break;
            case ProcessState::FINISHED:  finishedCount++; break;
            }
        }

        float utilization = (totalCores > 0)
            ? (float)runningCount / totalCores * 100.0f
            : 0.0f;

        std::cout << "\n=== CPU SUMMARY ===\n";
        std::cout << "CPU Utilization: " << utilization << "%\n";
        std::cout << "Cores Used: " << runningCount << "/" << totalCores << "\n";
        std::cout << "Cores Available: " << (totalCores - runningCount) << "\n";
        std::cout << "Ready: " << readyCount
            << " | Sleeping: " << sleepingCount
            << " | Finished: " << finishedCount << "\n";

        std::cout << "\n=== PROCESS TABLE ===\n";

        // Print all RUNNING and SLEEPING processes first
        for (const auto& p : snapshot) {
            if (p.state == ProcessState::RUNNING || p.state == ProcessState::SLEEPING) {
                std::string stateStr = (p.state == ProcessState::RUNNING ? "RUNNING" : "SLEEPING");
                std::cout << "  " << p.name << " [PID " << p.pid << "] - "
                    << stateStr << " (" << p.pc << "/" << p.instructions.size() << ")\n";
            }
        }

        // Show next 4 READY processes based on actual scheduler order
        std::vector<Process*> readyList;

        if (systemConfig.scheduler == "rr" && !snapshot.empty()) {
            size_t total = snapshot.size();
            size_t added = 0;
            for (size_t offset = 0; offset < total && added < 4; ++offset) {
                size_t idx = (rrCursorSnapshot + offset) % total;
                if (snapshot[idx].state == ProcessState::READY) {
                    readyList.push_back(&snapshot[idx]);
                    added++;
                }
            }
        }
        else if (systemConfig.scheduler == "fcfs") {
            for (auto& p : snapshot) {
                if (p.state == ProcessState::READY) {
                    readyList.push_back(&p);
                    if (readyList.size() >= 4) break;
                }
            }
        }

        // Display the READY list
        for (auto* p : readyList) {
            std::cout << "  " << p->name << " [PID " << p->pid << "] - READY ("
                << p->pc << "/" << p->instructions.size() << ")\n";
        }

        if (runningCount == 0 && sleepingCount == 0 && readyList.empty())
            std::cout << "  (No active or upcoming processes)\n";

        bool printedFinished = false;
        for (const auto& p : snapshot) {
            if (p.state == ProcessState::FINISHED) {
                if (!printedFinished) {
                    std::cout << "\n=== COMPLETED PROCESSES ===\n";
                    printedFinished = true;
                }
                std::cout << "  " << p.name << " [PID " << p.pid << "] - FINISHED ("
                    << p.pc << "/" << p.instructions.size() << ")\n";
            }
        }
        if (!printedFinished)
            std::cout << "\n(No completed processes yet)\n";


        std::cout << "=====================\n\n";
    }
}

// Scheduler-start/stop command handler
void handleSchedulerCommand(const std::vector<std::string>& args) {
    if (!initialized) {
        std::cout << "Error: System not initialized. Type 'initialize' first.\n";
        return;
    }

    if (args.size() == 1) {
        std::cout << "Usage:\n"
            << "  scheduler start\n"
            << "  scheduler stop\n";
        return;
    }

    const std::string& subcmd = args[1];

    if (subcmd == "start") {
        if (autoCreateRunning.load()) {
            std::cout << "Auto-creation is already running (every "
                << systemConfig.batch_process_freq << " tick"
                << (systemConfig.batch_process_freq == 1 ? "" : "s") << ").\n";
            // Still ensure the scheduler thread is alive.
            ensureSchedulerActive();
            return;
        }
        autoCreateRunning.store(true);

        ensureSchedulerActive();

        std::cout << "Auto-creation started new process every "
            << systemConfig.batch_process_freq << " tick"
            << (systemConfig.batch_process_freq == 1 ? "" : "s") << ".\n";
    }
    else if (subcmd == "stop") {
        if (!autoCreateRunning.load()) {
            std::cout << "Auto-creation is not running.\n";
            return;
        }
        autoCreateRunning.store(false);
        std::cout << "Auto-creation stopped.\n";
    }
    else {
        std::cout << "Invalid command. Use 'scheduler start' or 'scheduler stop'.\n";
    }
}


// scheduler-start
// === Multi-core scheduler (RR / FCFS) ===
void scheduler_loop_tick(bool hasActiveWork) {
    const auto activeTickDelay = std::chrono::milliseconds(5);
    const auto idleTickDelay = std::chrono::milliseconds(100);

    std::this_thread::sleep_for(hasActiveWork ? activeTickDelay : idleTickDelay);
    global_tick++;

    auto assignReadyToIdleCores = [&]() {
        size_t tableSize = processTable.size();
        if (tableSize == 0) {
            rrCursor = 0;
        }

        for (auto& core : cpuCores) {
            if (core.running) {
                continue;
            }

            tableSize = processTable.size();
            if (tableSize == 0) {
                rrCursor = 0;
                core.running = nullptr;
                continue;
            }

            if (systemConfig.scheduler == "rr" && tableSize > 0 && rrCursor >= tableSize) {
                rrCursor %= tableSize;
            }

            size_t chosenIndex = tableSize;
            if (systemConfig.scheduler == "rr") {
                for (size_t offset = 0; offset < tableSize; ++offset) {
                    size_t idx = (rrCursor + offset) % tableSize;
                    if (processTable[idx].state == ProcessState::READY) {
                        chosenIndex = idx;
                        rrCursor = (idx + 1) % tableSize;
                        break;
                    }
                }
            }
            else {
                for (size_t idx = 0; idx < tableSize; ++idx) {
                    if (processTable[idx].state == ProcessState::READY) {
                        chosenIndex = idx;
                        break;
                    }
                }
            }

            if (chosenIndex != tableSize) {
                core.running = &processTable[chosenIndex];
                processTable[chosenIndex].state = ProcessState::RUNNING;
                core.quantum_left = (systemConfig.scheduler == "rr")
                    ? systemConfig.quantum_cycles
                    : 0;
            }
        }
        };

    // === 1. Wake up sleeping processes ===
    for (auto& p : processTable) {
        if (p.state == ProcessState::SLEEPING && p.sleep_counter > 0) {
            p.sleep_counter--;
            if (p.sleep_counter == 0) {
                p.state = ProcessState::READY;
            }
        }
    }

    // === 2. Assign ready processes to idle cores ===
    for (auto& core : cpuCores) {
        if (!core.running || core.running->state == ProcessState::FINISHED) {
            core.running = nullptr;
        }
    }

    assignReadyToIdleCores();

    // === 3. Execute processes on each core ===
    bool rescheduleNeeded = false;
    for (auto& core : cpuCores) {
        if (core.running && core.running->state == ProcessState::RUNNING) {
            Process* p = core.running;

            // Execute one instruction = one tick
            if (p->pc < p->instructions.size()) {
                auto currentInstr = p->instructions[p->pc];
                logInstructionTrace(*p, currentInstr);
                currentInstr->execute(*p);

                if (systemConfig.scheduler == "rr") {
                    core.quantum_left--;
                }

                // Handle post-execution logic
                if (p->state == ProcessState::FINISHED) {
                core.running = nullptr;
                rescheduleNeeded = true;
                }
                else if (p->state == ProcessState::MEMORY_VIOLATED) {
                    // Log the violation to console
                    std::cout << "Process " << p->name << " (" << p->pid << ") terminated due to Memory Violation.\n";
                    core.running = nullptr; // Release the core
                    rescheduleNeeded = true;
                }
                else if (p->pc >= p->instructions.size()) {
                    p->state = ProcessState::FINISHED;
                    core.running = nullptr;
                    rescheduleNeeded = true;
                }
                else if (p->state == ProcessState::SLEEPING) {
                    core.running = nullptr;
                    rescheduleNeeded = true;
                }
                else if (systemConfig.scheduler == "rr" &&
                    core.quantum_left <= 0) {
                    // Quantum expired — check if another READY process exists
                    bool hasOtherReady = std::any_of(processTable.begin(), processTable.end(),
                        [&](const Process& other) {
                            return other.state == ProcessState::READY &&
                                &other != p;
                        });

                    if (hasOtherReady) {
                        p->state = ProcessState::READY;
                        core.running = nullptr; // Preempt
                        rescheduleNeeded = true;
                        core.quantum_left = systemConfig.quantum_cycles;

                        auto it = std::find_if(processTable.begin(), processTable.end(),
                            [&](Process& candidate) { return &candidate == p; });
                        if (it != processTable.end()) {
                            size_t idx = static_cast<size_t>(std::distance(processTable.begin(), it));
                            size_t tableSize = processTable.size();
                            if (tableSize > 0) {
                                rrCursor = (idx + 1) % tableSize;
                            }
                            else {
                                rrCursor = 0;
                            }
                        }
                    }
                    else {
                        // No other ready — keep executing
                        core.quantum_left = systemConfig.quantum_cycles;
                    }
                }
            }
            else {
                // PC out of bounds, finish
                p->state = ProcessState::FINISHED;
                core.running = nullptr;
                rescheduleNeeded = true;
            }
        }
        else if (!core.running) {
            rescheduleNeeded = true;
        }
    }

    if (rescheduleNeeded) {
        assignReadyToIdleCores();
    }

    // === 4. Auto-create processes if enabled ===
    if (autoCreateRunning.load() &&
        systemConfig.batch_process_freq > 0 &&
        global_tick % systemConfig.batch_process_freq == 0) {

        // Only create one process per batch frequency, not potentially multiple
        static unsigned long long lastCreationTick = 0;

        static auto lastCreationWallClock = std::chrono::steady_clock::now();
        const auto creationCooldown = std::chrono::milliseconds(100);

        const auto now = std::chrono::steady_clock::now();
        if (global_tick != lastCreationTick && now - lastCreationWallClock >= creationCooldown) {
            Process newProc;
            newProc.name = "auto_p" + std::to_string(nextPID++);
            newProc.pid = nextPID - 1;
            newProc.state = ProcessState::READY;
            int insCount = rand() % (systemConfig.max_ins - systemConfig.min_ins + 1) + systemConfig.min_ins;
            
            // Memory Allocation
            size_t memSize = rand() % (systemConfig.max_mem_per_proc - systemConfig.min_mem_per_proc + 1) + systemConfig.min_mem_per_proc;
            newProc.memory_required = memSize;
            newProc.instructions = generateDummyInstructions(insCount, (int)memSize);
            int pages = (memSize + systemConfig.mem_per_frame - 1) / systemConfig.mem_per_frame;
            memoryManager->initializePageTable(newProc, pages);
            
            processTable.push_back(newProc);

            lastCreationWallClock = now;
        }
    }
}

// report-util 
void reportUtilCommand()
{
    if (!initialized)
    {
        std::cout << "Error: System not initialized. Type 'initialize' first.\n";
        return;
    }

    int running = 0, ready = 0, sleeping = 0, finished = 0;
    {
        std::lock_guard<std::mutex> lock(processTableMutex);
        for (auto& p : processTable)
        {
            switch (p.state)
            {
            case ProcessState::RUNNING:   running++; break;
            case ProcessState::READY:     ready++; break;
            case ProcessState::SLEEPING:  sleeping++; break;
            case ProcessState::FINISHED:  finished++; break;
            }
        }
    }

    float utilization = (systemConfig.num_cpu > 0)
        ? (float)running / systemConfig.num_cpu * 100.0f
        : 0.0f;

    std::cout << "\n=== CPU UTILIZATION REPORT ===\n";
    std::cout << "CPU Utilization: " << utilization << "%\n";
    std::cout << "Cores Used: " << running << "/" << systemConfig.num_cpu << "\n";
    std::cout << "Ready: " << ready
        << " | Sleeping: " << sleeping
        << " | Finished: " << finished << "\n";

    {
        std::lock_guard<std::mutex> lock(processTableMutex);
        std::cout << "\n=== PROCESS DETAILS ===\n";
        for (auto& p : processTable)
        {
            std::string stateStr;
            switch (p.state)
            {
            case ProcessState::READY:     stateStr = "READY"; break;
            case ProcessState::RUNNING:   stateStr = "RUNNING"; break;
            case ProcessState::SLEEPING:  stateStr = "SLEEPING"; break;
            case ProcessState::FINISHED:  stateStr = "FINISHED"; break;
            default:                      stateStr = "UNKNOWN"; break;
            }

            std::cout << "  " << p.name
                << " [PID " << p.pid << "] - " << stateStr
                << " (" << p.pc << "/" << p.instructions.size() << ")\n";
        }
        std::cout << "===============================\n";
    }

    std::cout << "Report saved to csopesy-log.txt\n";
    std::cout << "===============================\n\n";

    std::ofstream log("csopesy-log.txt");
    if (!log.is_open())
    {
        std::cout << "Error: Unable to create csopesy-log.txt\n";
        return;
    }

    log << "=== CSOPESY CPU UTILIZATION REPORT ===\n";
    log << "CPU Utilization: " << utilization << "%\n";
    log << "Cores Used: " << running << "/" << systemConfig.num_cpu << "\n";
    log << "Ready: " << ready
        << " | Sleeping: " << sleeping
        << " | Finished: " << finished << "\n";
    log << "======================================\n";

    std::lock_guard<std::mutex> lock(processTableMutex);
    if (processTable.empty()) {
        log << "No processes created.\n";
    }
    else {
        log << "=== PROCESS TABLE ===\n";
        for (const auto& p : processTable) {
            std::string stateStr;
            switch (p.state) {
            case ProcessState::RUNNING:   stateStr = "RUNNING"; break;
            case ProcessState::READY:     stateStr = "READY"; break;
            case ProcessState::SLEEPING:  stateStr = "SLEEPING"; break;
            case ProcessState::FINISHED:  stateStr = "FINISHED"; break;
            }

            log << "  " << p.name << " [PID " << p.pid << "] - "
                << stateStr << " (" << p.pc << "/" << p.instructions.size() << ")\n";
        }
        log << "=====================\n";
    }

    log.close();
}

// process-smi inside process screen
void processSmiCommand() {
    Process procSnapshot;
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(processTableMutex);
        Process* proc = findProcess(current_process);
        if (proc) {
            found = true;
            procSnapshot = *proc; // Copy
        }
    }

    if (!found) {
        std::cout << "Error: Process " << current_process << " not found.\n";
        return;
    }

    std::cout << "\n=== Process SMI ===\n";
    std::cout << "Name: " << procSnapshot.name << "\n";
    std::cout << "PID: " << procSnapshot.pid << "\n";

    // Translate enum to string
    std::string stateStr;
    switch (procSnapshot.state) {
    case ProcessState::READY: stateStr = "READY"; break;
    case ProcessState::RUNNING: stateStr = "RUNNING"; break;
    case ProcessState::SLEEPING: stateStr = "SLEEPING"; break;
    case ProcessState::FINISHED: stateStr = "FINISHED"; break;
    case ProcessState::MEMORY_VIOLATED: stateStr = "MEMORY_VIOLATED"; break;
    }
    std::cout << "State: " << stateStr << "\n";

    // Instruction progress
    std::cout << "Instruction progress: " << procSnapshot.pc << " / " << procSnapshot.instructions.size() << "\n";

    // === Display Variables with Values from Memory ===
    if (!procSnapshot.symbol_table.empty()) {
        std::cout << "Variables (Stored in Page 0):\n";
        for (const auto& [name, addr] : procSnapshot.symbol_table) {
            std::cout << "  " << name << " @ Address " << addr;

            // Check if the page containing this variable is currently in RAM
            if (memoryManager->isPageResident(procSnapshot.pid, addr)) {
                int val = 0;
                // Attempt to read the value (this updates LRU but that is acceptable)
                if (memoryManager->access(procSnapshot.pid, addr, false, val)) {
                    std::cout << " = " << val;
                }
                else {
                    std::cout << " = (Error reading)";
                }
            }
            else {
                // If the page is not in RAM, we show this tag. 
                // This adds realism: you can't see the value because it's on the 'disk'.
                std::cout << " = [Swapped Out]";
            }
            std::cout << "\n";
        }
    }
    else {
        std::cout << "Variables: (none)\n";
    }

    // Display logs
    if (!procSnapshot.logs.empty()) {
        std::cout << "Logs:\n";
        for (const auto& log : procSnapshot.logs)
            std::cout << "  " << log << "\n";
    }
    else {
        std::cout << "Logs: (none)\n";
    }

    if (procSnapshot.state == ProcessState::FINISHED)
        std::cout << "Process has finished execution.\n";

    // Display Page Table
    std::cout << "\n--- Page Table ---\n";
    std::cout << "Total Frames: " << memoryManager->getTotalFrames() << "\n";
    std::cout << "Free Frames: " << memoryManager->getFreeFrameCount() << "\n";
    std::cout << "Page | Frame | Valid | Dirty | Last Accessed\n";
    for (const auto& [page, entry] : procSnapshot.page_table) {
        std::cout << "  " << page << "  | "
            << (entry.valid ? std::to_string(entry.frame_num) : "-") << "   | "
            << (entry.valid ? "Yes" : "No ") << "   | "
            << (entry.dirty ? "Yes" : "No ") << "   | "
            << entry.last_accessed << "\n";
    }

    std::cout << "=====================\n\n";
}

void vmstatCommand() {
    if (!initialized || !memoryManager) {
        std::cout << "Error: System not initialized.\n";
        return;
    }

    size_t total_mem = systemConfig.max_overall_mem;
    size_t used_mem = memoryManager->getUsedMemory();
    size_t free_mem = total_mem - used_mem;

    // Mock CPU ticks (since we don't track them granularly yet)
    unsigned long long idle_ticks = global_tick * systemConfig.num_cpu; // Simplification
    unsigned long long active_ticks = global_tick; // Simplification

    VMStatCounters stats = memoryManager->getVMStat();

    std::cout << "\n=== VMSTAT ===\n";
    std::cout << total_mem << " K total memory\n";
    std::cout << used_mem << " K used memory\n";
    std::cout << free_mem << " K free memory\n";
    std::cout << idle_ticks << " idle cpu ticks\n";
    std::cout << active_ticks << " active cpu ticks\n";
    std::cout << stats.pages_paged_in << " pages paged in\n";
    std::cout << stats.pages_paged_out << " pages paged out\n";
    std::cout << "=================\n\n";
}

void processSmiGlobal() {
    if (!initialized || !memoryManager) {
        std::cout << "Error: System not initialized.\n";
        return;
    }

    // ---- CPU UTILIZATION ----
    std::deque<Process> snapshot;
    size_t rrCursorSnapshot = 0;
    {
        std::lock_guard<std::mutex> lock(processTableMutex);
        if (processTable.empty()) {
            std::cout << "No processes created.\n";
            return;
        }
        snapshot.assign(processTable.begin(), processTable.end());
        rrCursorSnapshot = rrCursor;
        if (!snapshot.empty()) {
            rrCursorSnapshot %= snapshot.size();
        }
    }

    int totalCores = systemConfig.num_cpu;
    int runningCount = 0, finishedCount = 0, readyCount = 0, sleepingCount = 0;

    size_t total_mem = systemConfig.max_overall_mem;
    size_t used_mem = memoryManager->getUsedMemory();
    size_t free_mem = total_mem - used_mem;

    struct ProcSummary {
        std::string name;
        int pid;
        std::string state;
        int memReq;
        int totalPages;
        int residentPages;
        int dirtyPages;
        size_t ramUsage;
    };

    std::vector<ProcSummary> list;

    for (const auto& p : snapshot) {
        int totalPages = 0, resident = 0, dirty = 0;

        for (const auto& [pg, entry] : p.page_table) {
            totalPages++;
            if (entry.valid) resident++;
            if (entry.dirty) dirty++;
        }

        std::string stateStr;
        switch (p.state) {
        case ProcessState::READY:          stateStr = "READY"; break;
        case ProcessState::RUNNING:        stateStr = "RUNNING"; break;
        case ProcessState::SLEEPING:       stateStr = "SLEEPING"; break;
        case ProcessState::FINISHED:       stateStr = "FINISHED"; break;
        case ProcessState::MEMORY_VIOLATED:stateStr = "MEM VIOL"; break;
        default:                           stateStr = "UNKNOWN"; break;
        }

        size_t ramUsed = static_cast<size_t>(resident) * systemConfig.mem_per_frame;

        list.push_back({
            p.name,
            p.pid,
            stateStr,
            p.memory_required,
            totalPages,
            resident,
            dirty,
            ramUsed
            });
    }

    float utilization = (totalCores > 0)
        ? (float)runningCount / totalCores * 100.0f
        : 0.0f;

    std::sort(list.begin(), list.end(),
        [](const ProcSummary& a, const ProcSummary& b) {
            return a.ramUsage > b.ramUsage;
        });

    size_t totalResidentRAM = 0;
    for (auto& p : list) totalResidentRAM += p.ramUsage;

    std::cout << "\n========================== PROCESS-SMI (GLOBAL) ==========================\n";
    std::cout << "CPU Utilization: " << utilization << "%\n";
    std::cout << "Total Memory: " << total_mem << " bytes\n";
    std::cout << "Used Memory:  " << used_mem << " bytes\n";
    std::cout << "Free Memory:  " << free_mem << " bytes\n";
    std::cout << "Memory Util:" << (used_mem / total_mem) * 100.f;
    std::cout << "\n--------------------------------------------------------------------------\n";
    std::cout << "Total Resident Memory (All Processes): "
        << totalResidentRAM << " bytes\n";
    std::cout << "--------------------------------------------------------------------------\n";

    if (list.empty()) {
        std::cout << "No processes found.\n";
        std::cout << "==================================================================\n\n";
        return;
    }

    std::cout << std::left
        << std::setw(12) << "Name"
        << std::setw(7) << "PID"
        << std::setw(12) << "State"
        << std::setw(10) << "MemReq"
        << std::setw(8) << "Pages"
        << std::setw(10) << "Resident"
        << std::setw(8) << "Dirty"
        << std::setw(10) << "RAM Used"
        << "\n";

    std::cout << "---------------------------------------------------------------------------\n";

    for (const auto& p : list) {
        std::cout << std::left
            << std::setw(12) << p.name
            << std::setw(7) << p.pid
            << std::setw(12) << p.state
            << std::setw(10) << p.memReq
            << std::setw(8) << p.totalPages
            << std::setw(10) << p.residentPages
            << std::setw(8) << p.dirtyPages
            << std::setw(10) << p.ramUsage
            << "\n";
    }

    std::cout << "===========================================================================\n\n";
}

// === INPUT LOOP ===
void inputLoop() {
    std::string input;
    while (true) {
        {
            std::lock_guard<std::mutex> lock(io_mutex);
            std::cout << (mode == ConsoleMode::MAIN ? "CSOPESY> " : current_process + "> ") << std::flush;
        }

        std::getline(std::cin, input);
        if (input.empty()) continue;

        std::vector<std::string> tokens = tokenize(input);
        if (tokens.empty()) continue;

        std::string cmd = tokens[0];

        // === MAIN CONSOLE MODE ===
        if (mode == ConsoleMode::MAIN) {
            if (cmd == "help") {
                std::cout << "Available commands:\n"
                    << "  initialize          - Load configuration and start scheduler\n"
                    << "  screen              - Create or manage processes\n"
                    << "  scheduler start     - Begin automatic process creation\n"
                    << "  scheduler stop      - Stop automatic process creation\n"
                    << "  report-util         - Generate CPU report\n"
                    << "  report-trace        - Show execution trace log\n"
                    << "  exit                - Quit program\n";
            }
            else if (cmd == "initialize") initializeCommand();
            else if (cmd == "screen") handleScreenCommand(tokens);
            else if (cmd == "scheduler") handleSchedulerCommand(tokens);
            else if (cmd == "report-util") reportUtilCommand();
            else if (cmd == "vmstat") vmstatCommand();
            else if (cmd == "process-smi") processSmiGlobal();
            else if (cmd == "report-trace") {
                std::ifstream trace("csopesy-trace.txt");
                if (!trace.is_open()) {
                    std::cout << "No trace log found.\n";
                    continue;
                }
                std::cout << "\n=== EXECUTION TRACE ===\n";
                std::string line;
                while (std::getline(trace, line)) std::cout << line << "\n";
                std::cout << "=======================\n";
            }
            else if (cmd == "exit") break;
            else std::cout << "Unknown command. Type 'help'.\n";
        }

        // === PROCESS MODE ===
        else if (mode == ConsoleMode::PROCESS) {
            if (cmd == "process-smi") processSmiCommand();
            else if (cmd == "step") {
                int pcAfter = 0;
                std::string procName;
                bool found = false;
                {
                    std::lock_guard<std::mutex> lock(processTableMutex);
                    Process* p = findProcess(current_process);
                    if (p) {
                        found = true;
                        procName = p->name;
                        if (p->pc < p->instructions.size()) {
                            p->instructions[p->pc]->execute(*p);
                        }
                        pcAfter = p->pc;
                    }
                }
                if (!found) {
                    std::cout << "No active process.\n";
                    continue;
                }
                std::cout << "Executed instruction " << pcAfter
                    << " for process " << procName << ".\n";
            }
            else if (cmd == "exit") {
                std::cout << "Exiting process screen...\n";
                mode = ConsoleMode::MAIN;
                current_process.clear();
            }
            else std::cout << "Invalid command in process screen.\n";
        }
    }
}
