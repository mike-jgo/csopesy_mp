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


// === Global variables ===
std::mutex io_mutex;
bool initialized = false;
Config systemConfig;
ConsoleMode mode = ConsoleMode::MAIN;
std::vector<Process> processTable; 
std::vector<CPUCore> cpuCores;
int nextPID = 1;
std::string current_process = "";
std::atomic<bool> autoCreateRunning{ false };
std::atomic<bool> schedulerRunning{ false };
std::thread schedulerThread;
unsigned long long global_tick = 0;


// === Forward declarations ===
void scheduler_loop_tick();

// === Utility functions ===
std::vector<std::string> tokenize(const std::string& input) {
    std::istringstream stream(input);
    std::vector<std::string> tokens;
    std::string token;
    while (stream >> token) tokens.push_back(token);
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
        else if (key == "scheduler") systemConfig.scheduler = value;
        else if (key == "quantum-cycles") systemConfig.quantum_cycles = std::stoi(value);
        else if (key == "batch-process-freq") systemConfig.batch_process_freq = std::stoi(value);
        else if (key == "min-ins") systemConfig.min_ins = std::stoi(value);
        else if (key == "max-ins") systemConfig.max_ins = std::stoi(value);
        else if (key == "delays-per-exec") systemConfig.delays_per_exec = std::stoi(value);
    }

    file.close();

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

    return true;
}


// === Process helpers ===
Process* findProcess(const std::string& name) {
    for (auto& p : processTable)
        if (p.name == name)
            return &p;
    return nullptr;
}

// Resolve a token to either an integer or a variable value
int resolveValue(Process& p, const std::string& token) {
    try {
        return std::stoi(token);
    }
    catch (...) {
       
    }

    if (!p.variables.count(token)) {
        p.variables[token] = 0;
    }
    return p.variables[token];
}

// Execute a single instruction for a process
void executeInstruction(Process& p) {
    if (p.state == ProcessState::FINISHED) return;
    if (p.pc >= p.instructions.size()) {
        p.state = ProcessState::FINISHED;
        return;
    }

    std::string instr = p.instructions[p.pc];

    // === DECLARE(var, value) ===
    if (instr.rfind("DECLARE", 0) == 0) {
        std::regex re(R"(DECLARE\((\w+),\s*(-?\d+)\))");
        std::smatch match;
        if (std::regex_match(instr, match, re)) {
            std::string var = match[1];
            int value = std::stoi(match[2]);
            p.variables[var] = value;
        }
    }

    // === ADD(var1, var2/value, var3/value) ===
    else if (instr.rfind("ADD", 0) == 0) {
        std::regex re(R"(ADD\((\w+),\s*([\w\-]+),\s*([\w\-]+)\))");
        std::smatch match;
        if (std::regex_match(instr, match, re)) {
            std::string target = match[1];
            std::string op1 = match[2];
            std::string op2 = match[3];

            int val1 = resolveValue(p, op1);
            int val2 = resolveValue(p, op2);
            p.variables[target] = val1 + val2;
        }
    }

    // === SUBTRACT(var1, var2/value, var3/value) ===
    else if (instr.rfind("SUBTRACT", 0) == 0) {
        std::regex re(R"(SUBTRACT\((\w+),\s*([\w\-]+),\s*([\w\-]+)\))");
        std::smatch match;
        if (std::regex_match(instr, match, re)) {
            std::string target = match[1];
            std::string op1 = match[2];
            std::string op2 = match[3];

            int val1 = resolveValue(p, op1);
            int val2 = resolveValue(p, op2);
            p.variables[target] = val1 - val2;
        }
    }

    // === PRINT('message') ===
    else if (instr.rfind("PRINT", 0) == 0) {
        std::regex re(R"(PRINT\('([^']+)'\))");
        std::smatch match;
        if (std::regex_match(instr, match, re)) {
            std::string message = match[1];
            p.logs.push_back(message);
        }
    }

    // === SLEEP(n) ===
    else if (instr.rfind("SLEEP", 0) == 0) {
        std::regex re(R"(SLEEP\((\d+)\))");
        std::smatch match;
        if (std::regex_match(instr, match, re)) {
            int n = std::stoi(match[1]);
            p.sleep_counter = n;
            p.state = ProcessState::SLEEPING;
        }
    }

    // === FOR([...], repeats) ===
    else if (instr.rfind("FOR", 0) == 0) {
        std::regex re(R"(FOR\(\[([^\]]+)\],\s*(\d+)\))");
        std::smatch match;
        if (std::regex_match(instr, match, re)) {
            std::string body = match[1];
            int repeats = std::stoi(match[2]);

            // Split the inner instruction list
            std::vector<std::string> innerInstrs;
            std::stringstream ss(body);
            std::string temp;
            while (std::getline(ss, temp, ';')) {
                std::string trimmed = std::regex_replace(temp, std::regex(R"(^\s+|\s+$)"), "");
                if (!trimmed.empty()) innerInstrs.push_back(trimmed);
            }

            // Expand the FOR loop into repeated instructions with markers
            std::vector<std::string> expanded;
            std::ostringstream beginMarker;
            beginMarker << "// BEGIN FOR (" << repeats << "x)";
            expanded.push_back(beginMarker.str());

            for (int i = 0; i < repeats; ++i) {
                for (const auto& inner : innerInstrs) {
                    std::ostringstream labeled;
                    labeled << "[FOR iter " << (i + 1) << "] " << inner;
                    expanded.push_back(labeled.str());
                }
            }

            expanded.push_back("// END FOR");

            // Replace the FOR instruction with the expanded body
            p.instructions.erase(p.instructions.begin() + p.pc);
            p.instructions.insert(p.instructions.begin() + p.pc, expanded.begin(), expanded.end());

            // Do NOT increment pc here — scheduler/step will handle it
            return;
        }
    }



    // Advance PC
    p.pc++;
    if (p.pc >= p.instructions.size()) {
        p.state = ProcessState::FINISHED;
    }
}

// Generate dummy instructions for a process 
std::vector<std::string> generateDummyInstructions(int count) {
    std::vector<std::string> ins;
    static std::vector<std::string> pool = {
        "DECLARE(x, 5)",
        "DECLARE(y, 10)",
        "ADD(sum, x, y)",
        "SUBTRACT(diff, y, x)",
        "PRINT('Hello world!')",
        "PRINT('Value of sum: ' + sum)",
        "SLEEP(2)",
        "FOR([DECLARE(i,1); ADD(sum, sum, 1); PRINT('Loop iteration ' + sum)], 3)"
    };
    for (int i = 0; i < count; ++i)
        ins.push_back(pool[rand() % pool.size()]);
    return ins;
}

// Ensure the scheduler thread is running
void ensureSchedulerActive() {
    if (!schedulerRunning.load() && initialized) {
        schedulerRunning.store(true);
        schedulerThread = std::thread([]() {
            while (schedulerRunning.load()) {

                bool anyActive = std::any_of(processTable.begin(), processTable.end(),
                    [](const Process& p) {
                        return p.state == ProcessState::READY ||
                            p.state == ProcessState::RUNNING ||
                            p.state == ProcessState::SLEEPING;
                    });

                if (anyActive || autoCreateRunning.load()) {
                    scheduler_loop_tick(); // always tick sleeping processes
                }
                else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }

                // Stop only if truly finished
                bool allFinished = !processTable.empty() &&
                    std::all_of(processTable.begin(), processTable.end(),
                        [](const Process& p) { return p.state == ProcessState::FINISHED; });

                if (allFinished && !autoCreateRunning.load()) {
                    schedulerRunning.store(false);
                    std::cout << "[Tick " << global_tick
                        << "] Scheduler halted (all processes finished).\n";
                }
            }
            });
        schedulerThread.detach();
        std::cout << "Scheduler thread started.\n";
    }
}

// === COMMANDS ===
// Some commands are just couts simulating behavior
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
            << "  screen -s <process_name>\n"
            << "  screen -r <process_name>\n"
            << "  screen -ls\n";
        return;
    }
    std::string flag = args[1];

    // --- Create new process ---
    if (flag == "-s" && args.size() >= 3) {
        std::string name = args[2];
        if (findProcess(name)) {
            std::cout << "Process " << name << " already exists.\n";
            return;
        }
        Process newProc;
        newProc.name = name;
        newProc.pid = nextPID++;
        newProc.state = ProcessState::READY;
        int insCount = rand() % (systemConfig.max_ins - systemConfig.min_ins + 1) + systemConfig.min_ins;
        newProc.instructions = generateDummyInstructions(insCount);

        processTable.push_back(newProc);

        std::cout << "Created new process: " << name << " (PID " << newProc.pid << ")\n";
        std::cout << "Attached to process screen.\n";
        ensureSchedulerActive();

        mode = ConsoleMode::PROCESS;
        current_process = name;
    }
    // --- Reattach to an existing process ---
    else if (flag == "-r" && args.size() >= 3) {
        std::string name = args[2];
        Process* p = findProcess(name);
        if (!p) {
            std::cout << "Process " << name << " not found.\n";
            return;
        }

        if (p->state == ProcessState::FINISHED) {
            std::cout << "Process " << name << " already finished.\n";
            return;
        }

        std::cout << "Reattached to process " << name << " (PID " << p->pid << ")\n";
        mode = ConsoleMode::PROCESS;
        current_process = name;
    }

    // --- List all processes ---
    else if (flag == "-ls") {
        if (processTable.empty()) {
            std::cout << "No processes created.\n";
            return;
        }

        int totalCores = systemConfig.num_cpu;
        int runningCount = 0, finishedCount = 0, readyCount = 0, sleepingCount = 0;

        for (const auto& p : processTable) {
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
        for (const auto& p : processTable) {
            std::string stateStr;
            switch (p.state) {
            case ProcessState::READY: stateStr = "READY"; break;
            case ProcessState::RUNNING: stateStr = "RUNNING"; break;
            case ProcessState::SLEEPING: stateStr = "SLEEPING"; break;
            case ProcessState::FINISHED: stateStr = "FINISHED"; break;
            }
            std::cout << "  " << p.name << " [PID " << p.pid << "] - "
                << stateStr << " (" << p.pc << "/" << p.instructions.size() << ")\n";
        }
        std::cout << "=====================\n\n";
    }
}

// Trace function
void logInstructionTrace(Process& p, const std::string& instr) {
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
    trace << "[" << timestamp.str() << "] "
        << "[Tick " << global_tick << " | Q" << (p.pc % systemConfig.quantum_cycles + 1)
        << "/" << systemConfig.quantum_cycles << "] "
        << p.name << " [PID " << p.pid << "] pc="
        << p.pc << "/" << p.instructions.size()
        << " -> " << instr
        << " | State=" << stateStr << "\n";
    trace.close();
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

        std::cout << "Auto-creation started — new process every "
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

        // Do NOT kill the scheduler thread here it should keep ticking sleepers
        // and will auto-halt when all processes reach FINISHED.
        bool allFinished = !processTable.empty() &&
            std::all_of(processTable.begin(), processTable.end(),
                [](const Process& p) { return p.state == ProcessState::FINISHED; });

        if (allFinished) {
            schedulerRunning.store(false);
            std::cout << "All processes finished — scheduler halted.\n";
        }
    }
    else {
        std::cout << "Invalid command. Use 'scheduler start' or 'scheduler stop'.\n";
    }
}


// scheduler-start
// === Multi-core round robin scheduler ===
void scheduler_loop_tick() {
    global_tick++;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
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
            // Pick next ready process
            auto it = std::find_if(processTable.begin(), processTable.end(),
                [](Process& p) { return p.state == ProcessState::READY; });
            if (it != processTable.end()) {
                core.running = &(*it);
                it->state = ProcessState::RUNNING;
                core.quantum_left = systemConfig.quantum_cycles;
            }
            else {
                core.running = nullptr;
            }
        }
    }

    // === 3. Execute processes on each core ===
    for (auto& core : cpuCores) {
        if (core.running && core.running->state == ProcessState::RUNNING) {
            Process* p = core.running;

            // Execute one instruction = one tick
            if (p->pc < p->instructions.size()) {
                logInstructionTrace(*p, p->instructions[p->pc]);
                executeInstruction(*p);
                core.quantum_left--;

                // Handle post-execution logic
                if (p->state == ProcessState::FINISHED) {
                    core.running = nullptr;
                }
                else if (p->state == ProcessState::SLEEPING) {
                    core.running = nullptr;
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
                    }
                    else {
                        // No other ready — keep executing
                        core.quantum_left = systemConfig.quantum_cycles;
                    }
                }
            }
        }
    }

    // === 4. Auto-create processes if enabled ===
    if (autoCreateRunning.load() &&
        systemConfig.batch_process_freq > 0 &&
        global_tick % systemConfig.batch_process_freq == 0) {
    
        // Only create one process per batch frequency, not potentially multiple
        static unsigned long long lastCreationTick = 0;
    
        if (global_tick != lastCreationTick) {
            Process newProc;
            newProc.name = "auto_p" + std::to_string(nextPID++);
            newProc.pid = nextPID - 1;
            newProc.state = ProcessState::READY;
            int insCount = rand() % (systemConfig.max_ins - systemConfig.min_ins + 1) + systemConfig.min_ins;
            newProc.instructions = generateDummyInstructions(insCount);
            processTable.push_back(newProc);
        
            lastCreationTick = global_tick;
        }
    }
}

// report-util (simulated)
void reportUtilCommand() {
    if (!initialized) {
        std::cout << "Error: System not initialized.\n";
        return;
    }
    std::cout << "Generating CPU utilization report (simulated)...\n";
    std::cout << "CPU Utilization: 83%\nCores Used: 3/4\nFinished Processes: 5\n";
    std::cout << "Saved to csopesy-log.txt\n";
}

// process-smi inside process screen
void processSmiCommand() {
    Process* proc = findProcess(current_process);
    if (!proc) {
        std::cout << "Error: Process " << current_process << " not found.\n";
        return;
    }

    std::cout << "\n=== Process SMI ===\n";
    std::cout << "Name: " << proc->name << "\n";
    std::cout << "PID: " << proc->pid << "\n";

    // Translate enum to string
    std::string stateStr;
    switch (proc->state) {
    case ProcessState::READY: stateStr = "READY"; break;
    case ProcessState::RUNNING: stateStr = "RUNNING"; break;
    case ProcessState::SLEEPING: stateStr = "SLEEPING"; break;
    case ProcessState::FINISHED: stateStr = "FINISHED"; break;
    }
    std::cout << "State: " << stateStr << "\n";

    // Instruction progress
    std::cout << "Instruction progress: " << proc->pc << " / " << proc->instructions.size() << "\n";

    // Display variables
    if (!proc->variables.empty()) {
        std::cout << "Variables:\n";
        for (const auto& [var, val] : proc->variables)
            std::cout << "  " << var << " = " << val << "\n";
    }
    else {
        std::cout << "Variables: (none)\n";
    }

    // Display logs
    if (!proc->logs.empty()) {
        std::cout << "Logs:\n";
        for (const auto& log : proc->logs)
            std::cout << "  " << log << "\n";
    }
    else {
        std::cout << "Logs: (none)\n";
    }

    // === Display Instructions ===
    if (!proc->instructions.empty()) {
        std::cout << "\nInstructions:\n";
        for (size_t i = 0; i < proc->instructions.size(); ++i) {
            std::cout << "  [" << std::setw(2) << i << "] ";

            // Highlight the current instruction
            if (i == proc->pc && proc->state != ProcessState::FINISHED)
                std::cout << ">> " << proc->instructions[i] << "  <-- current\n";
            else
                std::cout << proc->instructions[i] << "\n";
        }
    }
    else {
        std::cout << "\nInstructions: (none)\n";
    }

    // Finished message
    if (proc->state == ProcessState::FINISHED)
        std::cout << "Process has finished execution.\n";

    std::cout << "=====================\n\n";
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
                Process* p = findProcess(current_process);
                if (!p) {
                    std::cout << "No active process.\n";
                    continue;
                }
                executeInstruction(*p);
                std::cout << "Executed instruction " << p->pc
                    << " for process " << p->name << ".\n";
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
