/*
 * interp.c -- workload (iv): indirect dispatch.
 *
 * A small stack-based bytecode interpreter, run twice on the same program:
 *   1. switch dispatch: one `switch` on the opcode, which gcc compiles to a
 *      jump table, so every bytecode instruction runs one indirect jump;
 *   2. function-pointer dispatch: a table of handler functions, so every
 *      bytecode instruction runs one indirect call (and a return).
 * The bytecode program counts Collatz steps for 1..LIMIT, which mixes
 * loops, an if/else and arithmetic, so the sequence of opcodes (and hence
 * of indirect targets) varies in a data-dependent way.
 *
 * Usage: ./interp [limit]      (default 1500)
 */
#include <stdio.h>
#include <stdlib.h>

enum { PUSH, LOAD, STORE, ADD, MUL, DIV, MOD, LT, NE, JZ, JMP, HALT, NUM_OPS };

typedef struct {
    int op;
    long arg;
} Ins;

static Ins prog[128];
static int nprog;

static int emit(int op, long arg)
{
    prog[nprog].op = op;
    prog[nprog].arg = arg;
    return nprog++;
}

/* Bytecode for:
 *   total = 0
 *   for (n = 1; n < limit + 1; n++) {
 *       x = n; steps = 0
 *       while (x != 1) { if (x % 2 == 0) x = x / 2; else x = 3 * x + 1; steps++; }
 *       total += steps
 *   }
 * Variables: 0 = n, 1 = x, 2 = steps, 3 = total. */
static void build(long limit)
{
    emit(PUSH, 1); emit(STORE, 0);
    int outer = nprog;
    emit(LOAD, 0); emit(PUSH, limit + 1); emit(LT, 0);
    int to_end = emit(JZ, 0);
    emit(LOAD, 0); emit(STORE, 1); emit(PUSH, 0); emit(STORE, 2);
    int inner = nprog;
    emit(LOAD, 1); emit(PUSH, 1); emit(NE, 0);
    int to_done = emit(JZ, 0);
    emit(LOAD, 1); emit(PUSH, 2); emit(MOD, 0);
    int to_even = emit(JZ, 0);
    emit(LOAD, 1); emit(PUSH, 3); emit(MUL, 0); emit(PUSH, 1); emit(ADD, 0); emit(STORE, 1);
    int to_step = emit(JMP, 0);
    prog[to_even].arg = nprog;
    emit(LOAD, 1); emit(PUSH, 2); emit(DIV, 0); emit(STORE, 1);
    prog[to_step].arg = nprog;
    emit(LOAD, 2); emit(PUSH, 1); emit(ADD, 0); emit(STORE, 2);
    emit(JMP, inner);
    prog[to_done].arg = nprog;
    emit(LOAD, 3); emit(LOAD, 2); emit(ADD, 0); emit(STORE, 3);
    emit(LOAD, 0); emit(PUSH, 1); emit(ADD, 0); emit(STORE, 0);
    emit(JMP, outer);
    prog[to_end].arg = nprog;
    emit(HALT, 0);
}

/* ---- Interpreter 1: switch dispatch ---- */

__attribute__((noinline)) static long run_switch(void)
{
    long stack[32], vars[4] = {0, 0, 0, 0};
    int sp = 0, pc = 0;
    for (;;) {
        Ins in = prog[pc++];
        switch (in.op) {
        case PUSH:  stack[sp++] = in.arg; break;
        case LOAD:  stack[sp++] = vars[in.arg]; break;
        case STORE: vars[in.arg] = stack[--sp]; break;
        case ADD:   sp--; stack[sp - 1] += stack[sp]; break;
        case MUL:   sp--; stack[sp - 1] *= stack[sp]; break;
        case DIV:   sp--; stack[sp - 1] /= stack[sp]; break;
        case MOD:   sp--; stack[sp - 1] %= stack[sp]; break;
        case LT:    sp--; stack[sp - 1] = stack[sp - 1] < stack[sp]; break;
        case NE:    sp--; stack[sp - 1] = stack[sp - 1] != stack[sp]; break;
        case JZ:    if (stack[--sp] == 0) pc = (int)in.arg; break;
        case JMP:   pc = (int)in.arg; break;
        case HALT:  return vars[3];
        }
    }
}

/* ---- Interpreter 2: function-pointer dispatch ---- */

typedef struct {
    long stack[32], vars[4];
    int sp, pc, halted;
} VM;

typedef void (*Handler)(VM *, long);

static void op_push(VM *m, long a)  { m->stack[m->sp++] = a; }
static void op_load(VM *m, long a)  { m->stack[m->sp++] = m->vars[a]; }
static void op_store(VM *m, long a) { m->vars[a] = m->stack[--m->sp]; }
static void op_add(VM *m, long a)   { (void)a; m->sp--; m->stack[m->sp - 1] += m->stack[m->sp]; }
static void op_mul(VM *m, long a)   { (void)a; m->sp--; m->stack[m->sp - 1] *= m->stack[m->sp]; }
static void op_div(VM *m, long a)   { (void)a; m->sp--; m->stack[m->sp - 1] /= m->stack[m->sp]; }
static void op_mod(VM *m, long a)   { (void)a; m->sp--; m->stack[m->sp - 1] %= m->stack[m->sp]; }
static void op_lt(VM *m, long a)    { (void)a; m->sp--; m->stack[m->sp - 1] = m->stack[m->sp - 1] < m->stack[m->sp]; }
static void op_ne(VM *m, long a)    { (void)a; m->sp--; m->stack[m->sp - 1] = m->stack[m->sp - 1] != m->stack[m->sp]; }
static void op_jz(VM *m, long a)    { if (m->stack[--m->sp] == 0) m->pc = (int)a; }
static void op_jmp(VM *m, long a)   { m->pc = (int)a; }
static void op_halt(VM *m, long a)  { (void)a; m->halted = 1; }

static Handler handlers[NUM_OPS] = {
    op_push, op_load, op_store, op_add, op_mul, op_div,
    op_mod, op_lt, op_ne, op_jz, op_jmp, op_halt
};

__attribute__((noinline)) static long run_fnptr(void)
{
    VM m = {{0}, {0, 0, 0, 0}, 0, 0, 0};
    while (!m.halted) {
        Ins in = prog[m.pc++];
        handlers[in.op](&m, in.arg);
    }
    return m.vars[3];
}

int main(int argc, char **argv)
{
    long limit = argc > 1 ? atol(argv[1]) : 1500;
    if (limit < 1)
        limit = 1500;
    build(limit);
    long a = run_switch();
    long b = run_fnptr();
    printf("interp: limit=%ld program=%d ops switch=%ld fnptr=%ld\n", limit, nprog, a, b);
    return a == b ? 0 : 1;
}
