#pragma once
#include "globals.h"
#include <string>
#include <memory>
#include <regex>
#include <iostream>
#include <sstream>
#include <algorithm>
#include <climits>

// === Base Instruction ===
class Instruction {
public:
    virtual ~Instruction() = default;
    virtual void execute(Process& p) = 0;
    virtual std::string toString() const = 0;
};

// === Helper ===
inline uint16_t clampUint16(int value) {
    return static_cast<uint16_t>(std::clamp(value, 0, 65535));
}

inline int resolveValue(Process& p, const std::string& token) {
    try { return std::stoi(token); }
    catch (...) {}
    if (!p.variables.count(token)) p.variables[token] = 0;
    return p.variables[token];
}

// === DECLARE ===
class DeclareInstruction : public Instruction {
    std::string var;
    int value;
public:
    DeclareInstruction(std::string v, int val) : var(std::move(v)), value(val) {}
    void execute(Process& p) override {
        p.variables[var] = clampUint16(value);
    }
    std::string toString() const override {
        return "DECLARE(" + var + ", " + std::to_string(value) + ")";
    }
};

// === ADD ===
class AddInstruction : public Instruction {
    std::string target, op1, op2;
public:
    AddInstruction(std::string t, std::string a, std::string b)
        : target(std::move(t)), op1(std::move(a)), op2(std::move(b)) {
    }
    void execute(Process& p) override {
        int val = resolveValue(p, op1) + resolveValue(p, op2);
        p.variables[target] = clampUint16(val);
    }
    std::string toString() const override {
        return "ADD(" + target + ", " + op1 + ", " + op2 + ")";
    }
};

// === SUBTRACT ===
class SubtractInstruction : public Instruction {
    std::string target, op1, op2;
public:
    SubtractInstruction(std::string t, std::string a, std::string b)
        : target(std::move(t)), op1(std::move(a)), op2(std::move(b)) {
    }
    void execute(Process& p) override {
        int val = resolveValue(p, op1) - resolveValue(p, op2);
        p.variables[target] = clampUint16(val);
    }
    std::string toString() const override {
        return "SUBTRACT(" + target + ", " + op1 + ", " + op2 + ")";
    }
};

// === PRINT ===
class PrintInstruction : public Instruction {
    std::string message;
public:
    explicit PrintInstruction(std::string msg) : message(std::move(msg)) {}
    void execute(Process& p) override {
        std::string output = message;

        // Support +var concatenation like PRINT("Value: " +x)
        size_t pos = output.find('+');
        if (pos != std::string::npos) {
            std::string prefix = output.substr(0, pos);
            std::string var = output.substr(pos + 1);
            if (p.variables.count(var))
                output = prefix + std::to_string(p.variables[var]);
            else
                output = prefix + "<undefined>";
        }

        p.logs.push_back(output);
        if (current_process == p.name)
            std::cout << output << "\n";
    }
    std::string toString() const override {
        return "PRINT('" + message + "')";
    }
};

// === SLEEP ===
class SleepInstruction : public Instruction {
    int ticks;
public:
    explicit SleepInstruction(int t) : ticks(t) {}
    void execute(Process& p) override {
        p.sleep_counter = ticks;
        p.state = ProcessState::SLEEPING;
    }
    std::string toString() const override {
        return "SLEEP(" + std::to_string(ticks) + ")";
    }
};

// === FOR LOOP ===
class ForInstruction : public Instruction {
    std::vector<std::unique_ptr<Instruction>> body;
    int repeats;
    int current_repeat = 0;
public:
    ForInstruction(std::vector<std::unique_ptr<Instruction>> b, int r)
        : repeats(r) {
        for (auto& instr : b)
            body.push_back(std::move(instr));
    }

    void execute(Process& p) override {
        if (current_repeat < repeats) {
            for (auto& instr : body) instr->execute(p);
            current_repeat++;
            if (current_repeat < repeats)
                p.pc--; // repeat next cycle
        }
    }

    std::string toString() const override {
        return "FOR([" + std::to_string(body.size()) + " instructions], " + std::to_string(repeats) + ")";
    }
};

// === Factory ===
inline std::unique_ptr<Instruction> parseInstruction(const std::string& instr) {
    std::smatch m;

    if (std::regex_match(instr, m, std::regex(R"(DECLARE\((\w+),\s*(\d+)\))")))
        return std::make_unique<DeclareInstruction>(m[1], std::stoi(m[2]));

    if (std::regex_match(instr, m, std::regex(R"(ADD\((\w+),\s*([\w\-]+),\s*([\w\-]+)\))")))
        return std::make_unique<AddInstruction>(m[1], m[2], m[3]);

    if (std::regex_match(instr, m, std::regex(R"(SUBTRACT\((\w+),\s*([\w\-]+),\s*([\w\-]+)\))")))
        return std::make_unique<SubtractInstruction>(m[1], m[2], m[3]);

    if (std::regex_match(instr, m, std::regex(R"(PRINT\(['"]([^'"]+)['"]\))")))
        return std::make_unique<PrintInstruction>(m[1]);

    if (std::regex_match(instr, m, std::regex(R"(SLEEP\((\d+)\))")))
        return std::make_unique<SleepInstruction>(std::stoi(m[1]));

    return nullptr;
}
