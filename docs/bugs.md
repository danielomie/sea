# Sea VM — Bugs Conhecidos

Bugs encontrados em uso real. Bugs marcados **(FIXED)** já foram corrigidos no código-fonte.

---

## Bug 1: `os.exec` retornava `nil` para comandos de 65+ caracteres **(FIXED 2026-10)**

O tamanho em si não era o problema: um **literal** longo funcionava. O que falhava era uma string longa montada com `..`. Resultados de concatenação com mais de 64 bytes viram ropes (`OBJ_ROPE`), e `os.exec`, como outras 23 funções das libs built-in, testa `sea_is_string(args[0])`, que só aceita `OBJ_STRING`. O rope era rejeitado e a função retornava `nil` em silêncio. É a mesma raiz do Bug 4.

**Correção (central, não por função):**
- **Chamadas de funções nativas** (`call_native` em `sea_vm.c`, usado pelo interpretador, `sea_vm_call` e o JIT): se algum argumento é rope, a função recebe uma cópia dos argumentos com o rope trocado pela sua versão achatada. A versão achatada fica em cache no próprio rope (`sea_rope_flat`, publicada com CAS, invalidada no append in-place e liberada junto com o rope).
- **A mesma raiz em outros lugares**, encontrada no teste de regressão:
  - rope como **chave** de tabela ou map não era encontrado: agora é normalizado em `sea_table_*`/`sea_map_*`, e em `table_get`/`table_set` da VM (structs);
  - `==`/`~=` entre ropes de mesmo conteúdo davam `false`;
  - `<`/`<=`/`>`/`>=` com rope davam erro de execução ("attempt to compare string with string").
- **De passagem:** em `sea_table_set`, sobrescrever uma chave existente gravava o novo objeto-chave sem `retain` (vazava o antigo e guardava um ponteiro sem dono). Agora a chave armazenada é mantida.

**Custo:** ~1,6% no `map_ops` (10M operações de map; medido A/B). Os demais benchmarks não mudaram.

**Teste de regressão:** `tests/unit/98_rope_strings.sea`.

---

## Bug 2: `for-in` entra em loop infinito quando há contador numérico declarado antes do loop **(FIXED)**

```sea
-- Padrão que causava loop infinito (agora funciona corretamente):
local n = 0
for k, v in m do
    n = n + 1
end
print(n)  -- imprime o número correto de entradas
```

**Causa original:** Em versões anteriores, o campo A da instrução `FOR_IN_NEXT` era armazenado em menos de 8 bits, causando truncamento quando havia muitos locais antes do loop. O registrador do cursor (`R[A+1]`) era lido da posição errada, reiniciando o iterador a cada chamada.

**Correção:** O campo A passou a usar 8 bits completos (0–255), suportando até 255 locais antes do loop sem truncamento. O comentário no compilador (`src/sea_compiler.c` linha 1657) documenta: _"Full 8-bit A field — no truncation, supports up to 255 locals before the loop."_

**Teste de regressão:** `tests/unit/10b_forin_counter.sea` cobre todos os padrões que antes causavam o problema.

---

## Bug 3: Variável externa perde conteúdo ao reutilizar local de loop **(FIXED 2026-05)**

```sea
-- Antes causava use-after-free (snapshot via nil ou conteúdo corrompido):
local snapshot = [=]
local i = 0
while i < 3 do
    local curr = [=]
    curr["val"] = i
    snapshot = curr   -- OK agora: ownership transferida para snapshot
    i = i + 1
end
print(snapshot["val"])  -- imprime 2 (correto)
```

**Causa original:** `snapshot = curr` emitia `OP_MOVE` sem incrementar RC. O compilador mantinha `reg_owned[curr]=1`, então `end_scope` do while emitia `OP_RELEASE curr` no fim de cada iteração — destruindo o objeto com RC=0. Na próxima iteração, `local curr = [=]` criava um novo mapa (potencialmente no mesmo endereço), e `snapshot` aparecia "vazio".

**Correção** (`src/sea_compiler.c`, `NODE_ASSIGN` fast path): ao detectar que o RHS é uma variável local owned, a correção transfere a propriedade RC do RHS para o dst — setando `reg_owned[dst]=1` e limpando `reg_owned[rhs]=0`. O `end_scope` do loop não emite mais `OP_RELEASE` para `curr`, e o objeto sobrevive até o `end_scope` de `snapshot`.

**Teste de regressão:** `tests/unit/42_forin_snapshot.sea`

---

## Bug 4: Strings rope (`OBJ_ROPE`) rejeitadas silenciosamente por funções nativas C **(FIXED 2026-10 — ver Bug 1)**

O operador `..` produz um objeto `OBJ_ROPE` (concatenação lazy) quando o resultado tem mais de 64 bytes. Funções nativas que verificam `sea_is_string(v)` aceitam apenas `OBJ_STRING`, ignorando silenciosamente ropes — retornam `nil` ou falham sem erro.

`type()` em Sea retorna `"string"` para ambos, ocultando a diferença.

**Causa:** As bibliotecas nativas foram escritas antes da otimização rope ser adicionada.

**Workaround em C:** Usar `sea_value_get_flat_string(v)` (de `sea_rope.h`) em vez de `sea_is_string(v)` + `SEA_AS_STRING(v)`. Lembrar de chamar `sea_obj_release(&s->obj)` depois, pois `sea_value_get_flat_string` retorna RC=1.

**Workaround em Sea:** Forçar achatamento antes de passar a função nativa: `s = string.replace(s, "", "")`.

Corrigido em: `libs/http/http_native.c`, `libs/json/json.c`.

**Atualização (2026-10):** resolvido de forma geral pela correção do Bug 1. Toda função nativa recebe ropes já achatados, então `sea_is_string`/`SEA_AS_STRING` funcionam com qualquer string. `sea_value_get_flat_string` continua válido para código existente, mas não é mais necessário em argumentos.

---

## Bug 5: `sync.mutex()` usa `acquire`/`release`, não `lock`/`unlock`

```sea
local mu = sync.mutex()
mu.acquire(mu)   -- correto
mu.release(mu)   -- correto

mu.lock(mu)      -- ERRO: "attempt to call a nil value"
mu.unlock(mu)    -- ERRO: "attempt to call a nil value"
```

**Causa:** O módulo `sync` adotou nomenclatura estilo Go. Não é um bug de implementação — é documentação ausente.

**Workaround:** Sempre usar `mu.acquire(mu)` e `mu.release(mu)`. Também disponível: `mu.try_lock(mu)`.

---

## Bug 6: `json.encode()` retornava `"null"` para maps `[=]` **(FIXED)**

`json.encode` só tratava `OBJ_TABLE` e `OBJ_ARRAY`. Um map `[=]` (`OBJ_MAP`) era codificado como `null`.

**Causa:** O encoder foi escrito antes de `OBJ_MAP` ser adicionado à VM.

**Correção:** `libs/json/json.c` — `encode_value` agora itera `OBJ_MAP` via `sea_map_next`. Nenhum workaround necessário.

---

## Bug 7: `string.join` descartava elementos rope silenciosamente **(FIXED 2026-05)**

`string.join(arr, sep)` apenas media e copiava elementos `OBJ_STRING`. Elementos que eram ropes (`OBJ_ROPE` — resultado de `..`) eram tratados como strings vazias, produzindo apenas separadores na saída.

**Causa:** A implementação em `src/sea_lib_string.c` antecedia a otimização rope.

**Correção:** Substituídas as duas passagens (medição de tamanho + cópia) para usar `sea_value_get_flat_string` por elemento. Nenhum workaround necessário após a correção.

---

## Bug 9: `for i = 0, n-1 do` entrava em loop infinito quando `n = 0` **(FIXED 2026-05)**

```sea
-- Quando n=0, `for i = 0, n-1 do` compilava para `for i = 0, -1 do`.
-- Antes da correção, o loop executava infinitamente.
-- Agora executa zero iterações, como esperado.
local n = 0
for i = 0, n-1 do
    print(i)  -- nunca executado quando n=0
end
```

**Causa original:** `OP_FOR_PREP` apenas copiava o contador inicial para a variável do loop (`R[A+3] = R[A]`) e caía diretamente no corpo — sem verificar se a condição inicial já era falsa. Com `start=0, limit=-1, step=1`, o corpo executava uma vez antes de `FOR_LOOP` ter chance de verificar `1 <= -1 → false`.

**Correção** (`src/sea_vm.c` e `src/sea_compiler.c`): `OP_FOR_PREP` agora funciona como salto condicional _forward_ (espelho de `FOR_LOOP`, que é salto condicional _backward_). Se a condição inicial falhar (`start > limit` com step positivo, ou `start < limit` com step negativo), salta diretamente para depois do loop, sem executar nenhuma iteração.

No compilador, `FOR_PREP` passou a ser emitido como `emit_jmp` (com `sBx` patchável), e o offset é preenchido após emitir `FOR_LOOP`, apontando para o fim do loop.

**Teste de regressão:** padrão `for i = 0, n-1 do` com `n=0` já coberto pelos testes existentes de loop numérico.

---

## Bug 8: Compilador trava/segfault em table literals com chaves nomeadas **(NÃO REPRODUZ desde 2026-10)**

> **Status (2026-10):** não reproduz. Um script gerado com mais de 1.000 linhas (150 funções, cada uma com literais `{ k = v, d = { inner = ..., deep = { k = ... } } }` aninhados, mais um literal passado direto como argumento de função) compila e dá o resultado correto, e todas as libs `libs/llm/*.sea` compilam. Provavelmente foi resolvido quando literais com chaves string passaram a compilar para `OBJ_STRUCT`. O workaround abaixo não é mais necessário. Se voltar a acontecer, guarde o script que trava.

```sea
-- CAUSA TRAVAMENTO DO COMPILADOR:
local cfg = {
    vocab_size = 8000,
    d_model    = 256,
    n_layers   = 4,
}

-- WORKAROUND: construir incrementalmente
local cfg = {}
cfg.vocab_size = 8000
cfg.d_model    = 256
cfg.n_layers   = 4
```

O compilador Sea trava (loop infinito) ou segfaults (exit 139) ao parsear construtores de tabela `{ chave = valor, ... }` com chaves nomeadas, especialmente em scripts grandes. Construtores de array `{val1, val2}` não são afetados.

**Causa:** Provável bug de recursão ou corrupção de estado no parser ao processar campos nomeados em table constructors.

**Workaround:** Nunca usar a sintaxe `{ key = val, ... }` em Sea. Construir tabelas incrementalmente: `t = {}` seguido de `t.key = val` por campo. Isso vale também para tabelas passadas diretamente como argumento de função — extrair para uma local antes de chamar.

---

## Bug 10: `defer f(args)` perdia o valor dos argumentos **(FIXED 2026-10)**

```sea
local function f(n)
    defer print("deferred")   -- imprimia "0", não "deferred"
    local s = 0
    return s
end
```

**Causa:** não era a VM (o `OP_DEFER` já copiava os valores na hora, com semântica de Go), e sim o otimizador do compilador. A *dead write elimination* não sabia que o `OP_DEFER` lê `R[A+1..A+B]`, então as cargas dos argumentos (`LOAD_K "deferred"`) viravam NOP como escritas mortas. O defer copiava o que estivesse no registrador; aqui, o valor de `s`. Correção: `emit_abc` marca `R[A..A+B]` como lidos para `OP_DEFER` (e `R[A]` para `FOR_IN_INIT`/`FOR_IN_NEXT`, que antes só funcionavam por coincidência).

**Encontrados junto:**
- `defer obj:m(args)` executava a chamada **na hora** (caía no ramo "adiar uma expressão"). Agora o método, o `self` e os argumentos são avaliados no `defer` e a chamada é adiada.
- Mais de 8 argumentos eram **truncados em silêncio** (limite fixo de `SeaDeferEntry`). Agora é erro de compilação (`SEA_DEFER_MAX_ARGS`, documentado em `docs/reference.md`).

**Teste de regressão:** `tests/unit/97_defer.sea`.

---

## Bug 11: `31_green_stress` travava ou dava use-after-free com goroutines **(FIXED 2026-10)**

O teste travava em ~45% das execuções (9/20 com `bin/sea`, sem JIT), e o ASAN acusava heap-use-after-free na string `"__wg_ptr"` dentro de `wg_done` → `sea_table_get_str`. Eram quatro defeitos de concorrência:

1. **Objetos alcançados por upvalue aberto nunca eram marcados como compartilhados.** `sea_obj_mark_shared` só seguia upvalues *fechados*. Uma goroutine que captura uma local ainda viva do pai (ex.: `local wg = sync.waitgroup()` no chunk principal) usa um upvalue aberto, então o waitgroup, as chaves dele, canais etc. ficavam com refcount não-atômico sendo alterado por várias threads. Incrementos se perdiam e o objeto era liberado em uso. Correção: marcar `*up->location` também para upvalues abertos (`sea_object.c`). Além disso, `STORE_UP` num upvalue compartilhado passa a marcar o valor gravado.
2. **Tabela de intern sem lock.** `g_interns` é global e é usada por natives em threads do escalonador. Agora ela é protegida por mutex, e strings internadas já nascem SHARED (RC atômico), porque são, por natureza, um único objeto visto por todas as threads (`sea_string.c`).
3. **Goroutine retomada por dois workers.** O ponto de retomada (`frame->ip = ip - 1`) era gravado *depois* de a goroutine entrar na fila de espera do canal. Além disso, `sea_sched_requeue` a colocava na fila de execução mesmo com o worker anterior ainda saindo do `vm_run` dela (`YIELD`/`select` se recolocavam na fila antes de retornar). Outro worker podia retomá-la com o IP antigo e pular o receive, o que causava o travamento. No send sem buffer, o `ip++` do receiver ainda podia ser sobrescrito, duplicando o valor. Correção: gravar o IP antes da operação no canal (`sea_vm.c`) e um handshake atômico de park (`park_state` em `SeaGoroutine`, `sea_sched.c`): entre o worker que termina de suspender e quem acorda, quem chega por último coloca na fila.
4. **Use-after-free na saída.** `run_source` liberava o chunk antes de esperar as goroutines que executam o bytecode dele. Agora faz `sea_sched_drain` antes do `sea_chunk_free` (`sea_main.c`).

Também: o teste do bit SHARED em retain/release e a publicação de upvalues fechados usam acessos atômicos (relaxed/acquire/release, de graça no x86).

**Verificação:** 0/300 falhas (150 com `bin/sea`, 150 com `bin/sea-jit --jit`); ASAN 0/30; ThreadSanitizer sem relatórios em `25_goroutines`, `26_channels`, `31_green_stress` (8×) e `34_select` (rodar com `setarch -R`, porque o TSAN não suporta a entropia de ASLR do kernel atual).

**Limitação (resolvida em 2026-10):** um objeto **novo** atribuído pelo pai a uma local já capturada por uma goroutine não era marcado como compartilhado. Agora o compilador emite `OP_MARK_CAPTURED` nessas atribuições (ver "Variável de laço compartilhada entre iterações").

---

## Bug 12: executar `.seac` dava segfault **(FIXED 2026-10)**

Era pior que o escalonador ausente: **qualquer** `.seac` com uma constante string dava segfault, até `print("hello")`, cujo nome `print` é uma constante. Por isso nenhum teste rodava como `.seac`.

**Causa:** `sea_chunk_write` gravava cada constante como o `SeaValue` cru de 64 bits. Para strings, isso é um ponteiro para a memória do processo que compilou, inválido em qualquer outro processo. Além disso, o caminho de execução de `.seac` em `sea_main.c` era uma cópia reduzida do `cmd_run`: sem escalonador de goroutines (`go` → `sea_sched_pool_get(NULL)`), sem `os.args` e sem drain; e `sea run x.seac` tentava parsear o binário como fonte.

**Correção:**
- Formato v2 (`SEA_BYTECODE_VERSION 2`): constantes com tag, sendo `RAW` para valores não-objeto e `STRING` com comprimento + bytes, re-internada na leitura. `sea_chunk_read` valida tamanhos e leituras e devolve NULL (liberando o que já leu) em arquivo truncado ou corrompido. Arquivos v1 são recusados com "invalid or outdated bytecode file … recompile".
- `.sea` e `.seac` agora passam pelo mesmo `cmd_run`, tanto via `sea arquivo.seac` quanto via `sea run arquivo.seac`.
- `sea compile` reporta erro de escrita em vez de deixar um arquivo parcial.

**Teste de regressão:** `make test-seac` (modo `SEAC=1` do `tests/run_tests.sh`) compila cada teste para `.seac` ao lado do fonte e executa: 52/52, inclusive sob ASAN e com `--jit`.

---

## Bug 13: send num canal sem buffer, do main thread para goroutine estacionada, travava **(FIXED 2026-10)**

```sea
local ping = chan()
go (function() for i = 1, 3 do print(<-ping) end end)()
for i = 1, 3 do ping <- i end   -- travava para sempre
```

`sea_chan_send` (o caminho bloqueante, usado pelo main thread) depositava o valor no slot de um canal sem buffer e esperava ele esvaziar, mas não acordava a goroutine estacionada em `recv_waitq`. Só o ramo de canal com buffer fazia isso. Correção: acordar o receptor, que tenta de novo e pega o valor do slot (`src/sea_channel.c`). Encontrado pelo teste `99_jit_coverage`.

---

## Variável de laço compartilhada entre iterações **(MUDADO 2026-10: agora é nova por iteração)**

Antes, uma closure ou goroutine criada no corpo de um `for` capturava a **mesma** variável `i` em todas as iterações; com goroutines o resultado era imprevisível. Por decisão de design (2026-10), a variável do laço passou a ser **nova por iteração**, como em Lua 5.4 e Go 1.22, tanto no `for` numérico quanto no `for-in`. Implementação: o compilador fecha os upvalues das variáveis do laço e das locals do corpo no ponto de `continue`, ao fim de cada iteração (`loop_close_from` em `sea_compiler.c`). Isso só custa algo quando alguma variável é de fato capturada. De passagem: antes, um `continue` também pulava o fechamento dos upvalues das locals do corpo. Documentado em `docs/reference.md` ("Closures and captured variables").

**Junto:** um objeto novo atribuído pelo dono a uma local já capturada por uma goroutine não era marcado como compartilhado (RC não atômico entre threads; o TSAN mostrava 10 corridas em 5 execuções). Agora, depois de atribuir a uma local que alguma closure captura, o compilador emite `OP_MARK_CAPTURED`. Em runtime, ele marca o valor só se o upvalue aberto daquele registrador já estiver compartilhado com uma goroutine, então objetos usados apenas por closures locais continuam com RC simples. **Teste:** `tests/unit/103_capture_semantics.sea` (o TSAN fica limpo; sem a correção, acusa a corrida).

---

## Bug 14: `if` testava o registrador errado depois de um lookup de dois valores **(FIXED 2026-10)**

```sea
local v, ok = m[i]
if ok then sum = sum + v end   -- entrava mesmo com ok == false → "arithmetic on nil"
```

Mesma família do Bug 10: a *dead write elimination* não sabia que os saltos condicionais (`JMP_TRUE`/`JMP_FALSE`/`JMP_NIL`, emitidos por `emit_jmp`) **leem** o registrador A. Quando o compilador copiava a condição para um temporário, a cópia virava NOP e o salto testava o valor antigo do temporário. Correção: `emit_jmp` marca `R[A]` como lido (e `R[A..A+3]` para `FOR_PREP`/`FOR_LOOP*`). Encontrado ao escrever o teste da Fase 4 do JIT. **Teste de regressão:** `tests/unit/102_dwe_conditions.sea` (`if`, `if not`, `while`, `and`/`or`, condições vindas de chamadas).

---

## Auditoria da dead write elimination (2026-10)

Depois dos Bugs 10 e 14 (dois casos de escrita viva apagada), a lista de leituras de cada opcode, que antes ficava espalhada em `emit_abc`, `emit_abx` e `emit_jmp`, virou uma tabela única: `sea_op_reads()` em `src/sea_chunk.c`, derivada handler por handler de `src/sea_vm.c`. Toda emissão passa por `emit()`, que marca as leituras pela tabela. O JIT (`op_regs`) usa a mesma tabela.

- **Furos que a tabela fechou:** `CHAN_TRY_SEND` lê `R[A]` (o canal) e só tinha B e C marcados; `FOR_IN_INIT`/`FOR_IN_NEXT` liam `R[A]` e funcionavam por coincidência. Um `MOVE` cru emitido no chunk pai (materialização de cópia antes de capturar um upvalue) não marcava leitura no estado do pai.
- **De passagem:** `alloc_reg` escrevia `reg_meta[r]` antes de checar `r < SEA_MAX_REGISTERS` (escrita fora do array com 256 registradores).
- **Verificação:** `make test-dwe`. Com `SEA_DWE_CHECK=1`, uma escrita eliminada carrega a string `"<dwe-eliminated>"` em vez de virar NOP, então uma escrita julgada morta por engano aparece na saída. `SEA_NO_DWE=1` desliga a eliminação. Os 139 programas do corpus (testes, exemplos, libs) dão a mesma saída nos três modos; as únicas diferenças vieram de rede, animação, ordem de goroutines e timeout. Para validar o detector, reintroduzi o Bug 14: o modo de checagem acusou um teste a mais que o modo normal.

---

## Bug 15: fatos do otimizador vazavam entre caminhos de execução **(FIXED 2026-10)**

Três miscompilações silenciosas, encontradas ao escrever o exemplo `examples/graphs` (o heap do Dijkstra falhava num pop com um elemento só):

```sea
local m = idx                     -- cópia adiada (sem MOVE)
if l < cnt then m = l end         -- o MOVE adiado era emitido só dentro do ramo
if m == idx then break end        -- caminho sem o ramo: m lia lixo

if flag then x = 7 else y = x + k end   -- `else` dobrava x como a constante 7

while n < 3 do y = a + k; a = 1.5; n = n + 1 end   -- IIADD (soma sem checagem)
                                                   -- com a já float na 2ª volta
```

**Causa:** o compilador é de passagem única e mantém fatos por registrador (cópia adiada, constante, "é inteiro") que só valiam em linha reta. Um fato aprendido num ramo do `if` vazava para o `else`; uma cópia materializada num ramo faltava no outro caminho; o corpo de um laço era compilado com os fatos de antes do laço, ignorando o que a volta anterior mudou; e locais capturadas por closures podiam ser alteradas por qualquer chamada.

**Correção** (`src/sea_compiler.c`):
- `if` e `select`: cópias materializadas e constantes descartadas **antes** dos ramos; cada alternativa parte do estado anterior; na junção, "é inteiro" só sobrevive se valer em todos os caminhos.
- Laços: na cabeça, um pequeno ponto-fixo sobre a AST do corpo mantém "é inteiro" só para as locais cujas atribuições no corpo são todas inteiras (`s = s + i` continua rápido); na saída, interseção com o estado da cabeça.
- Locais capturadas por closures nunca recebem fatos de constante ou inteiro.

`IIADD`/`IISUB` seguem sem checagem de tipo (checar custava 23% no laço mais apertado). Em vez disso, `SEA_TYPE_CHECK=1` faz os dois abortarem com diagnóstico se a inferência estiver errada. A troca é feita na tabela de dispatch, sem custo no modo normal, e entra no `make test-dwe`. Para validar o detector, desliguei a análise de laço de propósito, e ele apontou a linha exata. O corpus inteiro (139 programas) passa com a checagem. **Teste:** `tests/unit/104_optimizer_flow.sea`.


## Bug 16: modelo de memória vazava e liberava cedo demais **(FIXED 2026-10)**

Revisão da memória (não há GC). Medido com LeakSanitizer em probes que repetem um padrão N vezes: locais, `return` antecipado, `break`/`continue`, containers, closures, erros/`defer`, stdlib, goroutines e varargs vazavam de 69 a 669 bytes **por iteração**. Pior: havia use-after-free confirmado no ASAN:

```sea
local parts = string.split("alpha-" .. string.rep("z", 90) .. ",beta", ",")
local e = parts[0]
parts = nil
print(string.len(e))   -- heap-use-after-free
```

**Causa:** os registradores não participavam do refcount, e a "posse" no compilador era ad hoc (só resultados de chamadas e `..` eram liberados, e só em alguns lugares). Isso errava nos dois sentidos, e os vazamentos escondiam as liberações precoces.

**Correção:** regra de posse explícita, descrita em [memory.md](memory.md).
- Locais (e parâmetros) são donos do seu valor e são liberados em toda saída de escopo.
- Argumentos e retornos são passados como próprios.
- Canais, `defer` e goroutines possuem o que carregam.
- Nativas retornam +1 (`sea_val_ref`).
- Constantes de string ficam fixadas (sem refcount).

Os novos opcodes são `RETAIN`, `SETLOCAL`, `APPEND`, `JMP_NOT_ERR`, `LOAD_SELF`, `SET_FIELD_MOVE` e `ARRAY_PUSH_MOVE`. O formato `.seac` passou para a versão 3.

Corrigidos no caminho:
- `x = a or f(y)` compilava a chamada no registrador de `x` e sobrescrevia o local seguinte.
- `error(msg)` não segurava a mensagem.
- A análise de escape não via um objeto de arena passado como argumento (`local y = f(p)`) nem enviado num canal.
- `go obj:m(args)` passava os argumentos deslocados.
- `sea_vm_call` podia sobrescrever os argumentos da nativa que chamava de volta.
- Canais nunca eram destruídos.
- Os caches globais de strings e o pool de structs eram compartilhados entre threads sem sincronização. A corrida foi exposta quando structs passaram a ser liberados de verdade: `examples/worker_pool` dava "attempt to add field to sealed struct".
- `examples/pubsub` usava `array()`/`push()`, que não existem.

**Resultado:**
- Todos os probes estão em 0 byte/iteração, exceto ciclos, um limite conhecido do refcount.
- ASAN limpo na suíte e nos exemplos, no interpretador e no JIT; TSAN limpo nos testes concorrentes.
- Desempenho: a maioria dos benchmarks ficou igual ou mais rápida (fib e matrix_mul no interpretador ficaram mais rápidos). `sort` no JIT perdeu cerca de 50%: `local tmp = nums[j]` agora precisa segurar uma referência por iteração.

**Teste:** `tests/unit/105_ownership.sea`.

## Coletor de ciclos e correções encontradas no caminho **(2026-10)**

Ciclos de referência (`a.peer = b; b.peer = a`, uma closure guardada na tabela que ela captura, anéis, listas duplamente ligadas) não eram liberados pelo refcount. Agora há um coletor de ciclos (`src/sea_gc.c`, [memory.md](memory.md#cycles)), com `gc.collect()` e `gc.stats()`. Os probes de ciclos caíram de 480 e 3.736 bytes por iteração para 0.

Encontrados ao implementá-lo:
- **`sea_table_set` / `sea_map_set` liberavam o valor antigo antes de reter o novo.** Em `t.x = t.x`, quando só a tabela segurava o valor, ele era liberado e depois armazenado: use-after-free. Agora o valor novo é retido e armazenado antes de o antigo ser liberado; o mesmo vale para `sea_array_set` e `sea_struct_set`.
- **Gravar um valor local num container compartilhado não o marcava como compartilhado.** Outra goroutine podia ler o valor e mexer no seu contador sem operações atômicas. Os containers agora têm uma barreira de escrita (`sea_store_barrier`).
- **`math.random` atualizava um estado global sem sincronização** (corrida apontada pelo TSAN em `examples/pubsub`). Agora o estado é atualizado com compare-and-swap.
- **O JIT lia o tipo do objeto como inteiro de 32 bits.** Com o byte de flags do coletor no cabeçalho, a checagem de tipo do caminho rápido de arrays falhava sempre que o array estava no buffer de candidatos, e o código JIT caía para o interpretador.
- **Os Makefiles das bibliotecas nativas não dependiam dos headers do VM.** Uma mudança de layout deixava `.so` desatualizadas. Agora dependem de `src/*.h`.

**Testes:** `tests/unit/106_gc_cycles.sea`. Estresse: a suíte e os exemplos sob ASAN com `SEA_GC_THRESHOLD=1` (coleta em quase todo ponto seguro), mais TSAN nos testes concorrentes.

## Revisão da lib http, netpoller e correções encontradas no caminho **(2026-10)**

A revisão de `libs/http` encontrou falhas que derrubavam ou travavam o servidor e o cliente. Todas foram confirmadas por teste antes da correção:
- **Um handler que retornava `nil` derrubava o servidor.** Agora qualquer resposta inválida vira 500, que é registrado no log.
- **N conexões ociosas congelavam o servidor**, com N = número de threads do escalonador. Os sockets eram bloqueantes: cada leitura ocupava uma thread. Agora há um netpoller (`src/sea_netpoll.c`, epoll + heap de prazos numa thread própria) e o módulo embutido `net`. Uma espera estaciona só a goroutine. Com 16 clientes parados, as outras requisições respondem em 10 ms.
- **Headers como struct + timeout derrubavam o cliente**: `__timeout` era gravado num struct selado.
- **Redirect com `Location` relativo falhava.** Faltava também a porta no header `Host`.
- **Corpo não-string gerava `Content-Length: 0`.** Agora é convertido com `tostring`.
- **Respostas a HEAD enviavam corpo.**

A lib foi reescrita sobre `net` (veja [libs.md](libs.md#http)). As principais mudanças:
- keep-alive no servidor e pool de conexões no cliente;
- chunked e `100-continue`;
- limites e timeouts;
- validação de CR/LF e rejeição de formas de request smuggling;
- `req.query` (**`req.path` não inclui mais a query**);
- `srv.close()`.

`libs/ssl` passou a usar sockets de `net`. Ali também apareceu um bug: **opções passadas como map (`[=]`) eram ignoradas**. `verify = false` não tinha efeito.

Encontrados no VM:
- **Busca em struct por chave criada em tempo de execução falhava.** A chave não era interned, e `struct_find` comparava só ponteiros. Agora compara o conteúdo quando os ponteiros diferem.
- **Os casos de envio do `select` não marcavam o valor como compartilhado** antes de entregá-lo a outra goroutine. Agora chamam `sea_val_mark_shared`, como o `<-` comum.
- **Corrida ao fechar upvalues compartilhados.** Uma goroutine podia ler um upvalue enquanto o dono o fechava, e o valor era liberado sob ela (TSAN). Agora escrita e fechamento usam um spinlock por upvalue, e `LOAD_UP` relê se a localização mudou (`sea_upvalue_get`). Resta um aviso benigno do TSAN, comentado em `sea_vm.h`.

**Testes:** `tests/unit/107_net.sea` e `tests/unit/108_http_lib.sea`. Também rodaram sob ASAN (inclusive com `SEA_GC_THRESHOLD=1`) e TSAN. O cliente foi testado contra servidores HTTPS reais.

**Pendentes:** globais sem lock, DNS bloqueante e erro em handler encerrando o processo. Resolvidos na entrada seguinte.

## Globais concorrentes, DNS, panics de verdade e gzip **(2026-10)**

**Corrida na tabela de globais (crash).** `globals_lock` existia mas nunca era usado. Uma goroutine que criava uma global nova podia redimensionar a tabela enquanto outra lia qualquer global, até `print` ou `math`. O array antigo era liberado sob o leitor, e `entries`/`cap` eram publicados separados. O reprodutor dava core dump em 5 de 6 execuções e às vezes perdia uma global. Agora as globais usam `SeaTableSync` (`sea_table.h`):
- escritas com mutex e seqlock;
- leituras sem lock: o inline cache lê `cap` antes de `entries` e o caminho lento revalida pelo seqlock;
- o resize publica o array novo antes do `cap` novo e guarda o antigo até o fim. O crescimento é geométrico, então isso custa menos que o array atual.

Sem goroutines vivas, a escrita usa o caminho simples. Teste: `109_globals_concurrent`, que falhava 8 de 10 vezes no binário antigo.

**DNS bloqueava a thread do escalonador**, e o `net.connect` refazia o `getaddrinfo` a cada retomada depois de um park. Agora:
- as consultas rodam num pool de até 8 threads e a goroutine espera um eventfd pelo netpoller;
- o resultado fica em `io->aux` e não é refeito;
- IPs numéricos não passam pelo pool;
- o timeout do connect cobre a resolução, até na thread principal.

Com um DNS lento simulado (2 s por consulta), 8 conexões levam 2,5 s em vez de 10,3 s, e as outras goroutines não atrasam.

**Panic não desempilhava.** `OP_PANIC` rodava todos os defers da pilha do VM, inclusive os de funções externas. Depois de um `recover`, descartava sem rodar os defers que restavam e **continuava na instrução seguinte ao panic**: `panic("x")` seguido de `return "fine"` devolvia `"fine"`. Erros de execução nem passavam por ali: `sea_vm_error_noreturn` chamava `exit(1)`, então um erro num handler http derrubava o servidor. Agora panic e erros de execução fazem unwinding frame a frame, com semântica de Go (veja [architecture.md](architecture.md#panics) e "panic and recover" em [reference.md](reference.md)). Isso funciona através de callbacks nativos, corpos de módulo e código JIT, sem mudar o código gerado. Erros de execução trazem `arquivo:linha:` na mensagem, e o traceback mostra o arquivo de cada função (`SeaChunk.source`; antes aparecia `[?]`, e o chunk principal virava "função" quando o caminho não tinha `/`). O servidor http converte panics de handler em 500 (`opts.on_error`). Teste: `110_panic_recover`.

**`srv.close()` fechava fds que outra goroutine usava** (TSAN). Uma goroutine prestes a chamar `recv` podia ler de outra conexão se o número do fd fosse reaproveitado por um `accept`. Agora o servidor usa o novo `net.shutdown`, que acorda a goroutine dona do fd, e é ela que fecha.

**gzip:**
- o cliente pede gzip e descomprime de forma transparente (`opts.decompress`, `resp.uncompressed`);
- o servidor comprime respostas de texto quando `opts.gzip` está ligado;
- natives novos `gzip`/`gunzip` em `http_native.so`, com limite de tamanho contra zip bombs.
