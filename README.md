# Abstract Assembler

This is our mini project for Systems Programming. It's a basic assembler written in C++ that takes a `.asm` file as input and processes it.

## What it does

- Tokenizes each line of the assembly source
- Parses instructions and builds an IR
- Builds a symbol table (labels and their addresses)
- Resolves forward and backward label references
- Passes everything to worklet 2 and worklet 3 by function call (no files written)

## How to compile

```
g++ -std=c++17 -O2 -o worklet1 worklet1.cpp
```

## How to run

```
./worklet1 test.asm
```

## Supported instructions

MOV, ADD, SUB, CMP, INC, DEC, PUSH, POP, JMP, CALL, JE, JNE, NOP, RET, LOAD, STORE

## Team

- Pooja – Lexer
- Om – Parser
- Ved – Pass 1 / Symbol Table
- Mayuri – Pass 2 / Reference Resolution
