# Apoio nas emendas de piso estático — Linux

## Defeito reproduzido

Com uma cópia isolada do cartão fornecido, o percurso normal até a rampa de
Gaius apresenta bloqueio ao caminhar com o analógico parcialmente inclinado.
A direção processada acompanha o percurso pretendido. O problema permanece
com as otimizações de decompilação desligadas, com a geometria inicializada
pelas rotinas originais e com cadência de 30 quadros por segundo.

A amostragem do movimento localizou faces estreitas e inclinadas entre partes
da mesma rampa. Em cinco chamadas observadas, o ajuste à face avança 0,052856 m
no eixo Z; o deslizamento acumulado devolve 0,054688 m. As contas originais de
projeção, gravidade e atrito reproduzem os resultados observados. A consulta
posterior de colisão deslizante aceita o ponto proposto: ela não causa esse
cancelamento.

Essa reprodução demonstra o bloqueio na emenda. Não demonstra que todos os
relatos de atravessar o chão tenham a mesma causa.

## Limites da intervenção

A intervenção modifica somente a orientação de apoio do Wander em pequenas
transições de malha estática. Procura apoio amplo de ambos os lados por arestas
conectadas, com normais próximas e desvio limitado pelo raio do personagem.
Não altera vértices, altura de contato, referência da superfície ou atributos
da colisão. Consultas de paredes e câmera continuam recebendo o plano original.

O adaptador usa somente três cadeias conhecidas de chamada das rotinas de
movimentação e deslizamento. O plano temporário fica em espaço vivo do chamador,
sem devolver ponteiro de memória do host ou de uma função já encerrada. As
rotinas originais executam antes da intervenção; uma continuação ainda pendente
segue sem alteração. O cache da topologia é limitado e compara os dados completos
da parte, a matriz e a descompressão antes de reutilizar resultados, inclusive
após carregar um estado no mesmo endereço.

## Validação

Resultados de gameplay e pacote serão registrados depois dos testes da versão
candidata. Até lá, este documento registra a investigação e o escopo técnico.
