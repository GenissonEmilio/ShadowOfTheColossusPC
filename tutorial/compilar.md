# Como compilar o jogo

O port não distribui o jogo. O executável MIPS do disco (`SCUS_974.72`) e os
módulos `KERNEL`, `MANAGER` e `GAMECORE` são traduzidos para C++ pelo
PS2Recomp e compilados para x86_64 na sua máquina. Para isso você precisa do
seu disco **Shadow of the Colossus (USA, SCUS-97472)** em `.iso` (ou `.zip`
com a ISO dentro).

Há dois caminhos:

- **A) Build local com `./celosia`:** compila e roda o jogo direto da pasta do
  repositório. É o caminho para desenvolver.
- **B) Pacote de release (launcher):** gera o launcher gráfico e o kit de
  compilação, sem ROM. É o que vai para a página de Releases.

---

## A) Build local com `./celosia`

### 1. Requisitos

- Linux x86_64, GPU com OpenGL 3.3, 8 GB de RAM (16 GB recomendado), cerca
  de 15 GB livres.
- Ferramentas: `cmake`, `ninja`, `clang`/`clang++` (ou gcc), `python3`
  (3.10+), `bsdtar`, `pkg-config`, `git`.
- Bibliotecas de desenvolvimento: FFmpeg (`libavcodec`, `libavformat`,
  `libavutil`, `libswresample`, `libswscale`), ALSA, OpenGL e X11.

No Debian/Ubuntu, os nomes dos pacotes são:

```sh
sudo apt install cmake ninja-build clang python3 libarchive-tools pkg-config git \
  libavcodec-dev libavformat-dev libavutil-dev libswresample-dev libswscale-dev \
  libasound2-dev libgl-dev libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev
```

O raylib, o imgui e o sse2neon são baixados pelo CMake (FetchContent) no
primeiro configure, então é preciso internet nessa etapa.

> Para não instalar no sistema, dá para extrair os pacotes `.deb` numa pasta
> (por exemplo `build/deps/sysroot`) e apontar `PATH`, `PKG_CONFIG_PATH`,
> `LD_LIBRARY_PATH`, `CC=clang` e `CXX=clang++` para ela antes do `./celosia`.

### 2. Primeira compilação (setup)

```sh
cd ShadowOfTheColossusPC
CELOSIA_JOBS=8 \
CELOSIA_CMAKE_ARGS='-DPS2X_ENABLE_DEBUG_UI=OFF -DPS2X_ENABLE_IOP_RPC_TRACE=OFF -DCMAKE_CXX_FLAGS=-msse4.1' \
./celosia setup --iso "/caminho/Shadow of the Colossus (USA).iso"
```

O `setup` faz, em ordem:

1. **extract:** extrai a ISO para `disc/` (só na primeira vez).
2. **configure:** configura o CMake em `build/cmake` (log em `logs/configure.log`).
3. **stage boot:** recompila o `SCUS_974.72`.
4. **capture + stage game** para `KERNEL`, `MANAGER` e `GAMECORE`: roda o
   jogo até cada módulo carregar, captura a RAM e recompila o módulo
   (logs em `logs/capture-*.log`).
5. **vu1:** roda uma sessão roteirizada (~40 s) para coletar e recompilar o
   microcódigo VU1.

A primeira vez leva bastante tempo (dezenas de minutos, depende da CPU).
`CELOSIA_JOBS` define quantas compilações rodam em paralelo (o padrão é 2).
Cada job usa bastante RAM, então não passe de ~1 job por 2 GB livres.

### 3. Jogar

```sh
./celosia run          # log em logs/play.log
```

### 4. Depois de editar o código

| Você mudou | Comando |
| --- | --- |
| runtime (`PS2Recomp/ps2xRuntime/...`) ou overrides (`game/overrides/...`) | `./celosia build` |
| configuração do recompilador (`game/scus.toml`, `game/extra_stubs.txt`) | `./celosia recompile` |
| jogou partes novas e apareceu microcódigo VU1 novo | `./celosia vu1` |
| quer começar do zero | `./celosia clean` e depois `./celosia setup` |

`./celosia help` lista todos os comandos e variáveis (`CELOSIA_DISC`,
`CELOSIA_BUILD`, `CELOSIA_JOBS`, `CELOSIA_MEMCARDS`, `CELOSIA_CMAKE_ARGS`).

### 5. Testes e verificação

```sh
python3 -m unittest discover -s tests/linux -v   # testes do port (não precisa da ROM)
./celosia autotest 40 5                          # roda 40 s e salva capturas em shots/run
```

O autoteste usa um memory card separado (`build/autotest-memcards`), então
não mexe nos seus saves.

---

## B) Pacote de release (launcher Linux)

Precisa de **Docker** e **Python 3**. **Não** precisa da ROM.

```sh
python3 -m unittest discover -s tests/linux -v
tools/linux/build.sh
```

O `tools/linux/build.sh`:

1. monta a imagem `sotc-linux-builder` (`tools/linux/Dockerfile`) e compila
   o runtime e o recompilador dentro dela;
2. prepara o kit de compilação que vai junto do launcher
   (`tools/linux/prepare_compiler.py`) e junta as licenças;
3. monta a imagem `sotc-desktop-builder` e empacota o launcher GTK com
   PyInstaller;
4. testa o kit dentro de um container sem rede;
5. gera o pacote final (`tools/desktop/release/package_linux.py`) em
   `artifacts/`.

O pacote é verificado para não conter imagens de disco, executáveis do PS2,
módulos XFF, código gerado nem saves. O jogador escolhe a ISO dele no launcher,
e a compilação acontece na máquina dele.

---

## Outras plataformas

- **macOS:** o runtime tem backend Metal (e OpenGL). O `./celosia` foi feito
  originalmente para macOS (`brew install cmake ninja pkg-config`) e roda o
  mesmo `setup`.
- **PS Vita:** `./celosia vita` gera `build/vita/ps2xRuntime/ps2EntryRunner.vpk`.
  Precisa do VitaSDK (`VITASDK`, padrão `~/vitasdk`) e de um `./celosia setup`
  já feito no PC, porque reaproveita o código recompilado de `build/runner`.
- **Windows:** não há suporte nem scripts para Windows neste repositório.

## Quando algo dá errado

| Sintoma | Onde olhar |
| --- | --- |
| `CMake configure failed` | `logs/configure.log` (quase sempre é uma biblioteca `-dev` faltando) |
| `build of ps2EntryRunner failed` | `logs/build-ps2EntryRunner.log` |
| `recompiler failed` | `build/recomp/<estágio>/recomp.log` |
| captura travou | `logs/capture-<MÓDULO>.log` |
| jogo fecha ou trava | `logs/play.log` |
| `no SCUS_974.72 in disc` | a ISO não foi extraída, ou não é a edição USA |
