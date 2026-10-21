# Shadow of the Colossus PC — Linux

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

## Jogar

1. Baixe o pacote Linux em [Releases](https://github.com/GenissonEmilio/ShadowOfTheColossusPC/releases).
2. Extraia a pasta inteira e abra `ShadowOfTheColossusPC`.
3. Escolha seu disco `.iso`, `.7z`, `.zip` ou `.rar` da edição USA.
4. Aguarde a primeira compilação. Nas próximas sessões, clique em **Iniciar jogo**.

O compilador está incluído: o jogador não precisa instalar Docker, um SDK nem
executar comandos. A primeira preparação pode demorar. Até duas tarefas compilam
em paralelo, e a interface mostra o progresso. O cache é separado por versão do
compilador/runtime e validado antes de ser reutilizado.

Requisitos: Linux x86_64 com **glibc 2.39+** (Ubuntu 24.04+, Fedora 40+), GPU com
OpenGL 3.3, 8 GB de RAM e pelo menos 15 GB livres. Uma CPU moderna e 16 GB de RAM
são recomendáveis para a compilação. Este pacote é exclusivamente para Linux.

A seção **Sobre** cria um atalho no menu de aplicativos. Os dados ficam em
`~/.local/share/ShadowOfTheColossusPC` (ou `XDG_DATA_HOME`). Os saves são imagens
de memory card em `player/memcards`. A ROM original permanece intacta.

## Controles

| Teclado | Controle PS2 |
| --- | --- |
| WASD | analógico esquerdo |
| setas | direcional |
| H / J / Y / U | ✕ / ○ / □ / △ |
| I / O | L1 / L2 |
| K / L | R1 / R2 |
| Z / X | Start / Select |
| F1 | configurações do port |
| F11 | tela cheia |

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
