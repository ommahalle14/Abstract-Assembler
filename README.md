# 🛠️ Abstract Assembler — Worklet 1

> A multi-pass assembler for an abstract x86-32 machine, built as a team project.  
> Converts `.asm` source files into a validated Intermediate Representation (IR) handed off to downstream worklets for encoding and ELF generation.

---

## 📐 Architecture Overview

```
.asm source file
     │
     ▼
┌─────────────┐
│   Lexer     │  tokenizeLine()      — Pooja
│  (Pass 0)   │  Turns each line into typed tokens
└──────┬──────┘
       │
       ▼
┌─────────────┐
│   Parser    │  parseInstruction()  — Om
│  (Pass 0)   │  Builds Instruction IR from token stream
└──────┬──────┘
       │
       ▼
┌─────────────┐
│   Pass 1    │  pass1()             — Ved
│             │  Location Counter + Symbol Table
└──────┬──────┘
       │
       ▼
┌─────────────┐
│   Pass 2    │  pass2Resolve()      — Mayuri
│             │  Resolves forward & backward label refs
└──────┬──────┘
       │
       ▼
  Worklet1Output  (in-memory struct — no files written)
       │
       ├──► worklet2_receive()   [Encoding: ModR/M, immediates, …]
       └──► worklet3_receive()   [ELF: symbol table + relocations]
```

---

## 🧩 Team Contributions

| Member | Component | Functions |
|--------|-----------|-----------|
| **Pooja** | Lexer / Tokenizer | `tokenizeLine()` |
| **Om** | Parser | `parseInstruction()`, `parseLine()`, `checkOperandKinds()` |
| **Ved** | Pass 1 — Symbol Table & Location Counter | `pass1()`, `isDefined()`, `addSymbol()`, `getInstructionLength()`, `defineLabel()` |
| **Mayuri** | Pass 2 — Reference Resolution | `pass2Resolve()` |
| **All** | Driver, Printer, Main | `runWorklet1()`, `printIR()`, `printSymbolTable()`, `main()` |

---

## 🔧 Supported Instructions

| Instruction | Operands | Encoding Size |
|-------------|----------|---------------|
| `MOV reg, imm` | reg ← immediate | 5 bytes |
| `MOV reg, reg` | reg ← reg | 2 bytes |
| `ADD / SUB / CMP reg, reg` | arithmetic | 2 bytes |
| `ADD / SUB / CMP reg, imm` | imm fits in 8 bits | 3 bytes |
| `ADD / SUB / CMP EAX, imm` | EAX short form | 5 bytes |
| `ADD / SUB / CMP reg, imm` | general | 6 bytes |
| `INC / DEC / PUSH / POP reg` | single register | 1 byte |
| `NOP / RET` | — | 1 byte |
| `JMP / CALL label` | near rel32 | 5 bytes |
| `JE / JNE label` | near 0F 8x rel32 | 6 bytes |
| `LOAD reg, reg/sym` | memory load | 2 or 6 bytes |
| `STORE reg/sym, reg` | memory store | 2 or 6 bytes |

> **Note:** All branches use the **near (rel32)** form — size is always fixed regardless of label distance.

---

## 🗂️ Data Structures

```cpp
// A single assembly token
struct Token { TokenType type; string text; int line, column; };

// An instruction operand (register, immediate, or symbol)
struct Operand { string kind, value; int column; long long number;
                 bool resolved; unsigned int resolvedAddress; };

// One fully-parsed instruction
struct Instruction { string mnemonic; vector<Operand> operands;
                     unsigned int address; int length, line, column, displacement; };

// A label definition collected during parsing
struct LabelDef  { string name; int instrIndex, line, column; };

// An entry in the symbol table
struct Symbol    { string name; unsigned int address; int line; };

// An error with location info
struct AsmError  { string category, message; int line, column; };

// Everything Worklet 1 hands to downstream worklets
struct Worklet1Output {
    vector<Instruction>           instructions;
    unordered_map<string, Symbol> symbolTable;
    vector<AsmError>              errors;
    unsigned int                  finalLC;   // total code size in bytes
    bool                          ok;
};
```

---

## 🚀 Build & Run

### Prerequisites
- `g++` with C++17 support (or later)

### Compile
```bash
g++ -std=c++17 -O2 -Wall -Wextra -o worklet1 worklet1.cpp
```

### Run
```bash
./worklet1 <your_file.asm>
```

### Example `.asm` input
```asm
; Simple loop example
    MOV EAX, 0
    MOV ECX, 10
loop:
    INC EAX
    DEC ECX
    JNE loop
    RET
```

### Example output
```
INTERMEDIATE REPRESENTATION
-------------------------
Instruction 1:
Mnemonic: MOV
Operand 1: EAX
Operand 2: 0
Address: 0x00
Length: 5
...

SYMBOL TABLE
-------------------------
loop -> 0x0a

Final Location Counter: 0x17 (23 bytes)
```

---

## 🐞 Error Handling

The assembler reports three categories of errors — all with precise **line and column** numbers:

| Category | Examples |
|----------|---------|
| `Lexical Error` | Invalid character, malformed immediate (`0x`) |
| `Syntax Error` | Wrong operand type, missing operand, unknown mnemonic |
| `Symbol Error` | Undefined label, duplicate label definition |

All errors are sorted by position (line → column) before printing. Processing continues past errors where possible so multiple issues are reported in a single run.

---

## 🔗 Handoff Design

Worklet 1 does **not** write any output files.  
`runWorklet1()` returns a `Worklet1Output` struct, and `worklet2_receive()` / `worklet3_receive()` are called directly with that struct — **no disk I/O, no re-parsing**.

---

## 📁 Repository Structure

```
Abstract-Assembler/
├── worklet1.cpp          # Full assembler source (Worklet 1)
├── worklet1_half2.cpp    # Second-half source (contributed by Om)
└── README.md             # This file
```

---

## 📜 License

This project was built for academic purposes as part of a Systems Programming course.
