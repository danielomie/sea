# Feature: JIT (Just-In-Time) Compiler

> **Status:** Fases 0–4 **implementadas** (ver §6).
> **Default:** **desligado**. Liga-se apenas com a flag de linha de comando `--jit`,
> e só tem efeito num binário com JIT (`make sea-jit` → `bin/sea-jit`, ou `make jit`).
> **Backend:** *method JIT* via **libgccjit**, carregada com `dlopen` em runtime.
>
> **Estado atual (resumo):**
> - **O que compila:** funções quentes (≥ `SEA_JIT_THRESHOLD`=50 chamadas) e
>   qualquer chunk com laço quente (≥ `SEA_JIT_LOOP_THRESHOLD`=1000 back-edges) —
>   inclusive o chunk principal e funções chamadas uma única vez.
> - **Entrada por OSR:** o código nativo pode ser entrado no início do chunk
>   (chamada) ou na cabeça de qualquer laço "limpo" (todo estado vive em
>   `frame->base`, então qualquer fronteira de instrução é ponto de entrada
>   válido). O interpretador troca para o nativo no back-edge.
> - **Compilação fora do caminho crítico:** um **processo servidor** (fork no
>   startup) gera um `.so` por chunk; uma **thread worker** na VM alimenta o
>   servidor a partir de uma fila. O interpretador nunca espera uma compilação,
>   e a VM pode sair a qualquer momento (o servidor termina e limpa seus temp
>   files sozinho). Fallback: libgccjit in-process.
> - **Subconjunto suportado:** loads/moves, aritmética int/float (incl.
>   `MOD`/`IDIV`/`POW` com caminho float, `NEG` float), bitwise
>   (`BAND/BOR/BXOR/BNOT/SHL/SHR`), comparações, jumps, for-loops numéricos,
>   arrays genéricos e tipados (inline com guarda tipo+bounds), chamadas
>   (inclusive dentro de laços), campos de struct/tabela, upvalues, globais
>   (load/store), construtores (`NEW_STRUCT[_LOCAL]`/`NEW_TABLE`/`NEW_MAP`/arrays),
>   `CONCAT`, `RELEASE`, **for-in** (array inline), **closures** (`CLOSURE`/`CLOSE_UP`),
>   **canais e goroutines** (`CHAN_SEND/RECV/CLOSE`, `GO` — inclusive suspendendo a
>   goroutine a partir do código nativo), `DEL`, `GET_TABLE2` e return. Fora do
>   subconjunto (bailout): `SELECT_*`, `DEFER`, `TAIL_CALL`, `VARARG`, `CALL_MULTI`.
> - **Inline caches** em `LOAD_GLOBAL`/`GET_FIELD` (interpretador e JIT).
> - **Ganhos medidos** (`bin/sea-jit --jit` vs `bin/sea`): `loops` 3,35×, `sort`
>   3,3×, `hot_float` 2,9×, `hot_calls` 2,3×, `matrix_mul` 2,1×, `mandelbrot` 2,0×,
>   `fib_recursive` 2,0×, `quicksort` 1,44×, `binary_trees` 1,17×. Programas mais
>   curtos que uma compilação (~30–50 ms: `primes`, `fib_iterative`, `closures`,
>   `string_ops`) ficam em ≈1× — não regridem mais.
> - **Variáveis de ambiente:** `SEA_JIT_DEBUG=1` (loga compilações e por que um
>   laço ficou interpretado), `SEA_JIT_SYNC=1` (compila inline, determinístico),
>   `SEA_JIT_INPROC=1` (sem servidor), `SEA_JIT_STRICT=1` (erro de codegen aborta
>   em vez de cair no interpretador), `SEA_JIT_HOT_CALLS` / `SEA_JIT_HOT_LOOPS`
>   (thresholds), `SEA_JIT_OPT` (nível de otimização do libgccjit, default 1).
> - **Testes:** `make test-jit` roda a suíte com todo chunk compilado na 1ª
>   chamada/back-edge, em modo sync e async, com `SEA_JIT_STRICT=1`.

Este documento descreve como adicionar um compilador JIT opcional ao Sea VM. O VM
hoje é um interpretador bytecode puro; o JIT compila funções "quentes" para código
nativo, mantendo total compatibilidade com o interpretador (que continua sendo o
caminho default e o fallback).

---

## 1. Visão geral e objetivo

- **Opt-in:** o JIT nasce **desligado**. Sem `--jit`, o binário se comporta
  exatamente como hoje — custo praticamente zero no hot path (apenas um teste de
  flag previsível pelo branch predictor).
- **Estratégia:** *method JIT* — a unidade de compilação é uma função inteira
  (`SeaChunk`). Quando uma função é chamada muitas vezes (fica "quente"), ela é
  compilada para nativo e as próximas chamadas pulam o interpretador. **Não** é um
  tracing JIT.
- **Backend:** [libgccjit](https://gcc.gnu.org/onlinedocs/jit/) — biblioteca de
  geração de código do GCC. Geramos o equivalente em C de cada opcode via a API da
  libgccjit e deixamos o GCC otimizar/emitir o nativo.
- **Não-objetivos da v1:**
  - deoptimização / OSR (on-stack replacement) no meio de um loop;
  - inline de chamadas entre funções compiladas;
  - recompilação especulativa com type feedback.
  Esses ficam para fases futuras.

---

## 2. Arquitetura atual relevante

Contexto necessário para entender o design (referências de arquivo/linha do estado
atual do código):

- **Pipeline:** lexer → parser → compiler → `SeaChunk` (bytecode 32-bit) → VM.
- **Formato de instrução:** cada instrução é um `uint32_t`. Formatos ABC / ABx /
  AsBx, com macros `SEA_INSTR_A/B/C/Bx/sBx` e `SEA_MAKE_ABC/ABx/AsBx`
  (`src/sea_opcode.h`). São **110 opcodes** (`OP_COUNT`).
  - ABC: `[C:8][B:8][A:8][OP:8]`.
  - ABx: `A:8` + `Bx:16` (constante sem sinal).
  - AsBx: `A:8` + `sBx:16` (offset com sinal, viés 32767) — usado por saltos e
    `LOAD_INT`.
- **Loop do VM:** `static int vm_run(SeaVM *vm, int base_frame)` em
  `src/sea_vm.c:583`. Dispatch por computed-goto (macro `DISPATCH`,
  `dispatch_table[OP_COUNT]`). `base` (register file da frame) e `ip` são pinados em
  `r13`/`r14`. `frame->ip` aponta para a **próxima** instrução, então lookups de
  linha usam `ip - 1`.
- **`SeaCallFrame`** (`src/sea_vm.h:15`): `closure`, `ip`, `base`, `expected_ret`,
  `varargs`, `vararg_count`, `defer_base`, `arena_mark`.
- **Fast-path de `OP_CALL`** (`src/sea_vm.c`, região ~1582): aplica-se quando a
  callee não é vararg, a aridade bate exatamente e há espaço na pilha de frames.
  Empilha a frame sem checagens e faz `DISPATCH()`. Os argumentos já estão em posição
  porque `nf->base = &base[a + 1]`.
- **Retorno** (`OP_RETURN` / `OP_RETURN0`, ~`src/sea_vm.c:1758`): roda defers, libera
  o arena da frame (`frame_arena_used = frame->arena_mark`), faz pop e copia os
  valores de retorno do `base[a..]` da callee para `caller->base[fn_reg..]`, onde
  `fn_reg` vem da instrução `OP_CALL` do caller (`caller->ip[-1]`).
- **Valores NaN-boxed** (`src/sea_value.h`): nil / bool / int de 48 bits / ponteiro
  de objeto codificados no espaço quiet-NaN; doubles ocupam o espaço não-NaN.
  Checagens (`sea_is_int`, `sea_is_obj`, `sea_is_number`) são mask+compare sem branch;
  acessos (`sea_as_int` via shift aritmético, `sea_as_number` via bit-cast) são
  branchless. Construtores: `sea_int_val`, `sea_number_val`, `sea_obj_val`,
  `sea_bool_val`.
- **`SeaChunk`** (`src/sea_chunk.h:14`): `code` (`uint32_t*`), `code_len`,
  `constants`, `protos` (protótipos aninhados p/ `OP_CLOSURE`), `lines`, `arity`,
  `is_vararg`, `max_stack`, `upvals`.
- **Modelo de memória:** objetos são reference-counted (sem GC). O **register file
  não participa de RC** — valores fluem pelos registradores sem retain/release; só
  operações de container retêm. O JIT precisa respeitar exatamente isso.

---

## 3. Design do JIT

### 3.1 Unidade de compilação

Uma `SeaChunk` (uma função). Cada chunk passa a carregar:

- `void *jit_fn;` — ponteiro para a função nativa compilada, ou `NULL` (não
  compilada), ou `(void*)-1` (tentou compilar e recusou — não tentar de novo).
- `uint32_t exec_count;` — contador de hotness.

### 3.2 ABI da função compilada

Assinatura única para todo código gerado:

```c
typedef int (*SeaJittedFn)(SeaVM *vm, SeaCallFrame *frame);
```

A função nativa lê e escreve **diretamente** em `frame->base[...]` — o mesmo register
file plano que o interpretador usa. Ela retorna um código:

- `SEA_OK` — a função terminou (executou seu `OP_RETURN`); os valores de retorno já
  estão em `frame->base[...]` como o interpretador deixaria.
- `SEA_JIT_BAILOUT` — encontrou um opcode não suportado; gravou em `frame->ip` o
  ponto de retomada e devolveu o controle ao interpretador (ver §3.5).

### 3.3 Gatilho de hotness

> **Atualização (Fase 3e):** além do contador de chamadas, há um contador de
> **back-edges** por chunk (`jit_hot`, decrementado em `JIT_BACKEDGE()` após todo
> salto para trás tomado no interpretador — `JMP` negativo e `FOR_LOOP*`). Ao
> cruzar o threshold, o chunk é **enfileirado** (`sea_jit_request`) em vez de
> compilado na hora; `jit_fn` vale `SEA_JIT_PENDING` enquanto isso. O texto
> abaixo descreve o desenho original (compilação síncrona no `OP_CALL`).

- Novo campo `exec_count` na `SeaChunk`, incrementado no **fast-path de
  `OP_CALL` / `OP_CALL_SELF`**, somente quando `vm->jit_enabled`.
- Ao cruzar `SEA_JIT_THRESHOLD` (valor inicial sugerido: `50`) e com
  `jit_fn == NULL`, chama-se `sea_jit_compile(vm, chunk)`. O resultado é cacheado em
  `chunk->jit_fn`:
  - sucesso → ponteiro válido;
  - recusa/erro → `(void*)-1`, para nunca mais tentar.

### 3.4 Despacho para o nativo

> **Atualização (Fase 3e):** a função gerada começa com um `switch` em
> `frame->ip`: `chunk->code` → bloco 0; cabeça de laço limpo → aquele bloco
> (OSR); qualquer outro ip → bailout imediato sem efeitos. No interpretador, o
> caminho lento do back-edge (`jit_osr` em `vm_run`) chama o nativo com o ip da
> cabeça do laço; se o nativo chega ao `RETURN`, a VM termina a chamada pelo
> próprio `lbl_RETURN` com uma instrução sintética `RETURN A=0 B=nret` (defers,
> fechamento de upvalues e cópia para o chamador ficam no caminho de sempre).
> Após um bailout, a re-entrada só é tentada de novo depois de 64 back-edges.

No fast-path de `OP_CALL` / `OP_CALL_SELF`, **depois** de montar a frame da callee
(`nf`) e **antes** do `DISPATCH()`:

```c
if (vm->jit_enabled) {
    void *jf = callee_chunk->jit_fn;
    if (jf == NULL && ++callee_chunk->exec_count >= SEA_JIT_THRESHOLD)
        jf = sea_jit_compile(vm, callee_chunk);   /* preenche jit_fn */
    if (jf && jf != (void*)-1) {
        int rc = ((SeaJittedFn)jf)(vm, nf);
        if (rc == SEA_OK) {
            /* pop + copiar retornos: mesma lógica do OP_RETURN */
            ...
            DISPATCH();
        }
        /* rc == SEA_JIT_BAILOUT: continuar interpretando nf a partir de nf->ip */
    }
}
/* caminho interpretado normal */
```

Quando `jit_enabled == false`, nada disso ocorre: nem hotness é contada, nem há
compilação — comportamento idêntico ao de hoje.

### 3.5 Geração de código com libgccjit (opcode → C-equivalente)

`sea_jit_compile` cria um `gcc_jit_context` por compilação e emite, para cada
instrução de `chunk->code`, o trecho equivalente ao handler do interpretador.

- **Registradores:** `R[n]` mapeia para o lvalue `frame->base[n]` (acessos via
  `gcc_jit_context_new_array_access` sobre um campo `base` de tipo `uint64_t*`).
- **Macros NaN-box:** `sea_is_int`, `sea_as_int`, `sea_int_val`, `sea_is_number`,
  `sea_as_number`, etc. são **reimplementadas como expressões libgccjit** sobre
  `uint64_t`, espelhando bit-a-bit `src/sea_value.h`.
- **Blocos básicos:** antes de emitir, varre-se o bytecode coletando todos os alvos
  de salto (destinos sBx de `JMP*`, `FOR_LOOP*`). Cria-se um `gcc_jit_block` por
  alvo e encadeia-se o fluxo; saltos viram `gcc_jit_block_end_with_jump` /
  `_end_with_conditional`.

**Subconjunto de opcodes da v1** (quente e sem alocação/RC):

| Categoria   | Opcodes                                                                 |
|-------------|------------------------------------------------------------------------|
| Loads/move  | `LOAD_NIL`, `LOAD_TRUE`, `LOAD_FALSE`, `LOAD_INT`, `LOAD_K`, `MOVE`     |
| Aritmética  | `ADD/SUB/MUL/IDIV/MOD` (int fast-path), `*_INT`, `IIADD/IISUB`, `ADDI/SUBI`, `NEG` |
| Comparações | `EQ/NEQ/LT/LE/GT/GE`, `LTI/LEI/GTI/GEI`, `NOT`                          |
| Saltos      | `JMP`, `JMP_TRUE`, `JMP_FALSE`, `JMP_NIL`                               |
| Loops       | `FOR_PREP`, `FOR_LOOP`, `FOR_LOOP_STEP1`, `FOR_LOOP_INT`                |
| Retorno     | `RETURN`, `RETURN0`                                                     |

### 3.6 Bailout (fallback seguro)

Qualquer opcode **fora do subconjunto suportado** faz a função nativa gravar em
`frame->ip` o endereço da instrução não suportada e retornar `SEA_JIT_BAILOUT`. O
interpretador (`vm_run`) retoma a execução daquela frame a partir desse `ip`. Isso
permite **cobertura incremental** de opcodes sem precisar suportar todos de uma vez.

Casos que sempre fazem bailout na v1: chamadas (`OP_CALL*`), canais/`go`,
`defer`/`panic`/`recover`, `select`, varargs, e qualquer opcode que mexa em RC
(`NEW_TABLE`, `SET_FIELD`, `NEW_ARRAY`, `RELEASE`, …).

> **Alternativa mais simples para a primeira iteração (Fase 1):** em vez da máquina
> de bailout no meio da função, **recusar compilar** qualquer chunk que contenha um
> opcode não suportado (`jit_fn = (void*)-1`). Funções compiladas são então 100%
> nativas; funções mistas continuam 100% interpretadas. A máquina de bailout entra
> na Fase 2.

### 3.7 Memória e reference counting

O código gerado segue exatamente o modelo do interpretador: o register file não
participa de RC. Como todos os opcodes que alocam/retêm/liberam vão para bailout na
v1, o JIT inicial cobre apenas caminhos aritméticos e de controle **sem alocação**,
o que elimina qualquer risco de erro de RC no código gerado.

---

## 4. Ativação por flag (`--jit`, off por default)

Hoje os argumentos do `sea` são posicionais (`src/sea_main.c:281`): `argv[1]` é o
subcomando (`run`, `compile`, `bytecode`, `version`) ou um arquivo `.sea`/`.seac`.

**Pré-passo de flags globais.** Antes do dispatch do subcomando, varre-se `argv` em
busca de flags globais, reconhecendo `--jit`, setando `static bool g_jit_enabled` e
compactando o vetor (removendo a flag) para que o restante do parsing continue
inalterado. Formas aceitas:

```sh
sea --jit run app.sea
sea --jit app.sea
```

**Propagação para o VM.** Novo campo `bool jit_enabled;` em `struct SeaVM`
(`src/sea_vm.h:66`). Como `sea_vm_new()` faz `memset(vm, 0, ...)`, o default é
`false` automaticamente. Em `cmd_run` (e `cmd_repl`), após `sea_vm_new()`:

```c
vm->jit_enabled = g_jit_enabled;
```

**Sem `--jit`:** `jit_enabled == false` → nenhuma compilação, hotness não contada,
comportamento bit-a-bit idêntico ao de hoje.

---

## 5. Arquivos novos / modificados

| Arquivo                | Mudança                                                                                                 |
|------------------------|---------------------------------------------------------------------------------------------------------|
| `src/sea_jit.h` (novo) | `SeaJittedFn`, `sea_jit_compile(SeaVM*, SeaChunk*)`, `SEA_JIT_THRESHOLD`, `SEA_JIT_BAILOUT`.            |
| `src/sea_jit.c` (novo) | Implementação libgccjit. Pego automaticamente pelo `wildcard` do Makefile. Todo o corpo sob `#ifdef SEA_JIT`. |
| `src/sea_chunk.h`      | Campos `void *jit_fn;` e `uint32_t exec_count;` na `SeaChunk`; liberar `gcc_jit_result` em `sea_chunk_free`. |
| `src/sea_vm.h`         | Campo `bool jit_enabled;` na `SeaVM`.                                                                    |
| `src/sea_vm.c`         | Hotness + despacho para nativo no fast-path de `OP_CALL`/`OP_CALL_SELF`; retomada de bailout em `vm_run`. |
| `src/sea_main.c`       | Pré-passo de flags globais (`--jit`) + propagação para `vm->jit_enabled`.                                |
| `Makefile`             | Target `make jit` (`CFLAGS += -DSEA_JIT`, `LDFLAGS += -lgccjit`). Build default não usa libgccjit.       |
| `docs/reference.md`    | Documentar a flag `--jit` (regra do CLAUDE.md de manter o reference atualizado quando a CLI muda).       |

**Sobre o Makefile / dependência opcional.** Todo o código JIT fica atrás de
`#ifdef SEA_JIT`. No build default (`make`), `SEA_JIT` **não** é definido: a
libgccjit não é exigida, e passar `--jit` apenas imprime um aviso
("JIT not built in; running interpreted") e segue interpretado. Com `make jit`,
define-se `-DSEA_JIT` e linka-se `-lgccjit`, habilitando o JIT real.

---

## 6. Fases de entrega

- **Fase 0 — andaime (FEITA):** flag `--jit`, campo `jit_enabled` no VM,
  `sea_jit.{h,c}` sob `#ifdef SEA_JIT`, target `make jit`. Verificado: `make test`
  sem regressão; `make jit` linka.
- **Fase 1 — núcleo aritmético/controle (FEITA):** codegen libgccjit para o
  subconjunto da §3.5, com **recusa-de-chunk** para qualquer opcode fora da lista
  **e** *bailout por tipo* já implementado dentro das ops suportadas (o caminho
  lento volta ao interpretador via `frame->ip` + `SEA_JIT_BAILOUT`). Detalhe
  importante: `OP_ADD/SUB/MUL` inlinam **int+int e number+number** (espelhando
  `val_add`/`val_sub`/`val_mul`); só o resto (mistos, string, erro) faz bailout —
  sem isso um laço `s = s + i` (ambos int, mas compilado como `OP_ADD`) bailaria a
  cada iteração. Verificado: 50/50 na suíte oficial sob `--jit`; ~2,4× num laço
  numérico quente; saída idêntica ao interpretado no script de correção
  (incl. caminho de bailout int→float).
- **Fase 2 — bailout por opcode + JIT do chunk principal (FEITA):** trocada a
  recusa-de-chunk por *bailout por opcode não suportado* (o chunk só é recusado se
  o opcode não suportado estiver **dentro de um laço**, ou se for straight-line com
  op não suportado — ver `chunk_compilable`). Adicionada **análise de
  alcançabilidade** (`compute_reachable`) para não criar blocos mortos após um
  bailout (libgccjit rejeita blocos inalcançáveis). E o **chunk principal** passou
  a ser compilado em `sea_vm_exec` (não é alcançado pelo hook de `OP_CALL`).
  Resultado: laços top-level aceleram — `loops.sea` 0,26s→0,09s (~2,9×). Verificado
  50/50 na suíte sob `--jit`. Limitação conhecida: um opcode não suportado **antes**
  do laço quente faz bailout antes de chegar ao laço (sem ganho, mas correto).
- **Fase 3a — chamadas no JIT (FEITA):** `OP_CALL`/`OP_CALL_SELF` agora compilam,
  delegando a helpers C (`sea_jit_call`/`sea_jit_call_self` em `sea_vm.c`) que
  espelham o fast-path do interpretador: empilham o frame em `base[a+1]`, rodam o
  callee (seu próprio código nativo se quente, senão `vm_run` re-entrante) e
  copiam o retorno. Casos fora do escopo (vararg, aridade≠, >1 retorno, native sem
  resultado único, pilha cheia) retornam 1 → o JIT baila naquele opcode. Seguro:
  erros usam `exit()` e panics são flag-based (sem longjmp atravessando os helpers).
  Verificado: `fib_recursive` ~2,2×; suíte 50/50. Limitação: chunks com arrays/
  tabelas/closures no corpo ainda são recusados (binary_trees, quicksort, closures).
- **Fase 3b — arrays + MOD (FEITA):** `OP_GET_TABLE`/`OP_SET_TABLE` inline (fast-path
  de array: `is_obj`+`is_int`, `type==OBJ_ARRAY`, `0<=idx<len`, load/store sem RC —
  o mesmo que o interpretador faz; maps/structs/strings/OOB bailam). `ARRAY_PUSH`,
  `NEW_ARRAY`, `NEW_ARRAY_LOCAL` delegados a helpers C (`sea_jit_apush`/`new_array`/
  `new_array_local`). `OP_MOD_INT` inline (int path, zero-check baila, módulo floored).
  `compute_reachable` cobre todos como straight-line. Para evitar regressões,
  `chunk_compilable` também **recusa chunks com chamadas dentro de um laço** (a
  chamada re-entrante por iteração é mais lenta que o call inline do interpretador
  quando o callee é interpretado; um callee quente é JITado por conta própria e
  roda nativo a partir do laço interpretado). Resultados: `matrix_mul` ~1,8×
  (1,47→0,73s), `quicksort` ~+8%, `closures` neutro (recusado pela heurística de
  call-in-loop). Trade-offs medidos remanescentes: `sort` ~−6% (a guarda de array
  não supera o `GET_TABLE` justo do interpretador em laço access-bound) e
  `primes`/`fib_iterative` regridem (programas de ~30ms onde a latência de
  compilação one-shot domina — sem sinal estático de trip-count para evitar; custo
  de warmup, amortizado em qualquer execução >~100ms). Default `SEA_JIT_OPT=1`
  minimiza a latência. Verificado 50/50 (default e `--jit`).
- **Fase 3c — comparações e divisão float (FEITA):** `OP_LT/LE/GT/GE` ganharam o
  caminho **number** (antes int-only): int→int, senão number→float, senão bail.
  Adicionado `OP_DIV` (divisão float; both-int e both-number coagem para double,
  sem zero-check pois x/0=inf; mixed/string bailam). Desbloqueia `mandelbrot`
  (~1,95×: 0,37→0,19s — o laço de escape usa `>` float e os externos usam `/`).
  Verificado 50/50 (default e `--jit`).
- **Fase 3d — JIT em goroutines + segurança de suspensão (FEITA):** `jit_enabled`
  passou a viver em `SeaShared` (estado programa-wide), então as VMs filhas de
  goroutine o **herdam** e código concorrente também JITa. Segurança: canais só
  fazem *parking* (`SEA_SUSPENDED`) dentro de goroutine (na thread principal
  bloqueiam a thread). Como o `vm_run` re-entrante de `sea_jit_call` não consegue
  retomar através do frame nativo JITado, `sea_jit_call`/`sea_jit_call_self`
  **bailam quando `vm->goroutine != NULL`** — aí o interpretador faz a chamada
  inline, onde a suspensão é parte do próprio `vm_run` da goroutine (resume
  correto pelo scheduler). Verificado: função JITada numa goroutine chamando uma
  função que bloqueia em canal → saída correta, sem hang; suíte 50/50 sob `--jit`
  com todos os testes de concorrência (green_stress, canais, select).
- **Fase 3e — compilação assíncrona, OSR e cobertura (FEITA, out/2026):**
  - **Latência fora do caminho crítico.** Medido: cada compilação custa ~45 ms
    fixos (init do GCC + `as` + `ld` externos) mais ~1 ms/instrução; não há como
    evitar no libgccjit 13. Antes, `primes` (34 ms interpretado) ia a 87 ms com
    `--jit`. Agora: (1) **processo servidor** de compilação, forkado em
    `sea_jit_init()` antes de qualquer thread — recebe bytecode+constantes por
    `socketpair`, gera um `.so` (`gcc_jit_context_compile_to_file`) e a VM faz
    `dlopen`; (2) **thread worker** com fila FIFO (`chunk->jit_next`); (3) no
    exit a VM não espera — o servidor vê EOF, termina e apaga seu diretório
    temporário; um chunk liberado no meio da compilação é **abandonado** (o
    worker descarta o resultado sem tocá-lo); (4) a **libgccjit é carregada com
    `dlopen`** (tabela de ponteiros `GCCJIT_API`) — só o servidor a carrega,
    economizando ~3,5 ms de startup do `bin/sea-jit`; (5) `-nostdlib` no link do
    `.so` (~10% de cada compilação). Resultado: `primes` 0,39×→0,94×,
    `fib_iterative` 0,56×→0,97×.
  - **OSR + contador de back-edges** (§3.3/§3.4): compila funções chamadas uma
    vez com laço quente, e laços quentes que vêm depois de um op não suportado.
  - **Plano por laço** (`plan_chunk`): só laços limpos ganham entrada OSR; um laço
    limpo aninhado num laço sujo roda nativo. Laços com **CALL** passam a
    compilar: `sea_jit_call` executa nativamente callees nativos (C ou JITados) e
    faz bailout para callees interpretados (o interpretador faz a chamada inline,
    mais rápida que `vm_run` re-entrante); quando o callee compila, o laço fica
    100% nativo.
  - **Novos opcodes:** `MOD`/`MOD_INT`/`IDIV`/`IDIV_INT` com caminho float
    (`//` entre ints trunca, como o interpretador), `POW`, `NEG` float, bitwise
    (`BAND/BOR/BXOR/BNOT/SHL/SHR`) inline; via helpers C: `GET_FIELD`,
    `SET_FIELD`, `SET_FIELD_BORROW`, `LOAD_UP`/`STORE_UP`, `STORE_GLOBAL`,
    `CONCAT` (corpo extraído para `vm_concat`, `always_inline` no interpretador),
    `RELEASE`, `NEW_STRUCT`/`NEW_STRUCT_LOCAL`/`NEW_TABLE`/`NEW_MAP`. Helpers que
    podem lançar erro recebem `frame->ip` salvo antes (linha certa no erro).
  - **Lição medida:** o caminho lento de `GET_TABLE`/`SET_TABLE` continua sendo
    **bailout**. Tentamos um helper `table_get`/`table_set` que volta para o laço
    (cobriria tabelas/maps em laços quentes), mas um helper que escreve em `base[]`
    num caminho que retorna ao laço obriga o GCC a recarregar todos os
    registradores da VM da memória a cada iteração: `sort` caiu de 3,3× para
    2,35× e `matrix_mul` de 2,1× para 1,8×. Revertido.
  - **Testes:** `make test-jit` (sync + async, `SEA_JIT_STRICT=1`);
    `95_jit_osr` (saída por RETURN sintético, múltiplos retornos, defer, laço
    limpo em laço sujo, troca de tipo no meio do laço) e `96_jit_arith` (casos-
    limite de mod/idiv/pow/neg/bitwise/shift).
  - **Ainda não feito:** inline cache de globais (hoje `LOAD_GLOBAL` é um helper
    com lookup em tabela), hoisting da guarda de tipo de arrays para fora do laço,
    `FOR_IN_*`, canais/select, `CLOSURE`, `VARARG`, `TAIL_CALL`.

- **Fase 3f — for-in, closures, canais e inline caches (FEITA, out/2026):**
  medido em testes + exemplos + libs/llm: laços que ficavam interpretados caíram
  de **98 para 3** (sobram 2 `SELECT` e 1 `DEFER` em laço).
  - **for-in:** corpo do `FOR_IN_NEXT` extraído para `vm_for_in_next` (always_inline,
    compartilhado com o helper); no JIT o caso **array** é inline (cursor/índice/
    elemento sem chamada): for-in sobre array 1,93×.
  - **Canais com suspensão nativa:** dentro de goroutine o nativo nunca está
    aninhado (`sea_jit_call` baila em goroutines), então um helper de canal que
    estaciona a goroutine grava `frame->ip` = ponto de retomada e devolve
    `SEA_JIT_HELPER_SUSPEND`; o código gerado retorna `SEA_JIT_SUSPEND` e o `vm_run`
    devolve `SEA_SUSPENDED` (em `jit_osr` e em `JIT_TRY_CALL`). Helpers:
    `CHAN_SEND`, `CHAN_RECV`, for-in sobre canal; `CHAN_CLOSE` e `GO` sem suspensão.
  - **Closures:** `CLOSURE`/`CLOSE_UP` via helper; retornos nativos fecham upvalues
    abertos. **Bug corrigido** (pré-existente no caminho OSR): o `RETURN` nativo
    copiava os resultados para `base[0..]` *antes* de fechar upvalues, então uma
    closure capturava o valor de retorno no lugar da local. Agora o `RETURN` de
    chunks com `OP_CLOSURE` fecha os upvalues antes da cópia.
  - **`DEL` e `GET_TABLE2`** via helpers (corpos compartilhados com o interpretador).
  - **Inline caches** (`SeaTableIC`, `sea_table_get_ic`): um slot por instrução em
    `chunk->ics` (alocado sob demanda com CAS); guarda `entries` + índice e valida
    contra a tabela atual (resize, delete, tabela diferente e update rasgado entre
    threads só causam miss). O endereço dos slots viaja na requisição ao servidor
    de compilação. Também `key_equal` ganhou o atalho `a == b` e `GET_FIELD` em
    tabela pula a cadeia genérica `table_get`. Resultado: `LOAD_GLOBAL` 19→4 ns,
    `GET_FIELD` 29→7 ns; um laço com `math.abs(...)` 419→198 ms (interpretador) e
    398→158 ms (JIT).
  - **Testes:** `99_jit_coverage` (for-in de todos os containers, closures em laço,
    o caso do RETURN, produtor/consumidor e ping-pong por canal, `go` em laço) e
    `100_inline_cache` (invalidação do IC).
- **Type feedback para acesso a containers (out/2026):** o `GET_TABLE`/`SET_TABLE`
  genérico só tinha caminho inline para array genérico. Um `float_array`/`int_array`
  recebido por parâmetro ou upvalue (o compilador não vê o tipo) dava bail a cada
  acesso, e o `examples/nbody` não ganhava nada com o JIT. Qualquer caminho alternativo
  que volte ao laço (inline ou helper) custava ao caminho rápido do array genérico
  (sort 4,3×→3,2×). Solução: o interpretador anota, no slot de IC da instrução
  (`SEA_FB_TYPED_ARRAY` / `SEA_FB_OTHER`), o que passou por ali no caminho lento; o JIT
  só emite o caminho tipado inline ou o helper `table_get`/`table_set` nos sites que
  precisaram (o feedback viaja na requisição ao servidor). nbody 1,0×→1,8×, sort
  inalterado. O `make test-jit` ganhou uma 3ª passada com thresholds padrão, para
  compilar com feedback já coletado.
- **Fase 4 — promoção de registradores (FEITA, out/2026):** dentro de chunks com
  laço, cada registrador da VM que nenhuma closure captura vive numa local do
  código nativo (`J->L[n]`); `frame->base[n]` só é atualizado nos pontos de
  sincronização:
  - **entrada:** por chamada, carrega só os parâmetros (`R[0..arity-1]`; os demais
    começam nil); por OSR, carrega todos;
  - **helpers:** `op_regs()` declara, por opcode, quais registradores o helper lê
    (gravados antes da chamada) e escreve (recarregados depois). `CALL` lê fn +
    args e escreve o resultado; os registradores acima de A são temporários mortos;
  - **bailout:** um único bloco de saída compartilhado grava tudo de volta (um
    spill-all por guarda inchava o código e piorava a alocação de registradores do
    GCC: mandelbrot 2,04×→1,90× antes do bloco compartilhado, 2,28× depois);
  - **suspensão:** grava tudo de volta antes de retornar `SEA_JIT_SUSPEND`;
  - **return:** escreve os resultados direto em `base[0..]`.
  Registradores capturados (upvalue aberto aponta para `base[r]`) ficam sempre em
  memória; o conjunto vem dos descritores de upvalue dos protos e viaja na
  requisição ao servidor (com `max_stack` e a aridade). Chunks **sem laço** não
  promovem: cada op roda uma vez por chamada e as sincronizações não se pagam
  (fib_recursive ficava ~7% mais lento).
  Ganhos sobre a Fase 3: sort 3,28→4,41×, hot_float 2,84→3,51×, hot_calls
  2,12→2,70×, matrix_mul 2,14→2,34×, mandelbrot 2,04→2,28×, quicksort 1,40→1,50×.
  Teste: `101_jit_regpromo` (upvalue alterado por closure dentro do laço, bail no
  meio do laço, helper que escreve dois registradores, chamadas em laço, campos e
  globais, laços aninhados com muitos registradores vivos).
  O nível de otimização 2 continua pior que o 1 (compila mais devagar e o código
  nativo chega mais tarde).

> **Nota sobre a numeração:** a §3.6 previa o *bailout* como Fase 2; o bailout
> **por tipo** (dentro das ops) entrou já na Fase 1, e o bailout **por opcode não
> suportado** + JIT do chunk principal na Fase 2. O que resta (Fase 3) é
> **ampliar o conjunto de opcodes** — em especial chamadas e arrays.

---

## 7. Riscos e mitigação

- **libgccjit é dependência pesada/opcional** → isolada por `#ifdef SEA_JIT` e
  target `make jit` separado; build e testes default não a exigem.
- **Divergência semântica** entre handler interpretado e código gerado → cada opcode
  suportado ganha teste comparando saída JIT vs. interpretada.
- **Latência de compilação** (medida: ~45 ms fixos + ~1 ms/instrução) → feita em
  background por um processo servidor; o interpretador nunca espera (Fase 3e).
- **Custo no hot path com `--jit` off** → um único teste de `jit_enabled`/`jit_fn`,
  previsto pelo branch predictor; efetivamente zero.

---

## 8. Verificação

- `make test` e `make test-asan` **sem** `--jit`: zero regressão (default off).
- `make jit` e rodar a suíte passando `--jit`: saída idêntica à do modo
  interpretado (diff contra `tests/unit/*.expected`).
- Microbenchmark de loop quente (ex.: soma iterativa / fib iterativo): interpretado
  vs. `--jit`, confirmando ganho de desempenho.
- Build **sem** `SEA_JIT` + `--jit`: deve avisar ("JIT not built in") e rodar
  interpretado normalmente.
- `make test-jit`: suíte com `SEA_JIT_HOT_CALLS=1 SEA_JIT_HOT_LOOPS=1
  SEA_JIT_STRICT=1`, em modo sync e async (o runner tem timeout por teste,
  `TEST_TIMEOUT`, default 60 s).
- `SEA_JIT_DEBUG=1 bin/sea-jit --jit prog.sea` mostra o que compilou (tempo,
  servidor/in-process) e, para cada laço que ficou interpretado, quais opcodes
  impediram.
