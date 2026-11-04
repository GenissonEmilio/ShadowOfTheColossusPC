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

O teste integrado com os branches signed64 corrigidos confirmou a caminhada
lenta na subida inferior e a aterrissagem no degrau superior. Uma descida lenta
encontrou outra microdobra. A mesma reprodução, com o suporte desligado, bloqueou
no mesmo local tanto a 60 quanto a 30 atualizações de simulação por segundo.

O rastreamento das rotinas originais mostrou consulta de movimento sem colisão,
aterrissagem válida e ausência de obstrução adicional. O deslocamento pedido já
apontava contra a caminhada: a normal inclinada da face estreita participa da
força de deslizamento antes dessas consultas.

## Apoio dentro do raio do personagem

A extensão para essa situação considera o raio de apoio em XZ. Usa a normal de
uma face larga do mesmo material que ainda se encontra dentro desse raio.
Exige também uma saída larga do outro lado da dobra, alcançada por outra aresta
inicial. As cadeias permanecem fechadas, recíprocas, estreitas e limitadas a seis
arestas. A face de saída confirma a continuidade geométrica; seus atributos
originais são preservados e sua normal não é misturada à normal devolvida.

A política adicional fica restrita aos três acessos conhecidos do Wander com
apoio estático válido. A orientação XZ do deslocamento e os dois acessos de
deslizamento recebem a mesma normal, mantendo os cálculos de movimento coerentes.
O consumidor XZ utiliza somente XYZ para construir sua rotação; o termo D do
plano não entra no deslocamento.
O ponto local de terreno em GP+0x10 é transformado pela matriz da parte e
verificado contra o triângulo original antes de ancorar o plano temporário.
GP+0x20 contém a sonda do personagem, que pode estar acima do contato real.

As fixtures sintéticas cobrem a separação das duas cadeias, limites de seis e
sete arestas, materiais distintos, raio tangencial, vértices, saídas ausentes,
categorias condicionais, paredes e planos de apoio ambíguos. O adaptador confere
o contexto completo e toda a RAM fora dos 16 bytes temporários do chamador,
incluindo rejeição de contatos inválidos e do personagem sem apoio.

Resultados do percurso com a extensão serão registrados após a validação
integrada. A campanha completa continua pendente.
