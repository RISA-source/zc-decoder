# ZCE Decoder — Complete Documentation

**File:** `zce_decoder_v2.h`  
**Status:** FROZEN. This file does not change. Ever.  
**Version:** 1.0  
**Language:** C (C99, no dependencies beyond stdint.h, string.h, stdio.h)

---

## What This Is

ZCE stands for Zero-Count Encoding. The decoder is a single C header file that can execute ZCE bytecode streams. You drop it into any C project, include it, and you have a complete bytecode execution engine with no external dependencies, no build system requirements, and no update mechanism needed — ever.

The core claim of ZCE is this: **a stream can teach the decoder what it needs to know**. A ZCE stream carries its own instruction set definitions inside itself. If a stream uses an operation the decoder doesn't natively know about, it defines that operation using a `DEFINE_OP` token before using it. This means the decoder never needs to be updated to run new streams. The stream is the authority on its own instruction set.

This is not a minor engineering convenience. It is a fundamental architectural property with real consequences. A chip manufacturer can burn this decoder into ROM today. Five years from now, encoders will emit richer instruction sets, define new operations for ML inference or signal processing or whatever hasn't been invented yet — and those streams will still run on the original frozen decoder without modification.

---

## The Problem This Solves

Embedded systems have a deep ossification problem. A device ships with firmware. The firmware contains a bytecode interpreter. Six months later you need the interpreter to understand a new operation. You push a firmware update. On millions of devices. With all the risk, cost, and failure modes that entails.

The standard answer to this is to version the protocol — define v1, v2, v3 of the instruction set, negotiate on connection, handle compatibility matrices. This creates its own complexity and still requires decoder updates when you cross version boundaries.

ZCE's answer is different: make the stream self-describing at the instruction level. A stream that uses `MULTIPLY` starts with a `DEFINE_OP` that tells the decoder "token N means: take three slot pointers, multiply slots[a] by slots[b], store in slots[dst]". The decoder learns this from the stream and executes accordingly. No version negotiation. No decoder update. No compatibility matrix.

---

## Architecture Overview

The decoder is built in layers:

```
Stream bytes
    ↓
Bit reader (zce_bit, zce_peek, zce_raw)
    ↓
Elias gamma decoder (zce_elias)
    ↓
Token reader (zce_token)
    ↓
Argument reader (zce_arg)
    ↓
Execution engine (zce_run)
    ↓
Slot array (int64_t slots[4096])
```

Each layer is simple and independent. The bit reader knows nothing about tokens. The token reader knows nothing about execution. The execution engine knows nothing about bit layout. This separation means each layer can be reasoned about and tested independently.

---

## The Token Format

Every instruction in a ZCE stream starts with a token. The token format is:

```
[N zeros][M ones][0 terminator]
```

Where:
- `N` = the token type (what operation this is)
- `M` = parse-tree depth (structural position in a grammar tree — currently consumed and discarded by the executor, reserved for future use)
- `0` = the terminator bit that ends the token

This encoding is self-delimiting. You do not need to know the length of a token before you start reading it. You read zeros until you see a one, count them, then read ones until you see a zero, then read the final terminator zero. At that point you have `N` and `M` and the stream position is exactly at the start of the first argument.

**Why this encoding?**

Because small token numbers (the common ones like SET=1, ADD=2) produce short bit strings. Token type 1 (SET) encodes as `010` — three bits. Token type 2 (ADD) encodes as `0010` — four bits. The most frequent operations cost the fewest bits. This is the same principle as Huffman coding but without requiring a symbol frequency table — the code is determined entirely by the token number.

The depth field (the M ones) is a structural annotation. In a grammar tree where instructions have parent-child relationships, depth encodes position. For a flat program (which is what most ZCE programs are), depth is always 1. The encoder emits depth=1 for all tokens. The decoder reads and discards the depth bits. This means the depth field costs one bit in the common case and is available for future structural uses without changing the wire format.

**Token encoding table (depth=1):**

| Token N | Bit pattern | Length |
|---------|-------------|--------|
| 0 | `10` | 2 bits |
| 1 (SET) | `010` | 3 bits |
| 2 (ADD) | `0010` | 4 bits |
| 3 (JIF) | `00010` | 5 bits |
| 4 (JMP) | `000010` | 6 bits |
| 5 (DEFINE_OP) | `0000010` | 7 bits |
| N | `[N zeros]10` | N+2 bits |

---

## Elias Gamma Encoding

All arguments in a ZCE stream are encoded using Elias gamma coding. This is a universal code for non-negative integers that requires no external length prefix.

**Encoding procedure:**

To encode value `v`:
1. Let `x = v + 1`
2. Let `k = floor(log2(x))`
3. Write `k` zero bits
4. Write `x` in binary using `k+1` bits

**Decoding procedure:**

1. Count leading zero bits — that count is `k`
2. Read the next `k+1` bits as a binary integer — call it `x`
3. Value is `x - 1`

**Examples:**

| Value | x | k | Encoded | Length |
|-------|---|---|---------|--------|
| 0 | 1 | 0 | `1` | 1 bit |
| 1 | 2 | 1 | `010` | 3 bits |
| 2 | 3 | 1 | `011` | 3 bits |
| 3 | 4 | 2 | `00100` | 5 bits |
| 7 | 8 | 3 | `0001000` | 7 bits |
| 15 | 16 | 4 | `000010000` | 9 bits |

**Why Elias gamma?**

Small values (small slot indices, small immediate values) are overwhelmingly common in real programs. A program that accesses slot 0, slot 1, slot 2 — which is almost every program — pays 1, 3, 3 bits for those indices instead of a fixed 16 bits. For a program with 20 instructions using small slot indices and small values, Elias encoding can produce a stream 60-70% smaller than a fixed-width encoding.

Elias gamma is also self-delimiting — no length prefix needed, the decoder knows exactly where each encoded integer ends. This property is essential for a binary format where every bit position matters.

---

## Stream Layout

A complete ZCE stream has this structure:

```
[elias(grammar_bits)][grammar section][0-7 padding bits]
```

The header is a single Elias-encoded integer telling the decoder how many bits the grammar section contains. After reading this integer, the decoder knows exactly where the grammar section ends. It executes tokens until it reaches that boundary.

The padding at the end brings the total stream length to a byte boundary, because streams are stored and transmitted as bytes, not as raw bit sequences.

**Why not a fixed-size header?**

A fixed 16-bit or 32-bit header would impose an upper limit on grammar size and waste bits for small programs. An Elias-encoded header is self-delimiting and scales from tiny programs (a few bytes) to large ones (megabytes) with no waste and no limit.

---

## Base Grammar

The decoder ships with five built-in token definitions. These are hardcoded and always available without any `DEFINE_OP` preamble:

### N=1: SET
```
slots[dst] = immediate_value
```
Arguments: `elias(dst)`, `elias(value)`

Sets a slot to an immediate integer value. The destination is a slot index. The value is a literal integer encoded directly into the stream.

### N=2: ADD
```
slots[dst] = slots[a] + slots[b]
```
Arguments: `elias(dst)`, `elias(a)`, `elias(b)`

Adds two slots and stores the result. All three arguments are slot indices.

### N=3: JIF (IMMUTABLE)
```
if slots[cond] == 0: jump to grammar_start + offset
```
Arguments: `elias(cond)`, `elias(offset)`

Conditional branch. If the value at slot `cond` is zero, jumps to bit position `grammar_start + offset`. The offset is relative to the start of the grammar section, not the start of the file. This means jump targets are stable regardless of stream header size.

JIF is the only conditional construct in the base grammar. All higher-level control flow (if/else, while, for, switch) compiles to combinations of JIF and JMP.

**Why branch on zero?** Because testing equality to zero is the most general comparison. Any other comparison (`a > b`, `a == b`, `a != b`) can be reduced to "is some computed value zero?". For example, `a > b` compiles to `SUB temp a b; JIF temp exit` — if `a - b` is zero (they're equal) or negative, don't branch; if positive, branch.

### N=4: JMP (IMMUTABLE)
```
jump to grammar_start + offset
```
Arguments: `elias(offset)`

Unconditional jump. Same offset convention as JIF.

### N=5: DEFINE_OP (IMMUTABLE)
Teaches the decoder a new operation. See the full section below.

---

## The DEFINE_OP Mechanism

`DEFINE_OP` is the heart of ZCE's self-describing property. It appears in the stream before the first use of any operation the decoder doesn't natively know.

**Wire format:**

```
tok(5,1)              DEFINE_OP token
elias(target)         token slot to define (6..63, not JIF/JMP/DEFINE_OP)
elias(n_args - 1)     argument count minus one (decoder adds 1 back)
elias(arg_type[0])    type of first argument
elias(arg_type[1])    type of second argument
...                   (n_args times)
elias(action)         action code
elias(action_param)   present only when action == ACT_EXEC
```

**Argument types:**

| Code | Name | Meaning |
|------|------|---------|
| 0 | ARG_ELIAS_PTR | Elias-encoded slot index |
| 1 | ARG_ELIAS_VAL | Elias-encoded immediate value |
| 2 | ARG_RAW32 | 32 raw bits (used for IEEE 754 float literals) |

**Action codes:**

| Code | Name | Meaning |
|------|------|---------|
| 0 | ACT_EXEC | Call executor with action_param |
| 1 | ACT_JIF | Conditional branch (same semantics as base JIF) |
| 2 | ACT_JMP | Unconditional branch (same semantics as base JMP) |
| 3 | ACT_NOP | No operation |

**Example: defining SUB**

To define token N=6 as a three-argument integer subtraction:

```
DEFINE_OP token
elias(6)          target = token 6
elias(2)          n_args - 1 = 2, so n_args = 3
elias(0)          arg[0] type = ARG_ELIAS_PTR (destination slot)
elias(0)          arg[1] type = ARG_ELIAS_PTR (source slot a)
elias(0)          arg[2] type = ARG_ELIAS_PTR (source slot b)
elias(0)          action = ACT_EXEC
elias(3)          action_param = 3 (OP_SUB, handled by exec_ext)
```

After this DEFINE_OP appears in the stream, the decoder knows that whenever it encounters token N=6, it should read three Elias-encoded slot indices and call `exec_ext(d, 3, args)`.

**Why n_args - 1?**

Because Elias gamma cannot encode zero (it starts at 0 meaning value 0, but that would mean zero arguments which is invalid for any executable operation). By storing `n_args - 1`, we use the Elias gamma range efficiently: value 0 means 1 argument, value 1 means 2 arguments, value 7 means 8 arguments.

**Immutability rules:**

Three token slots can never be targeted by DEFINE_OP:
- N=3 (JIF) — the conditional branch semantic must be stable for all programs
- N=4 (JMP) — the unconditional branch semantic must be stable for all programs  
- N=5 (DEFINE_OP) — cannot redefine the definition mechanism itself

If a DEFINE_OP attempts to target one of these slots, the decoder reads and discards the payload (to maintain stream alignment) but does not register the definition. The stream continues executing normally.

Additionally, DEFINE_OP targeting N=0 (padding) or N >= 64 (out of rule table range) is similarly rejected. The decoder reads the payload for alignment and moves on.

This rejection behavior is intentional. It means a stream that contains an invalid DEFINE_OP does not crash — it just ignores the invalid definition and continues. Downstream tokens that reference that undefined slot will produce an error at execution time, which is the correct failure mode.

---

## The Extension Hook

The base executor handles only two operation codes: `OP_SET` (1) and `OP_ADD` (2). Every other operation — subtraction, multiplication, floats, ML ops, domain-specific computations — is handled by the extension hook.

The hook is a function pointer stored on the decoder struct:

```c
void (*exec_ext)(ZCEDecoder *d, int param, int64_t *args);
```

When the executor encounters an `ACT_EXEC` action with an `action_param` value it doesn't handle natively, it calls `d->exec_ext(d, action_param, args)` if the pointer is non-NULL. If the pointer is NULL, the operation is silently skipped — which is the forward-compatible behaviour (the decoder can execute future streams even if it doesn't understand all their operations, as long as those operations are not load-bearing for the computation it cares about).

**Why a function pointer on the struct rather than a global function?**

Several reasons:

1. You can have two decoder instances in the same process with different extension sets. One for integer-only computation, one with full float support.

2. You can swap the extension at runtime. A decoder could start with a minimal extension set and switch to a richer one after verifying stream compatibility.

3. No macro tricks, no link-time shenanigans, no `#define` gymnastics. A function pointer is the standard C mechanism for runtime dispatch. Clean and obvious.

4. The decoder header is self-contained. You can include it in any project without defining any external symbols.

**Using the extension hook:**

```c
#include "zce_decoder_v2.h"
#include "zce_stdlib.h"      // provides zce_stdlib_exec

ZCEDecoder d;
zce_init(&d, data, len);
d.exec_ext = zce_stdlib_exec;   // install standard ops
zce_run(&d);
```

---

## Slot Array

The decoder maintains an array of 4096 `int64_t` slots. These are the working memory of a ZCE program. All operations read from and write to slots.

Slots are indexed from 0 to 4095. Slot 0 is a valid general-purpose slot — there is no implicit zero slot, no stack pointer, no program counter stored in slots. All slots start at zero on `zce_init`.

Floats are stored in slots as raw IEEE 754 32-bit bit patterns in the lower 32 bits of the int64. The `zce_stdlib.h` float operations use `memcpy` to bit-cast between float and uint32 — the correct way to type-pun in C.

**Why 4096 slots?**

It's a practical limit. Large enough to hold any reasonable intermediate computation (ML inference over a small network, sensor fusion, etc.) and small enough that the slot array (32KB) fits entirely in L1 cache on most modern hardware. The limit is a `#define` — `ZCE_MAX_SLOTS` — and can be increased at compile time for applications that need it.

---

## Bounds Checking

Every memory access in the decoder is bounds-checked:

- Bit reads check against `data_len * 8`
- Slot reads and writes check against `[0, ZCE_MAX_SLOTS)`
- Jump targets check against `[grammar_start, grammar_end]`
- Token N checks against `ZCE_MAX_RULES`
- Elias gamma decoding has an overflow guard at k > 30

The decoder never invokes undefined behaviour on malformed input. A malformed stream sets `d.error = 1` and `d.errmsg` to a description, then `zce_run` returns -1. The caller can inspect these fields to understand what went wrong.

---

## Execution Limits

The decoder has a hard step limit: `ZCE_MAX_STEPS = 10,000,000`. If a program executes more than ten million instructions without completing, `zce_run` returns -1 with an appropriate error. This prevents infinite loops from hanging the host system.

This limit is intentional rather than a hack. A ZCE decoder running on an embedded system with real-time constraints needs a guaranteed execution bound. Ten million steps is enough for any reasonable computation but not enough to starve a real-time system.

The limit is a `#define` and can be adjusted at compile time.

---

## Error Handling

The decoder uses a simple error flag model rather than exceptions or setjmp:

```c
d.error    // 1 if an error occurred, 0 otherwise
d.errmsg   // human-readable description (128 chars max)
```

Every bit-reading function checks `d.error` before proceeding. Once an error is set, all subsequent reads return 0 or -1 without side effects. This means you don't need to check for errors inside the execution loop — the error propagates naturally and `zce_run` checks it at the end.

Error conditions:
- `"empty stream"` — data is NULL or length is zero
- `"read past end at bit N"` — stream truncated mid-instruction
- `"elias overflow at bit N"` — Elias gamma k > 30 (corrupt or malicious stream)
- `"token N=X used without prior DEFINE_OP"` — stream uses undefined operation
- `"JIF target N out of bounds [A,B]"` — jump would leave grammar section
- `"JMP target N out of bounds [A,B]"` — jump would leave grammar section
- `"DEFINE_OP n_args=N out of range [1,8]"` — invalid DEFINE_OP payload

---

## Public API

```c
// Initialise decoder. Zeros all state. Installs base grammar.
void zce_init(ZCEDecoder *d, const uint8_t *data, int len);

// Execute the stream. Returns 0 on success, -1 on error.
// On error, d.errmsg contains a description.
int zce_run(ZCEDecoder *d);

// Read a slot value after successful execution.
int64_t zce_slot(ZCEDecoder *d, int slot_index);
```

Minimal usage:

```c
#include "zce_decoder_v2.h"
#include "zce_stdlib.h"

uint8_t stream[] = { /* ZCE bytes */ };

ZCEDecoder d;
zce_init(&d, stream, sizeof(stream));
d.exec_ext = zce_stdlib_exec;

if (zce_run(&d) != 0) {
    fprintf(stderr, "Error: %s\n", d.errmsg);
    return -1;
}

int64_t result = zce_slot(&d, 0);
```

---

## Forward Compatibility — The Formal Statement

A ZCE stream is fully decodable by this decoder if and only if every token type N it uses satisfies at least one of:

(a) N is a base primitive: N ∈ {1, 2, 3, 4, 5}  
(b) A DEFINE_OP for N appears earlier in the same stream

No external schema. No version negotiation. No decoder update ever required.

If a stream uses an operation that has a DEFINE_OP but whose `action_param` is not handled by the installed `exec_ext`, the operation is silently skipped. The stream continues executing. This is correct forward-compatible behaviour — a decoder that doesn't understand an operation should not crash when encountering it, it should skip it.

This means you can send a stream that uses GPU acceleration opcodes to a device that has no GPU. The GPU ops are skipped. The rest of the program executes. Whether that produces a useful result is the encoder's problem — the decoder's contract is just "don't crash on unknown ops".

---

## Why It Is Frozen

The decoder is frozen — not updated, not patched, not versioned — because that is the entire point of its existence.

If the decoder could change, then streams would need to specify which decoder version they require. Devices in the field would need to track decoder versions. Compatibility matrices would emerge. The ossification problem would return in a different form.

By making the decoder permanently frozen and making streams self-describing, we eliminate the compatibility problem entirely. A stream produced in 2035 will run on a decoder compiled in 2024. Not because of careful versioning. Because the stream teaches the decoder what it needs to know.

The frozen status is enforced by design: the decoder has no configuration, no feature flags, no version negotiation, no upgrade path. It is what it is. The only valid response to "the decoder needs to change" is "no it doesn't — express it in DEFINE_OP".

---

## File Structure

```c
// Constants
ZCE_MAX_RULES, ZCE_MAX_SLOTS, ZCE_MAX_ARGS, ZCE_MAX_STEPS

// Argument type constants
ZCE_ARG_ELIAS_PTR, ZCE_ARG_ELIAS_VAL, ZCE_ARG_RAW32

// Action code constants
ZCE_ACT_EXEC, ZCE_ACT_JIF, ZCE_ACT_JMP, ZCE_ACT_NOP

// Base op codes
ZCE_OP_SET, ZCE_OP_ADD

// Base token slots
ZCE_TOK_SET, ZCE_TOK_ADD, ZCE_TOK_JIF, ZCE_TOK_JMP, ZCE_TOK_DEFINE_OP

// Types
ZCERule       — grammar rule: n_args, arg_types, action, action_param
ZCEDecoder    — full decoder state including slots, rules, exec_ext

// Internal (static, not for external use)
zce_bit()     — read one bit
zce_peek()    — peek one bit without consuming
zce_raw()     — read N raw bits
zce_elias()   — decode one Elias gamma integer
zce_token()   — decode one token (returns N)
zce_arg()     — decode one argument by type
zce_get()     — bounds-checked slot read
zce_setslot() — bounds-checked slot write
zce_base_grammar() — install base 5-token grammar
zce_read_define_op() — parse and register a DEFINE_OP payload

// Public API
zce_init()    — initialise decoder
zce_run()     — execute stream
zce_slot()    — read slot value
```

---

## Compilation

Single include, no external dependencies:

```bash
# Any file that includes zce_decoder_v2.h compiles directly
gcc -O2 -o myprogram myprogram.c -lm
```

The `-lm` is only needed if you include `zce_stdlib.h` alongside it (for `expf`, `tanhf`, etc.). The decoder header itself has no math dependencies.

Tested with gcc and clang. Compiles clean with `-Wall -Wextra`. C99 minimum.

---

## The Complete Picture

```
You write:          from zce_encoder import Program, Encoder
                    p = Program()
                    p.set(0, 10); p.set(1, 32); p.add(2, 0, 1)
                    stream = Encoder().encode(p)

Stream bytes:       [elias(grammar_bits)][SET 0 10][SET 1 32][ADD 2 0 1][padding]

Decoder runs:       zce_init(&d, stream, len)
                    zce_run(&d)
                    → reads elias header, learns grammar_bits
                    → reads token N=1 (SET), reads elias(0), elias(10), sets slot[0]=10
                    → reads token N=1 (SET), reads elias(1), elias(32), sets slot[1]=32
                    → reads token N=2 (ADD), reads elias(2), elias(0), elias(1),
                      sets slot[2] = slot[0] + slot[1] = 42
                    → reaches grammar_end, returns 0

Result:             zce_slot(&d, 2) == 42
```

This works identically whether the stream was produced yesterday or in five years. The decoder does not change. The stream tells it everything it needs.
