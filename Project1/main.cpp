#include <iostream>
#include <string>
#include <thread>
#include <mutex>
#include <vector>
#include <unordered_map>
#include <sstream>
#include <fstream>
#include <stdexcept>

std::mutex io_mutex;

// CLI mode tracker
enum class ConsoleMode { MAIN, PROCESS };
enum class ProcessState { READY, RUNNING, SLEEPING, FINISHED };

ConsoleMode mode = ConsoleMode::MAIN;

// Configuration structure
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
Config systemConfig;

// Process structure
struct Process {
    std::string name;
    int pid;
    ProcessState state;
    std::vector<std::string> instructions;
    int pc = 0; 
    std::vector<std::string> logs; // collected outputs from PRINT
    std::unordered_map<std::string, int> variables; // current variable values
};


// Process table
std::vector<Process> processTable;
int nextPID = 1;

// Find process by name
Process* findProcess(const std::string& name) {
    for (auto& p : processTable) {
        if (p.name == name)
            return &p;
    }
    return nullptr;
}

// Generate dummy instructions
std::vector<std::string> generateDummyInstructions(int count) {
    std::vector<std::string> ins;
    static std::vector<std::string> pool = {
        "DECLARE(x, 5)", "ADD(y, x, 2)", "SUBTRACT(x, y, 1)",
        "PRINT('Hello world!')", "SLEEP(2)"
    };
    for (int i = 0; i < count; ++i)
        ins.push_back(pool[rand() % pool.size()]);
    return ins;
}

// Initialization flag
bool initialized = false;

// === CONFIG FILE HANDLING ===
bool generateDefaultConfig(const std::string& filename);
bool loadConfigFile(const std::string& filename);

// Simulated process screen state
std::string current_process = "";

// Simple helper: split string into tokens
std::vector<std::string> tokenize(const std::string& input) {
    std::istringstream stream(input);
    std::vector<std::string> tokens;
    std::string token;
    while (stream >> token) tokens.push_back(token);
    return tokens;
}

//Load config file and generate with default values if not found
bool loadConfigFile(const std::string& filename) {
    std::ifstream file(filename);

    if (!file.is_open()) {
        std::cout << "Warning: " << filename << " not found.\n";
        std::cout << "Creating default configuration file...\n";

        if (!generateDefaultConfig(filename)) {
            std::cout << "Error: Failed to create default config.\n";
            return false;
        }

        // Reopen the newly created file
        file.open(filename);
        if (!file.is_open()) {
            std::cout << "Error: Could not open new config file.\n";
            return false;
        }
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

    if (systemConfig.num_cpu <= 0 || systemConfig.scheduler.empty()) {
        std::cout << "Warning: Invalid or incomplete config. Regenerating default config.\n";
        generateDefaultConfig(filename);
        return loadConfigFile(filename);
    }

    systemConfig.loaded = true;
    return true;
}

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

// === COMMANDS ===
// All commands are just couts simulating behavior
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

        std::cout << "Process List:\n";
        for (const auto& p : processTable) {
            std::string stateStr;
            switch (p.state) {
            case ProcessState::READY: stateStr = "READY"; break;
            case ProcessState::RUNNING: stateStr = "RUNNING"; break;
            case ProcessState::SLEEPING: stateStr = "SLEEPING"; break;
            case ProcessState::FINISHED: stateStr = "FINISHED"; break;
            }
            std::cout << "  " << p.name << " [PID " << p.pid << "] - " << stateStr << "\n";
        }
    }

    else {
        std::cout << "Invalid screen command.\n";
    }
}

// scheduler-start
void schedulerStartCommand() {
    if (!initialized) {
        std::cout << "Error: System not initialized.\n";
        return;
    }
    std::cout << "Scheduler started (simulated). Generating dummy processes...\n";
}

// scheduler-stop
void schedulerStopCommand() {
    if (!initialized) {
        std::cout << "Error: System not initialized.\n";
        return;
    }
    std::cout << "Scheduler stopped.\n";
}

// report-util
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
        //Skip if the user entered only whitespace
        if (tokens.empty()) {
            continue;
        }
        std::string cmd = tokens[0];

        if (mode == ConsoleMode::MAIN) {
            if (cmd == "help") {
                std::cout << "Available commands:\n"
                    << "  initialize        - Load configuration\n"
                    << "  screen            - Create or manage processes\n"
                    << "  scheduler-start   - Begin dummy process generation\n"
                    << "  scheduler-stop    - Stop scheduler\n"
                    << "  report-util       - Generate CPU report\n"
                    << "  exit              - Quit program\n";
            }
            else if (cmd == "initialize") initializeCommand();
            else if (cmd == "screen") handleScreenCommand(tokens);
            else if (cmd == "scheduler-start") schedulerStartCommand();
            else if (cmd == "scheduler-stop") schedulerStopCommand();
            else if (cmd == "report-util") reportUtilCommand();
            else if (cmd == "exit") break;
            else std::cout << "Unknown command. Type 'help'.\n";
        }
        else if (mode == ConsoleMode::PROCESS) {
            if (cmd == "process-smi") processSmiCommand();
            else if (cmd == "exit") {
                std::cout << "Exiting process screen...\n";
                mode = ConsoleMode::MAIN;
                current_process.clear();
            }
            else std::cout << "Invalid command in process screen.\n";
        }
    }
}

// === MAIN ===
int main() {
    std::cout << "Welcome to CSOPESY Emulator CLI\n";
    std::cout << "Developers: Go, Michael Joseph | Go, Michael Anthony | Magaling, Zoe | Uy, Matthew\n";
    std::cout << "Version date: 11/1/25\n\n";
    inputLoop();
    std::cout << "Exiting CSOPESY Emulator...\n";
}
