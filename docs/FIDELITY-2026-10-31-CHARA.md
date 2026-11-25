# Estado do chamador nativo nos callbacks de personagem — 31/10/2026

`charaLinks` (`0x010CE558`) guardava os registradores preservados somente em variáveis do host. O epílogo restaurava seus valores, mas os callbacks executados durante a função viam um frame de pilha sem os saves do prólogo e valores antigos de `s0–s7`, `fp` e `f20`.

Isso não é equivalente ao código PS2. Um callee pode salvar esses registradores na própria pilha e repassar parte desse estado a outro objeto. O upstream encontrou esse caminho na criação de sequências de animação. A correção é uma adaptação isolada da [auditoria upstream 77c962c945](https://github.com/HenryKun55/celosia/commit/77c962c945), conferida com o prólogo, os dois sites de callback e o epílogo SCUS-97472. Não inclui as novas otimizações ou a infraestrutura geral desse commit.

Agora a função grava seu frame real de `0x70` bytes, expõe os valores de registradores usados em cada callback e relê os saves no retorno. Preserva os valores de 64 bits onde o código usa `daddu`/`sd`/`ld`. Na segunda passagem, `a3` permanece com o valor deixado pelo callback anterior: o código original não o redefine em cada chamada.

## Evidência sem ROM

`tests/native/chara_state.cpp` usa objetos, handles e callbacks sintéticos. O callback inspeciona os saves na RAM, observa os registradores vivos e copia um save do chamador para uma área de objeto persistente. Verifica também os epílogos e a retenção de `a3` entre três chamadas. São quatro cenários: lista vazia, vínculos diretos, slot com sucesso e slots com falha.

O mesmo teste, compilado contra `decomp_chara.cpp` da v0.2.6 (`986643a`), falha em 234 observações de estado/frame/objeto. Contra a correção, todos os quatro cenários passam. Essas observações pertencem ao mesmo problema de fidelidade; não são 234 bugs diferentes. O script é `tools/linux/test_chara_state.sh`. As execuções locais usaram o executor protegido com limite de 1 GiB e container de 900 MiB, serialmente.

Logs privados: `artifacts/campaign-2026-10-31/collision/chara-before.log`, `chara-after.log` e `chara-comparison.log`. Nenhuma instrução extraída do jogo, save, ROM ou código gerado do jogo é incluído no teste publicado.

## Limite

O teste demonstra um defeito de estado visível a callbacks e sua correção. Ele não demonstra que esse defeito seja a causa do atravessamento de chão na rampa de Gaius. A derrota de cada colosso, seus efeitos visuais e as travessias precisam ser verificados na campanha normal com save separado. Não foram acrescentados teleporte, alterações de altura ou escrita externa no estado do jogo.
