# Colisão e aterrissagem — comparação signed no R5900

## Defeito localizado

O recompilador emitia comparações signed de 32 bits para BGEZ, BLTZ, BGTZ e
BLEZ, inclusive as variantes likely e link. O R5900 compara os 64 bits baixos
do registrador. A implementação de referência do
[PCSX2](https://github.com/PCSX2/pcsx2/blob/master/pcsx2/Interpreter.cpp#L333)
usa `SD[0]`, definido como `s64` em
[R5900.h](https://github.com/PCSX2/pcsx2/blob/master/pcsx2/R5900.h#L17).

Na rotina de condições de movimento do jogo, o desvio em `10C9120` recebe um
valor de 64 bits carregado por LD. O exemplo observado, `0x0000000680000000`,
é positivo em 64 bits e negativo se truncado para 32 bits. A tradução antiga
seleciona um teste de qualquer bit da máscara em vez da comparação de material
pretendida. A condição invertida passa a rejeitar a aterrissagem em um piso
com outro valor nesse campo.

O diagnóstico foi feito com uma cópia isolada do cartão fornecido. A consulta
física encontrou a rampa e registrou contato e inclinação de aproximadamente
20,36 graus. A sequência posterior das condições identificou a rejeição. O
problema também se reproduziu carregando apenas o cartão, sem F5/F8, e com
memória suficiente para não haver reclaim ou swap no grupo de testes.

## Alteração

O emissor usa `GPR_S64` nas quatro comparações compartilhadas pelas doze
variantes. A condição continua sendo calculada antes do delay slot. Não há
alteração das rotinas do jogo, de sua geometria ou de seus materiais.

A tentativa anterior de estabilizar normais em pequenas dobras da malha foi
retirada. A validação desta versão usa a colisão original com o código
regenerado pelo recompilador corrigido.

## Verificação

O teste `signed_branch_codegen_execution` gera código a partir de instruções
sintéticas, compila esse código e o executa com registradores de 64 bits. Ele
cobre sinal do bit 63 diferente do bit 31, valores zero, a metade superior
de 128 bits, delay slots e exemplos sintéticos de classificação por máscara.
Nenhum código ou dado do disco é usado na fixture.

A fixture executou **389 casos** (384 combinações dos branches e cinco
predicados por máscara), sem falhas. O controle negativo recompilou a mesma
fixture com a truncagem antiga e reproduziu a rejeição indevida do material.

A suíte MiniTest completa já apresenta falhas nesta base: o comparativo isolou
somente o objeto do emissor na biblioteca e manteve os demais membros por
SHA-256. O emissor antigo teve 103 aprovações e 48 falhas; o novo, 104 e 47.
A única diferença foi o novo teste signed64, que falha com o emissor antigo.
Ambos encerraram no mesmo SIGSEGV após os mesmos testes de timers. Isso não é
um resultado de aprovação da suíte inteira; os recibos locais preservam as
falhas preexistentes. Os testes de execução focados são um gate obrigatório
no CI, incluindo a exigência de que pelo menos um teste seja encontrado.

Os resultados do percurso com o executável regenerado serão acrescentados
após sua validação. A campanha completa continua pendente.
