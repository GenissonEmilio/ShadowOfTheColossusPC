# Shadow of the Colossus PC — Linux e Windows

Port independente da edição **PS2 USA SCUS-97472**, com launcher em português,
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

**25% validados no Linux · 4 de 16 colossos.** Valus, Quadratus, Hydrus e
Basaran estão **100% validados** nas versões indicadas abaixo. Há 2 colossos
com testes parciais e 10 aguardando validação. O encerramento e os créditos
também aguardam validação.

Cada colosso conta quando o teste cobre o acesso, a luta, o HUD, os pontos
fracos, os efeitos da derrota e a recarga do save. Derrotas registradas em
cartões importados ficam fora da contagem. Os resultados se referem às
versões Linux testadas.

| # | Colosso | Validação | Evidência ou próximo teste |
| --- | --- | --- | --- |
| 1 | Valus | **100% validado** | Luta, derrota, salvamento e recarga verificados. [Linux v0.2.7](https://github.com/GenissonEmilio/ShadowOfTheColossusPC/releases/tag/linux-v0.2.7) |
| 2 | Quadratus | **100% validado** | Acesso, escalada, HUD, os dois sigilos, derrota, salvamento e recarga verificados. [Linux v0.9.5](https://github.com/GenissonEmilio/ShadowOfTheColossusPC/releases/tag/v0.9.5) · [Registro do teste](https://github.com/GenissonEmilio/ShadowOfTheColossusPC/blob/5f11f5e3d4a0905d8f47c977f84bc98db7b31676/docs/CAMPAIGN-VALIDATION.md#quadratus-na-v094-linux) |
| 3 | Gaius | Parcial | Subida, piso e descida da rampa testados. Falta validar a luta. [Linux v0.3.1](https://github.com/GenissonEmilio/ShadowOfTheColossusPC/releases/tag/linux-v0.3.1) |
| 4 | Phaedra | Pendente | — |
| 5 | Avion | Pendente | — |
| 6 | Barba | Pendente | — |
| 7 | Hydrus | **100% validado** | Revanche, sigilo antes do dano, câmera submersa, derrota e recarga na margem verificados. [Linux v0.9.4](https://github.com/GenissonEmilio/ShadowOfTheColossusPC/releases/tag/v0.9.4) · [Registro do teste](https://github.com/GenissonEmilio/ShadowOfTheColossusPC/blob/5f11f5e3d4a0905d8f47c977f84bc98db7b31676/docs/CAMPAIGN-VALIDATION.md#hydrus-na-v093-linux) |
| 8 | Kuromori | Pendente | — |
| 9 | Basaran | **100% validado** | Luta, sigilo, efeitos da derrota, salvamento e recarga verificados. [Linux v0.3.4](https://github.com/GenissonEmilio/ShadowOfTheColossusPC/releases/tag/linux-v0.3.4) |
| 10 | Dirge | Pendente | — |
| 11 | Celosia | Pendente | — |
| 12 | Pelagia | Pendente | — |
| 13 | Phalanx | Pendente | — |
| 14 | Cenobia | Pendente | — |
| 15 | Argus | Pendente | — |
| 16 | Malus | Parcial | Acesso testado até depois do segundo túnel. Luta e encerramento pendentes. |

[Critérios e histórico dos testes](https://github.com/GenissonEmilio/ShadowOfTheColossusPC/blob/5f11f5e3d4a0905d8f47c977f84bc98db7b31676/docs/CAMPAIGN-VALIDATION.md)

<img width="1172" height="807" alt="image" src="https://github.com/user-attachments/assets/d18109b2-af2e-4947-a7b2-74f1a5ad8160" />


## Jogar

1. Baixe o pacote Linux em [Releases](https://github.com/GenissonEmilio/ShadowOfTheColossusPC/releases).
2. Extraia a pasta inteira e abra `ShadowOfTheColossusPC`.
3. Escolha seu disco `.iso`, `.7z`, `.zip` ou `.rar` da edição USA.
4. Aguarde a primeira compilação. Nas próximas sessões, clique em **Iniciar jogo**.

O compilador está incluído: o jogador não precisa instalar Docker, um SDK nem
executar comandos. A primeira preparação pode demorar. Até duas tarefas compilam
em paralelo, e a interface mostra o progresso. O cache é separado por versão do
compilador/runtime e validado antes de ser reutilizado.
Atualizações apenas da interface, shaders ou documentação preservam esse cache.

Requisitos: Linux x86_64 com **glibc 2.39+** (Ubuntu 24.04+, Fedora 40+), GPU com
OpenGL 3.3, 8 GB de RAM e pelo menos 15 GB livres. Uma CPU moderna e 16 GB de RAM
são recomendáveis para a compilação. Este pacote é exclusivamente para Linux.

A seção **Sobre** cria um atalho no menu de aplicativos. Os dados ficam em
`~/.local/share/ShadowOfTheColossusPC` (ou `XDG_DATA_HOME`). Os saves são imagens
de memory card em `player/memcards`. A ROM original permanece intacta.

## Tela cheia e shaders

<img width="1172" height="807" alt="image" src="https://github.com/user-attachments/assets/3814964c-d53b-4899-9490-69edd722dd77" />


Na aba **Gráficos**, escolha **Tela cheia · resolução do monitor** para usar o
monitor atual por inteiro. **Original 4:3** preserva a proporção e deixa faixas
nas laterais de telas largas; **Esticar** preenche toda a tela. F11 alterna entre
tela cheia e janela e salva a preferência. O menu F1 também ajusta a proporção.

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

Teclado e mouse (clique na janela para capturar o mouse; **Esc** pausa e solta o mouse):

| Teclado / mouse | Ação | Controle PS2 |
| --- | --- | --- |
| WASD | mover (segure **Alt esquerdo** para andar devagar) | analógico esquerdo |
| mouse | câmera | analógico direito |
| setas | direcional | direcional ← → ↑ ↓ |
| Espaço | pular | △ |
| botão esquerdo | usar a arma (espada / arco) | □ |
| botão direito / F | agarrar (segure) | R1 |
| E / Enter | chamar e acelerar o Agro, confirmar | ✕ |
| Shift esquerdo | erguer a espada (procurar a luz do colosso) | ○ |
| Tab / botão do meio | olhar para o colosso | L1 |
| rodinha / 1 e 2 | trocar de arma | direcional ← → |
| Z / X | L2 / R2 | L2 / R2 |
| C / V | L3 / R3 | L3 / R3 |
| Esc | pausa | Start |
| Backspace | Select | Select |
| F1 | configurações do port | — |
| F11 | tela cheia | — |

`PS2X_MOUSE_SENS` ajusta a sensibilidade do mouse (padrão 1) e
`PS2X_MOUSE_INVERT_Y=1` inverte o eixo vertical da câmera.

Controles USB/Bluetooth também são aceitos. O menu F1 ajusta vídeo, efeitos,
som e idioma da interface do port. Os textos originais do disco são preservados.

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

- Base fixada em **HenryKun55/celosia e0bc43d6d4eb8cc1a022c4b277f396be3ee4fbbe**.
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
