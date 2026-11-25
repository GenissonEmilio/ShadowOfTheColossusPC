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
