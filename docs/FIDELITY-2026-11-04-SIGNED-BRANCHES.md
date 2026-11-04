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
variantes. A condição continua sendo calculada antes do delay slot. A geometria e os materiais originais são preservados.

O teste isolado do emissor corrigido confirmou a aterrissagem no degrau superior
da rampa: corrida normal de Z=-1390 até -1407, seguida de estabilidade no piso.
Porém a caminhada lenta continuou bloqueada na emenda inferior, em torno de
(359,2; 31,2; -1373,5), e na descida em torno de Z=-1406. São dois defeitos
separados. A versão final combina o emissor corrigido com o suporte limitado
de orientação nas microdobras estáticas, descrito no relatório de 2026-11-03.
Ele não desloca vértices, alturas ou limites de materiais.

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

A validação integrada usou uma cópia isolada do cartão fornecido, carregado
pelo menu normal, seguida do percurso até a rampa. No runtime final do kit
`790145de980ce349e7acb515`, a caminhada lenta passou pela subida inferior,
pelo piso superior e pela descida, sem pular ou agarrar. A corrida normal
venceu o degrau superior e permaneceu estável por quatro segundos.

Foram aprovados 24 trechos guiados de caminhada, incluindo uma repetição da
emenda inferior após F5/F8. No trecho de subida Z=-1377,56 até -1381,53,
a orientação coerente reduziu a travessia do candidato anterior de cerca de
1.560 quadros para 403 quadros entre a primeira e a última amostra de navegação.
O log confirmou a nova normal nos três consumidores conhecidos, inclusive
`caller=010e785c` sobre a face 2313.

Na comparação com o mesmo executável e o mesmo checkpoint, desligar o suporte
voltou a bloquear a descida em Z=-1378,83. Reiniciar com a configuração padrão,
sem a variável de desativação, permitiu atravessar novamente até Z=-1374,41.

As amostragens de subida e descida registraram 6.000 e 7.200 amostras com
contato estático, sem regressão de quadros nem raiz abaixo da superfície
referenciada. A distância vertical entre a raiz e a superfície ficou em
0,568–0,797 m na primeira amostragem e 0,675–0,748 m na descida. A raiz fica
acima dos pés; a leitura externa não é atômica e suas trocas de referência
entre quadros são registradas como tais, sem tratá-las como defeito físico.

O CI do código final passou nos testes de compilação, branches sintéticos,
fidelidade, entrada, estados e pacote Linux. A campanha completa continua
pendente; estes resultados cobrem somente os percursos descritos.
