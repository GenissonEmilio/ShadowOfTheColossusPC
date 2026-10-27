# Fidelidade e controle — investigação de 27/10/2026

Relatos: colossos com cores incorretas após a derrota; tentáculos escuros
invisíveis; pontos fracos apagados; indicador da carga de ataque ausente;
Wander atravessando chão/colossos, especialmente Gaius; controle USB só funciona
via Steam Input.

O usuário confirmou que são relatos de outra pessoa, não reproduções neste
notebook. Ainda não temos a versão exata do pacote, o ID USB do controle nem
um save da cena relatada.

## Mudanças e evidências

- **HUD:** a faixa capturada terminava em `0x013F78A8`, deixando de fora as
  chamadas de desenho das rotinas seguintes. Agora inclui os helpers até
  `0x013F8AA0`. O parser também converte pontos GS em quadrados de um pixel
  lógico. O teste envia uma primitiva a partir de cinco origens do HUD e
  verifica os triângulos emitidos, sem instruções ou dados do jogo.
- **Paletas animadas:** a chave do cache amostrava apenas 16 palavras de uma
  CLUT de 256 cores. Entradas não amostradas podiam mudar sem invalidar a textura
  GPU. Agora todas as entradas são verificadas; o teste altera cada entrada,
  incluindo alpha, e confirma uma chave diferente a cada mudança. O tamanho
  usado para amostrar texturas RGBA passa a contar quatro bytes por pixel.
  Esses defeitos são comprovados pelo teste; a ligação com cada efeito relatado
  ainda exige comparação visual na cena correspondente.
- **Colisão:** as 19 rotinas reescritas manualmente em `decomp_collision.cpp`
  ficam opt-in. Por padrão, os pontos de entrada continuam apontando para as
  funções originais traduzidas. Essas reescritas foram validadas na base em
  trajetos do santuário e declaram não preservar todo o estado VU0. Não há
  ajuste artificial de coordenadas, teleporte ou chão invisível. Para comparar
  no desenvolvimento, `SOTC_NATIVE_COLLISION=1` reativa as reescritas e
  `SOTC_DIFFTEST=<nomes>` permite investigar divergências. O teste confirma
  o dispatch padrão, o opt-in e a preservação da função original para fallbacks.
- **Controle:** 743 mapeamentos Linux de SDL_GameControllerDB, revisão
  `22a98f6339abf4b6ca6cb061443a70e4f7ab252d`, com os bytes de CRC do nome zerados
  para os GUIDs GLFW. A licença zlib e a origem estão em
  `PS2Recomp/ps2xRuntime/third_party/gamecontrollerdb`. Preservado o mapeamento
  local preexistente do Zikway. O jogo e F1 selecionam um controle reconhecido
  entre as quatro posições oferecidas pelo raylib; um joystick sem mapeamento
  não bloqueia outro reconhecido. Mapeamentos personalizados podem ser carregados
  por arquivo, até 1 MiB, ou pela variável padrão SDL.
  GLFW e a leitura do estado do controle ficam na thread da janela. A thread
  do jogo consome um snapshot protegido por mutex; não chama GLFW diretamente.
- **Controles sem perfil:** o launcher 0.2.1 oferece um assistente de 20 etapas
  em Controles. Ele captura botões, hats, eixos e gatilhos do joystick bruto,
  cria um perfil pelo GUID real e grava atomicamente, preservando os perfis de
  outros dispositivos. Não depende do nome comercial do controle. O jogo
  carrega esse arquivo automaticamente. A configuração não altera o cache do
  executável; atualizações apenas da ferramenta também preservam esse cache.

## Validação local

- Dezoito testes Python de importação, cache, gráficos e seleção de perfis passaram.
- `tools/linux/test_fidelity.sh`: passou, sem ROM e sem contexto de GPU.
- `tests/native/gamepad.cpp`, acionado pelo Linux `uinput`: passou com um
  dispositivo desconhecido na primeira posição e um USB genérico mapeado na
  segunda. Resultado: `cross=1 up=1 analog=1 release=1 slot1=1`.
  Os dispositivos virtuais foram removidos ao terminar. Nenhum comando foi
  injetado em teclado, mouse ou outro aplicativo.
  O teste também usa threads separadas para a janela e para a leitura do
  pacote pelo jogo.
- Assistente completo, com um segundo USB virtual de VID/PID não incluídos na
  base de mapeamentos: 20 comandos configurados, arquivo persistido e carregado
  por outro processo. Botão X, direcional, analógico, soltura e seleção da
  segunda posição passaram pelo backend real do jogo. Roteiro:
  `tools/linux/test_gamepad_mapping.py`.
- Nova execução nativa com as colisões originais ativas: 180 segundos sem
  encerramento inesperado, terminada pelo limite do teste. É um teste de boot e
  entrada; não comprova a luta contra Gaius. O teste anterior terminou em
  SIGSEGV em `XPending` enquanto o kit também estava sendo refeito. A repetição
  usou bibliotecas privadas imutáveis e o snapshot de entrada entre threads.
- CI `40601484665` passou para `c0a23ab`: inclui o build Linux completo e os
  testes de HUD, paleta e dispatch. O assistente foi acrescentado depois desse CI.

O CI executa as regressões de HUD/paleta/dispatch após compilar o kit. O teste
USB virtual requer permissão de acesso a `/dev/uinput` e fica como integração
local, sem modificar permissões do sistema.

## Validação ainda necessária

Não há save da luta contra o terceiro colosso confirmado neste computador.
O cartão existente do jogador não deve ser modificado por diagnósticos: copiar
para uma pasta privada de teste antes de executar o jogo. As duas imagens
recebidas mostram os sintomas, mas não contêm o estado necessário para repetir
o combate. O controle físico relatado também não estava conectado na inspeção.

Ainda verificar: carga do ataque durante escalada; marcas antes/depois das
estocadas; transformação do corpo e tentáculos na derrota; subida e permanência
em Gaius; desempenho com colisões originais; controle físico e reconexão.

Foram consultados a base Celosia (`upstream/main` em `d2d8bb1`), o projeto
ico-pc e a documentação de entrada do GLFW. Não foi incorporada uma atualização
geral da base: seus avanços recentes incluem mudanças extensas de renderização,
áudio e escalonamento, que precisam de validação própria no Linux.
