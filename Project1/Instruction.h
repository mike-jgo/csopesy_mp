#pragma once
#include <string>
#include <vector>
#include <memory>

// Forward declaration
class Process;

// === Instruction Interface ===
class Instruction {
public:
    virtual ~Instruction() = default;
    virtual void execute(Process& p) = 0;
    virtual std::string toString() const = 0;
};

// === Instruction Subclasses ===

class DeclareInstruction : public Instruction {
    std::string var;
    int val;
public:
    DeclareInstruction(const std::string& v, int value);
    void execute(Process& p) override;
    std::string toString() const override;
};

class AddInstruction : public Instruction {
    std::string target, op1, op2;
public:
    AddInstruction(const std::string& t, const std::string& o1, const std::string& o2);
    void execute(Process& p) override;
    std::string toString() const override;
};

class SubtractInstruction : public Instruction {
    std::string target, op1, op2;
public:
    SubtractInstruction(const std::string& t, const std::string& o1, const std::string& o2);
    void execute(Process& p) override;
    std::string toString() const override;
};

class PrintInstruction : public Instruction {
    std::string expression;
public:
    PrintInstruction(const std::string& expr);
    void execute(Process& p) override;
    std::string toString() const override;
};

class SleepInstruction : public Instruction {
    int duration;
public:
    SleepInstruction(int d);
    void execute(Process& p) override;
    std::string toString() const override;
};

class ForInstruction : public Instruction {
    std::string body;
    int repeats;
public:
    ForInstruction(const std::string& b, int r);
    void execute(Process& p) override;
    std::string toString() const override;
};

class WriteInstruction : public Instruction {
    std::string addrStr;
    std::string valStr;
public:
    WriteInstruction(const std::string& a, const std::string& v);
    void execute(Process& p) override;
    std::string toString() const override;
};

class ReadInstruction : public Instruction {
    std::string addrStr;
    std::string var;
public:
    ReadInstruction(const std::string& a, const std::string& v);
    void execute(Process& p) override;
    std::string toString() const override;
};

// === Parsing Function ===
std::shared_ptr<Instruction> parseInstruction(const std::string& line);
