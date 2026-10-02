// =====================================================================
//  Worklet 1 : Assembler for an Abstract Machine  (x86-32)
//  ---------------------------------------------------------------------
//  Pipeline:
//      .asm source
//        -> Pooja  : tokenizeLine()        Lexer
//        -> Om     : parseInstruction()    Parser + Instruction IR
//        -> Ved    : pass1()               Location Counter + Symbol Table
//        -> Mayuri : pass2Resolve()        Forward/backward reference
//                                          resolution + error handling
//        -> Worklet1Output (in memory)
//        -> worklet2_receive() / worklet3_receive()   (function-call handoff)
//
//  HANDOFF DESIGN:
//      Worklet 1 does NOT write any output files. runWorklet1() returns a
//      Worklet1Output struct and Worklet 2 / Worklet 3 are called with that
//      struct directly. No disk I/O, no re-tokenising, no re-parsing.
//
//  Worklet 1 does NOT encode machine code and does NOT build an ELF file.
//
//  Supported instructions (32-bit registers only):
//      MOV/ADD/SUB/CMP  reg, reg|imm
//      INC/DEC/PUSH/POP reg
//      JMP/CALL/JE/JNE  label      (always the NEAR form: rel32)
//      NOP/RET
//  Labels:   name:
//  Comments: ; to end of line
//
//  Build : g++ -std=c++17 -O2 -Wall -Wextra -o worklet1 worklet1.cpp
//  Run   : ./worklet1 test.asm
// =====================================================================

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <unordered_map>
#include <cctype>

using namespace std;

// ---------------------------------------------------------------------
// DATA STRUCTURES (shared contract with Worklet 2 and Worklet 3)
// ---------------------------------------------------------------------

// Token types. DIRECTIVE can be added here later.
enum class TokenType {
    MNEMONIC,    // MOV, ADD, ...
    REGISTER,    // EAX, EBX, ...
    IMMEDIATE,   // 5, -3, 0x1000
    IDENTIFIER,  // a name used as an operand (label reference)
    LABEL,       // name followed by ':'
    COMMA
};

struct Token {
    TokenType type;
    string    text;
    int       line;
    int       column;   // 1-based
};

struct Operand {
    string       kind;             // "REG", "IMM" or "SYM"
    string       value;            // text as written / normalised
    int          column;           // where it starts in the source line
    long long    number;           // numeric value (IMM only)
    bool         resolved;         // SYM only: set by Pass 2
    unsigned int resolvedAddress;  // SYM only: address of the label
};

struct Instruction {
    string          mnemonic;
    vector<Operand> operands;
    unsigned int    address;       // set by Pass 1
    int             length;        // set by Pass 1 (bytes)
    int             line;
    int             column;
    int             displacement;  // branches only: target - (address+length)
};

// A label found by the parser. instrIndex = index of the instruction that
// follows it (so Pass 1 knows which address the label gets).
struct LabelDef {
    string name;
    int    instrIndex;
    int    line;
    int    column;
};

struct Symbol {
    string       name;
    unsigned int address;
    int          line;      // where it was defined (for duplicate errors)
};

struct AsmError {
    string category;   // "Lexical Error", "Syntax Error", "Symbol Error"
    string message;
    int    line;
    int    column;
};

// Everything Worklet 1 hands over to the next worklets.
struct Worklet1Output {
    vector<Instruction>           instructions;
    unordered_map<string, Symbol> symbolTable;
    vector<AsmError>              errors;
    unsigned int                  finalLC;   // total code size in bytes
    bool                          ok;        // true if no errors at all
};

// ---------------------------------------------------------------------
// SMALL HELPERS
// ---------------------------------------------------------------------

void addError(vector<AsmError>& errors, const string& category,
              const string& message, int line, int column) {
    AsmError e;
    e.category = category;
    e.message  = message;
    e.line     = line;
    e.column   = column;
    errors.push_back(e);
}

string toUpper(string s) {
    for (size_t i = 0; i < s.size(); i++) s[i] = (char)toupper((unsigned char)s[i]);
    return s;
}

// How many operands each mnemonic takes. -1 means "not a valid mnemonic".
// To support a new instruction, add it here first.
int expectedOperandCount(const string& m) {
    if (m == "MOV" || m == "ADD" || m == "SUB" || m == "CMP") return 2;
    if (m == "LOAD" || m == "STORE") return 2;
    if (m == "INC" || m == "DEC" || m == "PUSH" || m == "POP") return 1;
    if (m == "JMP" || m == "CALL" || m == "JE" || m == "JNE") return 1;
    if (m == "NOP" || m == "RET") return 0;
    return -1;
}

bool isValidMnemonic(const string& upper) {
    return expectedOperandCount(upper) != -1;
}

bool isValidRegister(const string& upper) {
    return upper == "EAX" || upper == "EBX" || upper == "ECX" || upper == "EDX" ||
           upper == "ESI" || upper == "EDI" || upper == "ESP" || upper == "EBP";
}

// Reads decimal (5, -3) or hex (0x1F) numbers that fit in 32 bits.
// Returns false for anything else. Never throws.
bool parseNumber(const string& text, long long& value) {
    size_t i = 0;
    bool negative = false;
    if (i < text.size() && text[i] == '-') { negative = true; i++; }
    if (i >= text.size()) return false;

    int base = 10;
    if (i + 1 < text.size() && text[i] == '0' && (text[i + 1] == 'x' || text[i + 1] == 'X')) {
        base = 16;
        i += 2;
        if (i >= text.size()) return false;
    }

    long long result = 0;
    for (; i < text.size(); i++) {
        char c = text[i];
        int digit;
        if (c >= '0' && c <= '9')                 digit = c - '0';
        else if (base == 16 && c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (base == 16 && c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        else return false;

        result = result * base + digit;
        if (result > 4294967295LL) return false;   // more than 32 bits
    }
    if (negative) {
        result = -result;
        if (result < -2147483648LL) return false;
    }
    value = result;
    return true;
}

Operand makeOperand(const string& kind, const string& value, int column) {
    Operand op;
    op.kind            = kind;
    op.value           = value;
    op.column          = column;
    op.number          = 0;
    op.resolved        = false;
    op.resolvedAddress = 0;
    return op;
}

// =====================================================================
//  POOJA : Lexer / Tokenizer
//  Turns ONE source line into tokens. Every token remembers its line and
//  column. Bad characters / bad numbers are reported as errors; the lexer
//  never crashes and never reads outside the string.
// =====================================================================
vector<Token> tokenizeLine(const string& line, int lineNumber, vector<AsmError>& errors) {
    vector<Token> tokens;
    size_t i = 0;

    while (i < line.size()) {
        char c = line[i];
        int column = (int)i + 1;

        // skip white space
        if (isspace((unsigned char)c)) { i++; continue; }

        // comment: ignore the rest of the line
        if (c == ';') break;

        // comma
        if (c == ',') {
            tokens.push_back({TokenType::COMMA, ",", lineNumber, column});
            i++;
            continue;
        }

        // number: starts with a digit, or '-' followed by a digit
        if (isdigit((unsigned char)c) ||
            (c == '-' && i + 1 < line.size() && isdigit((unsigned char)line[i + 1]))) {
            size_t start = i;
            i++;
            while (i < line.size() && (isalnum((unsigned char)line[i]) || line[i] == '_')) i++;
            string text = line.substr(start, i - start);

            long long ignored;
            if (parseNumber(text, ignored)) {
                tokens.push_back({TokenType::IMMEDIATE, text, lineNumber, column});
            } else {
                addError(errors, "Lexical Error", "invalid immediate '" + text + "'",
                         lineNumber, column);
            }
            continue;
        }

        // word: mnemonic, register, label or identifier
        if (isalpha((unsigned char)c) || c == '_') {
            size_t start = i;
            while (i < line.size() && (isalnum((unsigned char)line[i]) || line[i] == '_')) i++;
            string word  = line.substr(start, i - start);
            string upper = toUpper(word);

            bool followedByColon = (i < line.size() && line[i] == ':');

            if (followedByColon) {
                i++;   // consume ':'
                if (isValidMnemonic(upper) || isValidRegister(upper)) {
                    addError(errors, "Lexical Error",
                             "label name '" + word + "' is a reserved word", lineNumber, column);
                } else {
                    tokens.push_back({TokenType::LABEL, word, lineNumber, column});
                }
            } else if (isValidMnemonic(upper)) {
                tokens.push_back({TokenType::MNEMONIC, upper, lineNumber, column});
            } else if (isValidRegister(upper)) {
                tokens.push_back({TokenType::REGISTER, upper, lineNumber, column});
            } else {
                tokens.push_back({TokenType::IDENTIFIER, word, lineNumber, column});
            }
            continue;
        }

        // anything else is not part of our language
        addError(errors, "Lexical Error", string("invalid character '") + c + "'",
                 lineNumber, column);
        i++;
    }
    return tokens;
}

// =====================================================================
//  OM : Parser  (tokens -> Instruction IR)
//  Grammar of one line:   [LABEL] [MNEMONIC [operand {, operand}]]
// =====================================================================

// Checks that the operand KINDS are legal for this mnemonic.
bool checkOperandKinds(const Instruction& instr, vector<AsmError>& errors) {
    const string& m = instr.mnemonic;
    bool ok = true;

    if (m == "MOV" || m == "ADD" || m == "SUB" || m == "CMP") {
        const Operand& a = instr.operands[0];
        const Operand& b = instr.operands[1];
        if (a.kind != "REG") {
            string msg = (a.kind == "SYM") ? "invalid register '" + a.value + "'"
                                           : "first operand must be a register";
            addError(errors, "Syntax Error", msg, instr.line, a.column);
            ok = false;
        }
        if (b.kind == "SYM") {
            addError(errors, "Syntax Error",
                     "invalid register '" + b.value + "' (expected register or immediate)",
                     instr.line, b.column);
            ok = false;
        }
    }
    else if (m == "LOAD") {
        const Operand& a = instr.operands[0];
        const Operand& b = instr.operands[1];
        if (a.kind != "REG") {
            addError(errors, "Syntax Error", "first operand of LOAD must be a register", instr.line, a.column);
            ok = false;
        }
        if (b.kind == "IMM") {
            addError(errors, "Syntax Error", "second operand of LOAD cannot be an immediate (use MOV)", instr.line, b.column);
            ok = false;
        }
    }
    else if (m == "STORE") {
        const Operand& a = instr.operands[0];
        const Operand& b = instr.operands[1];
        if (a.kind == "IMM") {
            addError(errors, "Syntax Error", "first operand of STORE cannot be an immediate", instr.line, a.column);
            ok = false;
        }
        if (b.kind != "REG") {
            addError(errors, "Syntax Error", "second operand of STORE must be a register", instr.line, b.column);
            ok = false;
        }
    }
    else if (m == "INC" || m == "DEC" || m == "PUSH" || m == "POP") {
        const Operand& a = instr.operands[0];
        if (a.kind != "REG") {
            string msg = (a.kind == "SYM") ? "invalid register '" + a.value + "'"
                                           : "operand must be a register";
            addError(errors, "Syntax Error", msg, instr.line, a.column);
            ok = false;
        }
    }
    else if (m == "JMP" || m == "CALL" || m == "JE" || m == "JNE") {
        const Operand& a = instr.operands[0];
        if (a.kind != "SYM") {
            addError(errors, "Syntax Error", "expected a label as branch target",
                     instr.line, a.column);
            ok = false;
        }
    }
    return ok;
}

// Parses tokens[start..end] as one instruction. Returns false on error
// (the error is already recorded).
bool parseInstruction(const vector<Token>& tokens, size_t start,
                      Instruction& instr, vector<AsmError>& errors) {
    const Token& first = tokens[start];
    instr.line         = first.line;
    instr.column       = first.column;
    instr.address      = 0;
    instr.length       = 0;
    instr.displacement = 0;

    // 1. first token must be a valid mnemonic
    if (first.type != TokenType::MNEMONIC) {
        if (first.type == TokenType::IDENTIFIER)
            addError(errors, "Syntax Error", "invalid mnemonic '" + first.text + "'",
                     first.line, first.column);
        else
            addError(errors, "Syntax Error", "expected a mnemonic, found '" + first.text + "'",
                     first.line, first.column);
        return false;
    }
    instr.mnemonic = first.text;

    // 2. read operands:  operand {, operand}
    size_t idx = start + 1;
    while (idx < tokens.size()) {
        const Token& t = tokens[idx];
        Operand op;

        if (t.type == TokenType::REGISTER) {
            op = makeOperand("REG", t.text, t.column);
        } else if (t.type == TokenType::IMMEDIATE) {
            op = makeOperand("IMM", t.text, t.column);
            parseNumber(t.text, op.number);   // already validated by the lexer
        } else if (t.type == TokenType::IDENTIFIER) {
            op = makeOperand("SYM", t.text, t.column);
        } else {
            addError(errors, "Syntax Error", "expected an operand, found '" + t.text + "'",
                     t.line, t.column);
            return false;
        }
        instr.operands.push_back(op);
        idx++;

        if (idx >= tokens.size()) break;               // end of line: done

        if (tokens[idx].type != TokenType::COMMA) {    // operands must be separated by ','
            addError(errors, "Syntax Error", "expected comma",
                     tokens[idx].line, tokens[idx].column);
            return false;
        }
        idx++;                                         // skip the comma

        if (idx >= tokens.size()) {                    // line ended right after a comma
            const Token& comma = tokens[idx - 1];
            addError(errors, "Syntax Error", "missing operand after comma",
                     comma.line, comma.column + 1);
            return false;
        }
    }

    // 3. operand COUNT must match the mnemonic
    int expected = expectedOperandCount(instr.mnemonic);
    int actual   = (int)instr.operands.size();

    if (actual < expected) {
        const Token& last = tokens.back();
        addError(errors, "Syntax Error",
                 "missing operand: " + instr.mnemonic + " expects " + to_string(expected) +
                 " operand(s), found " + to_string(actual),
                 last.line, last.column + (int)last.text.size());
        return false;
    }
    if (actual > expected) {
        addError(errors, "Syntax Error",
                 "too many operands: " + instr.mnemonic + " expects " + to_string(expected) +
                 " operand(s), found " + to_string(actual),
                 instr.line, instr.operands[expected].column);
        return false;
    }

    // 4. operand KINDS must be legal for the mnemonic
    return checkOperandKinds(instr, errors);
}

// Handles one whole line: optional label, then optional instruction.
void parseLine(const vector<Token>& tokens, vector<Instruction>& instructions,
               vector<LabelDef>& labels, vector<AsmError>& errors) {
    if (tokens.empty()) return;

    size_t idx = 0;
    if (tokens[0].type == TokenType::LABEL) {
        LabelDef label;
        label.name       = tokens[0].text;
        label.instrIndex = (int)instructions.size();   // the next instruction
        label.line       = tokens[0].line;
        label.column     = tokens[0].column;
        labels.push_back(label);
        idx = 1;
    }
    if (idx >= tokens.size()) return;                  // label-only line

    Instruction instr;
    if (parseInstruction(tokens, idx, instr, errors)) {
        instructions.push_back(instr);
    }
}
// =====================================================================
//  VED : Symbol Table
// =====================================================================
bool isDefined(const unordered_map<string, Symbol>& table, const string& name) {
    return table.find(name) != table.end();
}
