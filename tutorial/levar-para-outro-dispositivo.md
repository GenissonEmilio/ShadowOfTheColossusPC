# Como levar o jogo para outro dispositivo

Há três situações diferentes:

1. **Outro PC Linux:** copiar o jogo já compilado, ou compilar de novo lá.
2. **Só o progresso:** levar os saves.
3. **Outra plataforma** (macOS, PS Vita): compilar para ela.

> O jogo compilado contém código gerado a partir do **seu** disco. Use as
> cópias só nos seus dispositivos. Não publique `disc/`, `build/`, a ISO nem
> os memory cards.

---

## 1. Outro PC Linux

### Opção A: instalar pelo launcher (mais simples)

No outro PC, baixe o pacote de Releases, abra `ShadowOfTheColossusPC` e
escolha a sua ISO. O launcher compila tudo lá. Depois, copie os saves
(seção 2).

Requisitos do destino: Linux x86_64 com glibc 2.39+ (Ubuntu 24.04+, Fedora 40+),
OpenGL 3.3, 8 GB de RAM e 15 GB livres.

### Opção B: copiar a pasta já compilada

Para não compilar de novo, copie a pasta do projeto inteira **mantendo o
mesmo caminho relativo** (`disc/`, `build/` e `memcards/` precisam ficar juntos).

O que é necessário para jogar:

| Pasta / arquivo | Para quê | Tamanho aprox. |
| --- | --- | --- |
| `celosia` | script que abre o jogo | — |
| `build/cmake/ps2xRuntime/ps2EntryRunner` | o jogo compilado | 130 MB |
| `disc/` | arquivos do seu disco (modelos, texturas, áudio) | 2,5 GB |
| `memcards/` | seus saves | 8 MB |
| `celosia_settings.cfg` | suas opções do menu F1 | — |

Copiando com `rsync` (pelo terminal, de um PC para o outro):

```sh
rsync -a --info=progress2 \
  --exclude '*.iso' --exclude '*.7z' --exclude 'build/deps' --exclude 'logs' --exclude 'shots' \
  ShadowOfTheColossusPC/ usuario@outro-pc:~/Jogos/ShadowOfTheColossusPC/
```

Ou use um pendrive/HD externo formatado em **ext4** (FAT32 não guarda
arquivos acima de 4 GB nem a permissão de execução).

No destino:

```sh
cd ~/Jogos/ShadowOfTheColossusPC
./celosia run
```

**Limitação importante:** o executável usa as bibliotecas do sistema em que foi
compilado (por exemplo, um build feito no Debian 13 precisa do **FFmpeg 7.1**:
`libavcodec.so.61`, `libavformat.so.61`). Para conferir se o destino tem
tudo:

```sh
ldd build/cmake/ps2xRuntime/ps2EntryRunner | grep "not found"
```

Se aparecer alguma linha `not found`, instale a biblioteca correspondente ou
compile no destino (`./celosia build`, veja `compilar.md`). O código
recompilado em `build/runner` é reaproveitado, então não precisa refazer o
`setup` inteiro, só ter as ferramentas de compilação.

**Atalhos:** atalhos `.desktop` ou scripts criados à mão costumam ter o
caminho absoluto da pasta antiga. No destino, ajuste as linhas `Exec=`,
`Path=` e `Icon=` (e qualquer `cd` no script) para o novo caminho.

---

## 2. Levar só o progresso (saves)

O progresso inteiro fica num arquivo de memory card de 8 MB, no formato PCSX2:

| Como você joga | Arquivo |
| --- | --- |
| `./celosia run` | `memcards/Mcd001.ps2` (ou a pasta em `CELOSIA_MEMCARDS`) |
| launcher | `~/.local/share/ShadowOfTheColossusPC/player/memcards/Mcd001.ps2` |
| PS Vita | `ux0:data/celosia/memcards/Mcd001.ps2` |

Para levar:

1. **Feche o jogo** nos dois lados (o arquivo fica aberto enquanto o jogo roda).
2. No destino, faça uma cópia do `Mcd001.ps2` que já existe lá (se existir).
3. Copie o seu `Mcd001.ps2` para a pasta do destino, com o mesmo nome.

Como o formato é o do PCSX2, o mesmo arquivo também abre no PCSX2 (e pode
vir dele), na edição USA do jogo.

---

## 3. Outra plataforma

- **macOS:** clone o repositório no Mac e siga `compilar.md` (o `./celosia`
  e o backend Metal são da base original, feita no macOS). Copie os saves
  como na seção 2.
- **PS Vita:** no PC, depois de um `./celosia setup`, rode `./celosia vita`
  (precisa do VitaSDK). Instale o `build/vita/ps2xRuntime/ps2EntryRunner.vpk`
  no Vita (VitaShell). Copie o conteúdo da sua pasta `disc/` para
  `ux0:data/celosia/disc/` e os saves para `ux0:data/celosia/memcards/`.
  Logs ficam em `ux0:data/celosia/log.txt` e `err.txt`. No Vita, o desempenho
  ainda é baixo (18–25 fps na introdução, segundo `docs/UPSTREAM-CELOSIA.md`).
- **Windows / Android / ARM:** não há suporte neste repositório. Seria preciso
  portar o runtime (janela, áudio, entrada e OpenGL) e os scripts de build.
  O PS2Recomp já traz `sse2neon` para ARM, mas isso sozinho não basta.
