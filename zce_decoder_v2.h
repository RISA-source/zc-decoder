/*
 * zce_decoder.h  —  Zero-Count Encoding  v1.0  FROZEN
 * ======================================================
 *
 * Single-header decoder. Drop into any C project. No build system needed.
 * gcc -O2 -o prog prog.c        (include this header, call zce_init + zce_run)
 *
 * TOKEN FORMAT
 *   [N zeros][M ones][0]
 *   N = token type (what this operation IS)
 *   M = parse-tree depth (structural position, does not affect execution)
 *   0 = terminator
 *   Self-delimiting. No length prefix. No lookahead beyond the zero.
 *
 * ELIAS GAMMA
 *   Encode v: let x=v+1, k=floor(log2 x), write k zeros then x in binary.
 *   Decode:   count k leading zeros, read k+1 bits, value = result - 1.
 *   Self-delimiting. No external length needed.
 *
 * STREAM LAYOUT
 *   elias(grammar_bits)          — bit count of grammar section
 *   [grammar_bits of tokens]     — DEFINE_OP definitions + executable tokens
 *   [0-7 padding bits]           — pad to byte boundary
 *
 * BASE GRAMMAR — 5 tokens, 3 immutable
 *   N=1  SET        slots[dst] = immediate_value
 *   N=2  ADD        slots[dst] = slots[a] + slots[b]
 *   N=3  JIF        if slots[cond]==0: jump to grammar_start+offset  [IMMUTABLE]
 *   N=4  JMP        jump to grammar_start+offset                     [IMMUTABLE]
 *   N=5  DEFINE_OP  teach decoder a new operation from the stream    [IMMUTABLE]
 *
 * Everything else (SUB, MUL, DIV, floats, ML ops...) is defined by
 * DEFINE_OP tokens in the stream. The decoder learns inline.
 *
 * FORWARD COMPATIBILITY
 *   A stream is decodable iff every token N it uses either:
 *   (a) is a base primitive (N=1..5), or
 *   (b) has a DEFINE_OP earlier in the same stream.
 *   No decoder update ever required. Encoders evolve. This file does not.
 *
 * DEFINE_OP WIRE FORMAT
 *   tok(5,1)               DEFINE_OP token
 *   elias(target)          token slot to define (must be 6..63, not JIF/JMP/DEFINE_OP)
 *   elias(n_args - 1)      argument count minus one
 *   elias(arg_type[i])...  type of each argument
 *   elias(action)          action code (0=EXEC, 1=JIF, 2=JMP, 3=NOP)
 *   elias(action_param)    for action=EXEC: which executor op to call
 *
 * USAGE
 *   ZCEDecoder d;
 *   zce_init(&d, data, len);
 *   int ok = zce_run(&d);          // 0 = success, -1 = error
 *   int64_t v = zce_slot(&d, 0);   // read result from slot 0
 *
 * EXTENDED OPS
 *   The base executor handles only SET and ADD.
 *   All other action_param values call zce_exec_extended(d, param, args).
 *   Define this function before including this header to add SUB, MUL,
 *   floats, or any domain-specific operations.
 *
 *   Example:
 *     static void zce_exec_extended(ZCEDecoder *d, int param, int64_t *args) {
 *         if (param == 3)  // SUB
 *             d->slots[args[0]] = d->slots[args[1]] - d->slots[args[2]];
 *     }
 *     #include "zce_decoder.h"
 *
 * This file does not change. Flash it once. Run anything.
 */

#ifndef ZCE_DECODER_H
#define ZCE_DECODER_H

#include <stdint.h>
#include <string.h>
#include <stdio.h>

/* ── Limits ─────────────────────────────────────────────── */

#define ZCE_MAX_RULES   64
#define ZCE_MAX_SLOTS   4096
#define ZCE_MAX_ARGS    8
#define ZCE_MAX_STEPS   10000000

/* ── Argument types ─────────────────────────────────────── */

#define ZCE_ARG_ELIAS_PTR  0   /* Elias gamma → slot index */
#define ZCE_ARG_ELIAS_VAL  1   /* Elias gamma → immediate value */
#define ZCE_ARG_RAW32      2   /* 32 raw bits (used for IEEE 754 floats) */

/* ── Action codes ───────────────────────────────────────── */

#define ZCE_ACT_EXEC  0   /* call executor with action_param */
#define ZCE_ACT_JIF   1   /* conditional branch */
#define ZCE_ACT_JMP   2   /* unconditional branch */
#define ZCE_ACT_NOP   3   /* no operation (used by DEFINE_OP itself) */

/* ── Base executor op codes ─────────────────────────────── */

#define ZCE_OP_SET  1   /* slots[dst] = immediate */
#define ZCE_OP_ADD  2   /* slots[dst] = slots[a] + slots[b] */
/* op codes 3+ are handled by zce_exec_extended if defined */

/* ── Base token slots ───────────────────────────────────── */

#define ZCE_TOK_SET        1
#define ZCE_TOK_ADD        2
#define ZCE_TOK_JIF        3   /* immutable */
#define ZCE_TOK_JMP        4   /* immutable */
#define ZCE_TOK_DEFINE_OP  5   /* immutable */

/* ── Grammar rule ───────────────────────────────────────── */

typedef struct {
    int defined;
    int n_args;
    int arg_types[ZCE_MAX_ARGS];
    int action;
    int action_param;
} ZCERule;

/* ── Decoder state ──────────────────────────────────────── */

typedef struct ZCEDecoder {
    const uint8_t *data;
    int            data_len;
    int            bit_pos;
    ZCERule        rules[ZCE_MAX_RULES];
    int64_t        slots[ZCE_MAX_SLOTS];
    int            grammar_start;
    int            grammar_end;
    int            steps;
    int            error;
    char           errmsg[128];

    /* Extended executor hook.
       Called for any action_param value not handled by the base executor.
       Set after zce_init() to handle SUB, MUL, floats, domain ops, etc.
       If NULL, unknown action_param values are silently skipped.

       Example:
         zce_init(&d, data, len);
         d.exec_ext = my_extended_ops;
    */
    void (*exec_ext)(struct ZCEDecoder *d, int param, int64_t *args);
} ZCEDecoder;

/* ── Bit reader ─────────────────────────────────────────── */

static int zce_bit(ZCEDecoder *d) {
    if (d->bit_pos >= d->data_len * 8) {
        d->error = 1;
        snprintf(d->errmsg, sizeof d->errmsg,
                 "read past end at bit %d", d->bit_pos);
        return 0;
    }
    int b = d->bit_pos / 8;
    int p = 7 - (d->bit_pos % 8);
    d->bit_pos++;
    return (d->data[b] >> p) & 1;
}

static int zce_peek(ZCEDecoder *d) {
    if (d->bit_pos >= d->data_len * 8) return -1;
    return (d->data[d->bit_pos / 8] >> (7 - (d->bit_pos % 8))) & 1;
}

/* Read n raw bits, MSB first */
static uint32_t zce_raw(ZCEDecoder *d, int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; i++)
        v = (v << 1) | (uint32_t)zce_bit(d);
    return v;
}

/* ── Elias gamma decoder ────────────────────────────────── */

static int zce_elias(ZCEDecoder *d) {
    int k = 0;
    while (!d->error && zce_peek(d) == 0) {
        zce_bit(d);
        if (++k > 30) {
            d->error = 1;
            snprintf(d->errmsg, sizeof d->errmsg,
                     "elias overflow at bit %d", d->bit_pos);
            return -1;
        }
    }
    return d->error ? -1 : (int)(zce_raw(d, k + 1) - 1);
}

/* ── Token reader ───────────────────────────────────────── */

static int zce_token(ZCEDecoder *d) {
    int n = 0;
    while (!d->error && zce_peek(d) == 0) {
        zce_bit(d);
        if (++n >= ZCE_MAX_RULES) { d->error = 1; return -1; }
    }
    while (!d->error && zce_peek(d) == 1) zce_bit(d); /* depth — consumed, discarded */
    if (!d->error) zce_bit(d);                         /* terminator zero */
    return d->error ? -1 : n;
}

/* ── Argument reader ────────────────────────────────────── */

static int64_t zce_arg(ZCEDecoder *d, int type) {
    switch (type) {
        case ZCE_ARG_ELIAS_PTR:
        case ZCE_ARG_ELIAS_VAL:
            return zce_elias(d);
        case ZCE_ARG_RAW32:
            /* FIX: was zce_read_raw() which does not exist.
               Correct call is zce_raw() which is defined above. */
            return (int64_t)zce_raw(d, 32);
        default:
            d->error = 1;
            snprintf(d->errmsg, sizeof d->errmsg,
                     "unknown arg type %d", type);
            return 0;
    }
}

/* ── Slot accessors (bounds-checked) ────────────────────── */

static int64_t zce_get(ZCEDecoder *d, int64_t s) {
    return (s >= 0 && s < ZCE_MAX_SLOTS) ? d->slots[s] : 0;
}

static void zce_setslot(ZCEDecoder *d, int64_t s, int64_t v) {
    if (s >= 0 && s < ZCE_MAX_SLOTS) d->slots[s] = v;
}

/* ── Base grammar initialisation ────────────────────────── */

static void zce_base_grammar(ZCEDecoder *d) {
    /* SET: dst=PTR, val=VAL */
    d->rules[ZCE_TOK_SET].defined      = 1;
    d->rules[ZCE_TOK_SET].n_args       = 2;
    d->rules[ZCE_TOK_SET].arg_types[0] = ZCE_ARG_ELIAS_PTR;
    d->rules[ZCE_TOK_SET].arg_types[1] = ZCE_ARG_ELIAS_VAL;
    d->rules[ZCE_TOK_SET].action       = ZCE_ACT_EXEC;
    d->rules[ZCE_TOK_SET].action_param = ZCE_OP_SET;

    /* ADD: dst=PTR, a=PTR, b=PTR */
    d->rules[ZCE_TOK_ADD].defined      = 1;
    d->rules[ZCE_TOK_ADD].n_args       = 3;
    d->rules[ZCE_TOK_ADD].arg_types[0] = ZCE_ARG_ELIAS_PTR;
    d->rules[ZCE_TOK_ADD].arg_types[1] = ZCE_ARG_ELIAS_PTR;
    d->rules[ZCE_TOK_ADD].arg_types[2] = ZCE_ARG_ELIAS_PTR;
    d->rules[ZCE_TOK_ADD].action       = ZCE_ACT_EXEC;
    d->rules[ZCE_TOK_ADD].action_param = ZCE_OP_ADD;

    /* JIF: cond=PTR, offset=VAL */
    d->rules[ZCE_TOK_JIF].defined      = 1;
    d->rules[ZCE_TOK_JIF].n_args       = 2;
    d->rules[ZCE_TOK_JIF].arg_types[0] = ZCE_ARG_ELIAS_PTR;
    d->rules[ZCE_TOK_JIF].arg_types[1] = ZCE_ARG_ELIAS_VAL;
    d->rules[ZCE_TOK_JIF].action       = ZCE_ACT_JIF;

    /* JMP: offset=VAL */
    d->rules[ZCE_TOK_JMP].defined      = 1;
    d->rules[ZCE_TOK_JMP].n_args       = 1;
    d->rules[ZCE_TOK_JMP].arg_types[0] = ZCE_ARG_ELIAS_VAL;
    d->rules[ZCE_TOK_JMP].action       = ZCE_ACT_JMP;

    /* DEFINE_OP: handled specially in zce_run, no args at rule level */
    d->rules[ZCE_TOK_DEFINE_OP].defined = 1;
    d->rules[ZCE_TOK_DEFINE_OP].n_args  = 0;
    d->rules[ZCE_TOK_DEFINE_OP].action  = ZCE_ACT_NOP;
}

/* ── DEFINE_OP reader ───────────────────────────────────── */

static int zce_read_define_op(ZCEDecoder *d) {
    int target = zce_elias(d);
    if (d->error) return -1;

    /* Immutability: JIF, JMP, DEFINE_OP cannot be redefined.
       Also reject out-of-range and N=0 (padding).
       Read the payload anyway to keep stream alignment. */
    int reject = (target <= 0 || target >= ZCE_MAX_RULES ||
                  target == ZCE_TOK_JIF        ||
                  target == ZCE_TOK_JMP        ||
                  target == ZCE_TOK_DEFINE_OP);

    ZCERule scratch;
    ZCERule *r = reject ? &scratch : &d->rules[target];
    memset(r, 0, sizeof *r);

    int n_args = zce_elias(d) + 1;   /* wire: n_args-1, decode adds 1 */
    if (d->error) return -1;
    if (n_args < 1 || n_args > ZCE_MAX_ARGS) {
        d->error = 1;
        snprintf(d->errmsg, sizeof d->errmsg,
                 "DEFINE_OP n_args=%d out of range [1,%d]", n_args, ZCE_MAX_ARGS);
        return -1;
    }
    r->n_args = n_args;

    for (int i = 0; i < n_args; i++) {
        r->arg_types[i] = zce_elias(d);
        if (d->error) return -1;
    }

    r->action = zce_elias(d);
    if (d->error) return -1;

    /* action_param only present for EXEC actions */
    if (r->action == ZCE_ACT_EXEC) {
        r->action_param = zce_elias(d);
        if (d->error) return -1;
    }

    if (!reject) r->defined = 1;
    return 0;
}

/* ── Extended op hook ───────────────────────────────────── */
/* Set d.exec_ext after zce_init() to handle ops beyond SET and ADD. */
/* (No macro needed — the function pointer in the struct handles this.) */

/* ── Init ───────────────────────────────────────────────── */

static void zce_init(ZCEDecoder *d, const uint8_t *data, int len) {
    memset(d, 0, sizeof *d);
    d->data      = data;
    d->data_len  = len;
    d->exec_ext  = NULL;   /* no extended ops by default */
    zce_base_grammar(d);
}

/* ── Run ────────────────────────────────────────────────── */

static int __attribute__((unused)) zce_run(ZCEDecoder *d) {
    if (!d->data || d->data_len < 1) {
        d->error = 1;
        snprintf(d->errmsg, sizeof d->errmsg, "empty stream");
        return -1;
    }

    /* Header: elias(grammar_bits) */
    int grammar_bits = zce_elias(d);
    if (d->error) return -1;

    d->grammar_start = d->bit_pos;
    d->grammar_end   = d->bit_pos + grammar_bits;
    d->steps         = 0;

    while (!d->error
           && d->bit_pos < d->grammar_end
           && d->steps < ZCE_MAX_STEPS) {

        d->steps++;
        int tok = zce_token(d);
        if (d->error || tok < 0 || d->bit_pos > d->grammar_end) break;
        if (tok == 0) continue;  /* padding */

        /* DEFINE_OP: grammar extension */
        if (tok == ZCE_TOK_DEFINE_OP) {
            zce_read_define_op(d);
            continue;
        }

        /* Unknown: stream violated self-describing requirement */
        if (!d->rules[tok].defined) {
            d->error = 1;
            snprintf(d->errmsg, sizeof d->errmsg,
                     "token N=%d used without prior DEFINE_OP", tok);
            break;
        }

        /* Read arguments */
        ZCERule *rule = &d->rules[tok];
        int64_t args[ZCE_MAX_ARGS] = {0};
        for (int i = 0; i < rule->n_args && !d->error; i++)
            args[i] = zce_arg(d, rule->arg_types[i]);
        if (d->error) break;

        /* Execute */
        switch (rule->action) {

            case ZCE_ACT_EXEC:
                switch (rule->action_param) {
                    case ZCE_OP_SET:
                        zce_setslot(d, args[0], args[1]);
                        break;
                    case ZCE_OP_ADD:
                        zce_setslot(d, args[0],
                                    zce_get(d, args[1]) + zce_get(d, args[2]));
                        break;
                    default:
                        /* Delegate to extended executor if set */
                        if (d->exec_ext)
                            d->exec_ext(d, rule->action_param, args);
                        /* else: silently skip (forward compatible) */
                        break;
                }
                break;

            case ZCE_ACT_JIF: {
                /* Branch if slots[cond] == 0 */
                int target = d->grammar_start + (int)args[1];
                if (zce_get(d, args[0]) == 0) {
                    if (target < d->grammar_start || target > d->grammar_end) {
                        d->error = 1;
                        snprintf(d->errmsg, sizeof d->errmsg,
                                 "JIF target %d out of bounds [%d,%d]",
                                 target, d->grammar_start, d->grammar_end);
                        break;
                    }
                    d->bit_pos = target;
                }
                break;
            }

            case ZCE_ACT_JMP: {
                int target = d->grammar_start + (int)args[0];
                if (target < d->grammar_start || target > d->grammar_end) {
                    d->error = 1;
                    snprintf(d->errmsg, sizeof d->errmsg,
                             "JMP target %d out of bounds [%d,%d]",
                             target, d->grammar_start, d->grammar_end);
                    break;
                }
                d->bit_pos = target;
                break;
            }

            case ZCE_ACT_NOP:
            default:
                break;
        }
    }

    if (d->error) {
        fprintf(stderr, "[ZCE] error: %s\n", d->errmsg);
        return -1;
    }
    return 0;
}

/* ── Query ──────────────────────────────────────────────── */

static int64_t __attribute__((unused)) zce_slot(ZCEDecoder *d, int s) {
    return zce_get(d, (int64_t)s);
}

#endif /* ZCE_DECODER_H */
