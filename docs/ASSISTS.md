# Vida e força de agarre infinitas

As duas opções começam desativadas e podem ser ligadas separadamente no
launcher. Ao iniciar o executável diretamente, use `SOTC_INFINITE_HEALTH=1`
para vida ou `SOTC_INFINITE_GRIP=1` para força de agarre. Ambas se aplicam à
edição USA SCUS-97472.

A proteção mantém o valor atual da barra nos pontos em que Wander receberia
dano ou gastaria força de agarre durante o jogo. Isso cobre os golpes letais
tratados pelo sistema de dano. Recuperação, limites das barras, melhorias por
itens, animações de impacto e quedas continuam a cargo do jogo. Mortes
forçadas por outros mecanismos e mudanças de vida do final roteirizado
continuam possíveis. Wander com vida zerada segue o fluxo de morte do jogo.

As opções ficam nas configurações do launcher. O código de proteção lê os
valores máximos do jogo e altera somente o argumento das rotinas que reduzem
as barras. Os arquivos do memory card são gravados pelo jogo durante a
gravação normal. Um save state registra as barras e o progresso que a sessão
alcançou com as opções ativas. Ao carregar, as opções da sessão em execução
determinam a proteção dali em diante.
