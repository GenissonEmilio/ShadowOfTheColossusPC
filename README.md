# Shadow of the Colossus PC

Port independente da edição **PS2 USA SCUS-97472**, com launcher para Linux e Windows,
recompilador local e renderizador nativo OpenGL. Desenvolvido a partir de
[Celosia](https://github.com/HenryKun55/celosia) e
[PS2Recomp](https://github.com/ran-j/PS2Recomp).

O executável MIPS do disco e os módulos KERNEL, MANAGER e GAMECORE são traduzidos
para C++ e compilados para x86_64 no seu computador. Modelos, texturas, animações
e áudio são lidos do seu disco. O jogo não roda em PCSX2 e não requer uma BIOS.
O runtime implementa os serviços do PS2 usados pelo jogo; a decompilação manual
continua em andamento na base Celosia.

**Nenhuma ROM, BIOS ou código gerado do jogo é distribuído.**

## Validação da campanha

**18,75% validados no Linux · 3 de 16 colossos.** Valus, Hydrus e Basaran estão
**100% validados** nas versões indicadas abaixo. Há 3 colossos com testes
parciais e 10 aguardando validação. O encerramento e os créditos também
aguardam validação.

Cada colosso conta quando o teste cobre o acesso, a luta, o HUD, os pontos
fracos, os efeitos da derrota e a recarga do save. Derrotas registradas em
cartões importados ficam fora da contagem. Os resultados se referem às
versões Linux testadas.

| # | Colosso | Validação | Evidência ou próximo teste |
| --- | --- | --- | --- |
| 1 | Valus | **100% validado** | Luta, derrota, salvamento e recarga verificados. [Linux v0.2.7](https://github.com/GenissonEmilio/ShadowOfTheColossusPC/releases/tag/linux-v0.2.7) |
| 2 | Quadratus | Parcial | Acesso, flecha na sola dianteira, escalada, HUD e sigilo da cauda verificados. Sigilo da cabeça, derrota e recarga pendentes. [Registro do teste](docs/CAMPAIGN-VALIDATION.md#quadratus-na-v094-linux) |
| 3 | Gaius | Parcial | Subida, piso e descida da rampa testados. Falta validar a luta. [Linux v0.3.1](https://github.com/GenissonEmilio/ShadowOfTheColossusPC/releases/tag/linux-v0.3.1) |
| 4 | Phaedra | Pendente | — |
| 5 | Avion | Pendente | — |
| 6 | Barba | Pendente | — |
| 7 | Hydrus | **100% validado** | Revanche, sigilo antes do dano, câmera submersa, derrota e recarga na margem verificados. [Linux v0.9.4](https://github.com/GenissonEmilio/ShadowOfTheColossusPC/releases/tag/v0.9.4) · [Registro do teste](docs/CAMPAIGN-VALIDATION.md#hydrus-na-v093-linux) |
| 8 | Kuromori | Pendente | — |
| 9 | Basaran | **100% validado** | Luta, sigilo, efeitos da derrota, salvamento e recarga verificados. [Linux v0.3.4](https://github.com/GenissonEmilio/ShadowOfTheColossusPC/releases/tag/linux-v0.3.4) |
| 10 | Dirge | Pendente | — |
| 11 | Celosia | Pendente | — |
| 12 | Pelagia | Pendente | — |
| 13 | Phalanx | Pendente | — |
| 14 | Cenobia | Pendente | — |
| 15 | Argus | Pendente | — |
| 16 | Malus | Parcial | Acesso testado até depois do segundo túnel. Luta e encerramento pendentes. |

[Critérios e histórico dos testes](docs/CAMPAIGN-VALIDATION.md)

<img width="1109" height="751" alt="image" src="https://github.com/user-attachments/assets/856dbfab-25f1-40a2-95b1-1b8060d062c9" />


## Jogar

1. Baixe o pacote para o seu sistema em [Releases](https://github.com/GenissonEmilio/ShadowOfTheColossusPC/releases).
2. Extraia a pasta inteira e abra `ShadowOfTheColossusPC` no Linux ou
   `ShadowOfTheColossusPC.exe` no Windows.
3. Escolha seu disco `.iso`, `.7z`, `.zip` ou `.rar` da edição USA.
4. Aguarde a primeira compilação. Nas próximas sessões, clique em **Iniciar jogo**.

Use **Compilar / atualizar**, ao lado de **Iniciar jogo**, para preparar uma
compilação com o disco já importado enquanto faz outras tarefas. Ao terminar,
o launcher mostra **Compilação pronta**. Clique em **Iniciar jogo** quando quiser
jogar. Esse botão também verifica e prepara o cache para um runtime atualizado.

O compilador está incluído: o jogador não precisa instalar Docker, um SDK nem
executar comandos. A primeira preparação pode demorar. Até duas tarefas compilam
em paralelo, e a interface mostra o progresso. O cache é separado por versão do
compilador/runtime e validado antes de ser reutilizado.
Atualizações apenas da interface, shaders ou documentação preservam esse cache.

Requisitos comuns: CPU x64 com SSE4.1, GPU com OpenGL 3.3, 8 GB de RAM e 15 GB
livres. Linux requer **glibc 2.39+** (Ubuntu 24.04+, Fedora 40+). O pacote
Windows é portátil e requer Windows 10 versão 1903 ou posterior, ou Windows 11.
Uma CPU moderna e 16 GB de RAM são recomendáveis para a recompilação.

A seção **Sobre** cria um atalho no menu de aplicativos. Os dados ficam em
`~/.local/share/ShadowOfTheColossusPC` (ou `XDG_DATA_HOME`) no Linux e em
`%LOCALAPPDATA%\ShadowOfTheColossusPC` no Windows. Os saves são imagens
de memory card em `player/memcards`. **Meus saves** permite importar e exportar
um cartão PS2/PCSX2 de 8 MB (`.ps2`, 8.650.752 bytes) entre computadores. Feche o
jogo antes de transferir; ao importar, o cartão anterior fica em
`player/memcards/backups`. Depois, use **Carregar jogo** no menu do próprio jogo.
O botão **Abrir pasta** usa o aplicativo padrão do sistema com o ambiente de
bibliotecas restaurado, inclusive no pacote portátil. A ROM original permanece intacta.

## Preferências e atualizações do launcher

Abra **Tela cheia** no rodapé ou use **F11** para ver o launcher em tela cheia.
A preferência é lembrada. As abas e os atalhos de saves e saída ficam visíveis
enquanto a página rola. **Configurações → Sons da interface → Aplicar** liga ou
desliga os sons de navegação e confirmação.

Em **Configurações → Idioma do launcher**, escolha Português, English, Español
ou 日本語. A escolha inicial segue o sistema quando disponível, com inglês como
alternativa. Em jogo, **F1 → Sistema → Idioma** oferece inglês e português.

O launcher consulta as releases públicas do GitHub ao abrir. Na aba **Início**,
o painel **Atualizações** fica logo abaixo de **Seu jogo**. Quando encontra uma
versão compatível, mostra **Atualizar**. Use **Verificar agora** para consultar
novamente e ver se o launcher já está atualizado.
O botão baixa o pacote, confere o SHA-256 publicado pelo GitHub, atualiza a pasta
do launcher e abre a nova versão. A pasta de dados fica separada. A primeira
versão com o atualizador precisa ser instalada a partir do pacote da release.
Se a v0.4.2 mostrar erro 403 ao consultar o GitHub, instale a v0.5.0 pela
página de [Releases](https://github.com/GenissonEmilio/ShadowOfTheColossusPC/releases).
A partir da v0.5.0 o launcher usa o manifesto publicado com os pacotes para
verificar novas versões.

## Save states

No Linux, **F5** grava o estado rápido e **F8** restaura esse ponto. No menu **F1 → Estados**,
escolha o slot rápido ou um dos cinco slots manuais para salvar, carregar e
excluir. O gerenciador mostra a data e o tamanho dos estados e pede confirmação
para substituir ou excluir um slot. Os arquivos ficam em `player/savestates`;
o launcher mostra os atalhos e abre essa pasta em **Meus saves**.

O formato inclui execução, memória, áudio e serviços nativos e exige o mesmo
executável do runtime e a mesma edição do disco. Estados de outra compilação ou
arquivos incompletos são recusados antes de alterar a partida. A gravação é
atômica. A primeira versão funciona no modo nativo Linux padrão do launcher;
aguarde o fim de vídeos ou operações de carregamento se aparecer uma mensagem
de ponto ainda não suportado. Caches de renderização são reconstruídos ao carregar.

Continue salvando nos altares: o cartão normal é o caminho para transferir o
progresso entre versões. Carregar um estado não substitui o cartão real. Se esse
cartão mudou desde a captura, o carregamento do estado é recusado para preservar
os saves persistentes.

## Tela cheia e shaders

<img width="1109" height="751" alt="image" src="https://github.com/user-attachments/assets/f4dfb9da-b702-46d1-b79a-d151fa18b36a" />


Na aba **Gráficos**, escolha **Tela cheia · resolução do monitor** para usar o
monitor atual por inteiro. **Original 4:3** preserva a proporção e deixa faixas
nas laterais de telas largas; **Esticar** preenche toda a tela. F11 alterna entre
tela cheia e janela e salva a preferência. O menu F1 também ajusta a proporção.

Em **Gráficos → Perfil gráfico**, escolha **Original**, **Equilibrado**,
**Aprimorado** ou **Ultra**. Os perfis ajustam a resolução interna, o FXAA, a
filtragem de texturas e o detalhe dos modelos e da água. Use **F1 → Qualidade**
para alterar cada opção durante o jogo. [Tabela dos perfis](docs/SHADERS.md#perfis-gráficos)

Escolha o tamanho da janela e o **FSR 1** na aba **Gráficos** ou em **F1 → Vídeo**.
Use **F1 → Qualidade → Resolução** para ajustar a resolução de renderização
interna. O FSR 1 amplia a imagem da resolução interna para a área de exibição.

**F1 → Vídeo → VSync** sincroniza a apresentação com o monitor
para reduzir tearing. A opção vem ligada e pode ser alterada durante o jogo.

**Importar shader…** copia e compila no seu driver um fragment shader de
pós-processamento **GLSL 330**, em `.frag`, `.fs` ou `.glsl`. A seleção anterior
é preservada quando a validação falha. **Usar Nitidez HD** ativa o filtro incluído;
**Desativar** restaura a imagem original. A mudança de shader vale na próxima
abertura do jogo e não recompila o código do jogo.

O modelo incluído usa `texture0`, `fragTexCoord`, `fragColor` e `colDiffuse`,
além de `sourceSize`, `outputSize` (pixels físicos) e `time` (segundos).
Filtros são aplicados à imagem do jogo antes do menu e do contador de FPS.
Shaders modificam a imagem final; não criam texturas HD nem aumentam a resolução
da geometria. Presets ReShade `.fx` e RetroArch `.slangp` não são compatíveis
diretamente. Veja [o formato e exemplos](docs/SHADERS.md).

## Controles

No launcher, use o direcional ou analógico para mover o foco, **A/✕** para
confirmar e **B/○** para voltar ou fechar diálogos. **LB/RB** ou **L1/R1**
trocam de aba. **Start/Options** inicia o jogo preparado, e **←/→** ajustam o
seletor em foco. Na escolha de arquivo, **Y/△** abre a pasta acima. Controles reconhecidos
pelo SDL2 usam a base de mapeamentos incluída e o perfil salvo em
**Configurar meu controle…**. A interface mostra os atalhos quando o controle
é conectado. A navegação funciona com a janela do launcher em foco e libera
o dispositivo enquanto o jogo ou a calibração estão abertos.

**Select/Back** ou **F1** abre o menu lateral deslizante. Ele mantém a navegação
dentro do menu até confirmar uma página ou voltar. Os botões crescem suavemente
ao receber foco ou passar o mouse; a área de clique permanece estável.
**F11** alterna a tela cheia do launcher. Setas, Enter e Esc também funcionam
com perfis do Steam Input que emulam o teclado. O launcher detecta novos controles
automaticamente, mesmo com outro conectado, e combina os comandos físicos e
virtuais para evitar confirmações duplicadas. A aba **Controles** mostra os nomes
detectados e informa quando um dispositivo precisa de mapeamento.

Em **Configurações → Ícones dos botões no jogo**, escolha **Automático**,
**PlayStation**, **Xbox** ou **Teclado e mouse**. Automático é o padrão de novos
perfis: os ícones mudam durante a partida conforme o último dispositivo usado.
Um controle conectado sem atividade ou com pequeno desvio no analógico não
altera a escolha. Os ícones de teclado/mouse seguem suas atribuições na aba
Controles. Perfis existentes conservam a escolha manual; selecione Automático
para ativar a troca. Controles apresentados pelo Steam Input como Xbox usam
os ícones Xbox; a seleção manual continua disponível.

A escolha troca os elementos do jogo que usam o atlas de controle. Legendas
de tutorial que têm o texto embutido na arte original continuam no formato
do disco. A escolha do modo vale na próxima abertura do jogo.

Em **Configurações → Assistências (cheats)**, vida infinita e resistência
infinita são opções independentes, desligadas por padrão e aplicadas na próxima
abertura do jogo. Elas impedem as reduções normais de vida/resistência de Wander;
não aumentam os limites do personagem nem substituem eventos roteirizados.
Veja o [escopo e os testes das assistências](docs/ASSISTS.md).

O suporte à edição **PAL SCES-53326** permanece [pendente](docs/PAL-SUPPORT.md)
até termos um disco dessa edição para recompilar e validar. O launcher identifica
essa edição e explica a pendência; os arquivos PAL não são tratados como USA.

| Teclado | Controle PS2 |
| --- | --- |
| WASD | movimentar |
| Espaço | pular / montar · △ |
| Mouse | mover a câmera |
| Clique esquerdo / direito / central | atacar · □ / agarrar · R1 / ação · ○ |
| Q / E | arma anterior / próxima arma |
| F / R | L1 / L2 |
| C / Shift esquerdo | chamar / acelerar Agro · ✕ / R2 |
| Esc / I | Start / Select |
| Enter / Backspace | confirmar / voltar nos menus |
| ↑ / ↓ | direcional para cima / baixo |
| ← / → / Page Up / Page Down | câmera pelo teclado |
| Ctrl esquerdo / V | L3 / R3 |
| F1 | configurações, liberar cursor e **Sistema → Voltar ao launcher** |
| F5 / F8 | salvar / carregar o estado rápido |
| F11 | alternar janela / tela cheia |

Na aba **Controles → Personalizar teclado e mouse**, cada ação aceita outra tecla
ou botão do mouse. Ajuste a sensibilidade e a inversão dos dois eixos ou desative
a câmera pelo mouse. O perfil é salvo localmente e aplicado na próxima abertura.
F1, F5, F8, F9 (diagnóstico) e F11 são reservados; Delete remove uma atribuição no editor.
O cursor é liberado ao abrir F1 ou trocar de janela; comandos mantidos durante
a transição precisam ser soltos antes de voltar ao jogo. Esc pausa sem encerrar.

Controles USB/Bluetooth também são aceitos. O menu F1 ajusta vídeo, efeitos,
som e idioma da interface do port. Os textos originais do disco são preservados.
O mapeamento Linux inclui o Zikway HID gamepad / GameSir Nova Lite em modo HID
(USB `3537:1041`), inclusive direcionais, analógicos e gatilhos.
O runtime também inclui 743 registros Linux da base comunitária
[SDL_GameControllerDB](https://github.com/mdqinc/SDL_GameControllerDB), com GUIDs
adaptados ao GLFW. O jogo e o menu F1 usam o controle reconhecido que estiver
recebendo comandos, inclusive quando ele não está na primeira posição USB.
Reconectar o controle não exige reiniciar o jogo. Um modelo ainda ausente pode
ser configurado em **Controles → Configurar meu controle…**: selecione o
dispositivo, solte os analógicos e siga as 20 etapas. O perfil fica salvo para
as próximas aberturas do jogo e também funciona no menu F1. Xbox/XInput,
PlayStation e genéricos são aceitos quando o Linux os disponibiliza como joystick;
o nome comercial e o formato físico do controle não determinam o mapeamento.
Enter pula comandos ausentes, Backspace refaz a etapa anterior e Esc cancela
preservando o perfil anterior. Cada dispositivo mantém seu próprio perfil.
No desenvolvimento, também são aceitos arquivos indicados por
`PS2X_GAMEPAD_MAPPINGS` ou a variável `SDL_GAMECONTROLLERCONFIG`.

## Correções de fidelidade em validação

As rotinas adicionais dos indicadores do HUD passam a ser capturadas, incluindo
o anel de carga do ataque. O cache de texturas verifica toda a paleta animada,
evitando reaproveitar cores e transparências antigas quando uma entrada muda.
As colisões usam por padrão o código original traduzido; as reescritas manuais
experimentais dessa parte ficam desativadas. Isso preserva também os registradores
VU0 que as reescritas não reproduzem integralmente.

Os testes sintéticos cobrem essas correções. O registro de 28/10 reúne os
três trajetos no altar Cliff Path com o cartão recebido e a correção de
conteúdo gráfico antigo no cache. O estado das lutas está na
[validação da campanha](#validação-da-campanha). [Registro de 28/10](docs/FIDELITY-2026-10-28.md)

## Compilar e verificar

Para desenvolver, instale Docker e Python 3, então execute:

```sh
python3 -m unittest discover -s tests/linux -v
tools/linux/build.sh
```

Esse build **não precisa da ROM** e gera o launcher, bibliotecas do runtime e o
kit de compilação. O pacote final é verificado para excluir imagens, executáveis
PS2, módulos XFF, código gerado e saves. O workflow Linux executa esses testes e
compila a cada push/PR. A execução do jogo é validada localmente com disco próprio;
CI não recebe arquivos do jogo.

O CI também testa a apresentação OpenGL em monitores virtuais 16:9 e ultrawide:
resolução nativa, barras, esticamento, pixels filtrados, rejeição de GLSL inválido,
desativação do filtro e retorno ao tamanho anterior da janela.

O pipeline original de desenvolvimento segue disponível em `./celosia`;
[a documentação da base](docs/UPSTREAM-CELOSIA.md) descreve seus estágios. Ele não
é o procedimento necessário para usar o release gráfico.

## Origem e licenças

- Base principal fixada em **HenryKun55/celosia e0bc43d6d4eb8cc1a022c4b277f396be3ee4fbbe**,
  com [rotinas nativas e otimizações selecionadas](docs/CELOSIA-INTEGRATION-2026-11-17.md)
  da revisão **c2514d9**. A decompilação continua em andamento; a cobertura
  nativa dos serviços do PS2 e do renderizador é uma medida separada.
- Celosia e PS2Recomp: **GPL-3.0**, [LICENSE](LICENSE). O código correspondente do
  port também acompanha o release em `compiler/source`.
- Launcher Linux e ilustração vetorial original: GenissonEmilio, MIT em
  [tools/desktop/LICENSE](tools/desktop/LICENSE). Fluxo adaptado dos launchers
  ForbiddenMemoriesPC e CrashBandicootPC.
- [Tenkaichi3Decomp](https://github.com/stufno102/Tenkaichi3Decomp) foi referência
  para a experiência de importação e build PS2. Seu código de jogo é específico
  do Dragon Ball; ele não foi renomeado para executar Shadow of the Colossus.
- As bibliotecas do compilador mantêm suas licenças e identificação de versões
  em `compiler/licenses` e `compiler/packages.txt` no release.

Shadow of the Colossus pertence à Sony / Team Ico. Projeto não oficial, sem
vínculo com os titulares. Créditos completos da base estão na documentação Celosia.
