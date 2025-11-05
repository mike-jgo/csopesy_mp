# 💻 [Group 2] MCO1 - CSOPESY Console Emulator

This repository contains a **console emulator and process multiplexer** written in C++ for CSOPESY. The program provides an interactive `CSOPESY>` shell that loads scheduling policies from `config.txt`, synthesizes batches of pseudo-processes, and lets you inspect or single-step them through a simulated CPU scheduler. It is designed for experimenting with round-robin and FCFS scheduling, quantum accounting, and lightweight process tracing inside a CLI environment.

---

## 👥 Group Members

- Go, Michael Joseph  
- Go, Michael Anthony  
- Magaling, Zoe  
- Uy, Matthew  

---

## 📐 Build & Run Instructions
### Option 1: Run in Visual Studio
1. **Open Visual Studio**.
2. On the start window, click **Clone a repository**.
3. Enter the GitHub repository URL and select a local path for the clone.
4. Once cloned, Visual Studio will open the solution/workspace automatically.
5. Configure project settings:
   - Right-click the project → **Properties**.
   - Navigate to:  
     **Configuration Properties > General > C++ Language Standard**  
     Set to: **ISO C++20 Standard (/std:c++20)**.
   - Click **Apply** then **OK**.
6. **Build the solution**:  
   Go to **Build > Build Solution** or press `Ctrl+Shift+B`.
7. **Run the program**:  
   Press **Ctrl+F5** (Start Without Debugging).  
   The console window will open with the `CSOPESY>` prompt.

---

## 🚪 Entry Point
- **Entry class/file:** `Project1/main.cpp`
- **Path from repository root:** `Project1/main.cpp`
- `int main()` seeds the runtime, prints CLI metadata, and jumps into `inputLoop()` from `Project1/emulator.cpp` to drive the console emulator.

---

## 🧭 Using the Emulator
- Run the program and type `help` to view supported commands.
- Use `initialize` to load `config.txt` (or auto-generate defaults) and set up CPU cores.
- `screen` enters the per-process screen where you can create processes and use `process-smi` for inspection.
- `scheduler start` / `scheduler stop` toggles automatic batch creation, while `report-util` shows system statistics and execution logs.

---

## 🗂️ MVC
```bash
csopesy_mp/
│
├── Project1/
│   └── (main.cpp)                         # This is where the main function is located
│   └── (Project1.vcxproj)
│   └── (Project1.vcxproj.filters)
│ 
└── README.md/                             # Project overview and setup instructions (This file)

