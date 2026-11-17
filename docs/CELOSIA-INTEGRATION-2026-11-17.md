# Integração do Celosia em 17 de novembro de 2026

A revisão de referência do Celosia é [`c2514d9`](https://github.com/HenryKun55/celosia/commit/c2514d9111f9f1f3b0bb41c7e1fa797d8f95a2f5). Nela, o mapa do port marca como nativos os 35 recursos do renderer e os 15 componentes de hardware listados. Esse mapa acompanha a substituição do hardware do PS2 e o suporte do renderer. A contagem da decompilação acompanha a substituição das rotinas MIPS recompiladas por implementações em C++. O lote D13 leva essa contagem upstream a **5.515 de 9.024 funções (61,11%)**. A porcentagem se refere ao Celosia nessa revisão. [Mapa do port](https://github.com/HenryKun55/celosia/blob/c2514d9/docs/port_status.json) · [Contagem da decompilação](https://github.com/HenryKun55/celosia/blob/c2514d9/docs/decomp_status.json)

## Alterações incorporadas

| Arquivo | Alteração |
| --- | --- |
| [`decomp_math.cpp`](../game/overrides/decomp_math.cpp) | `iosGetSinf` e `iosGetCosf`, adaptadas a partir do [lote `23595fb`](https://github.com/HenryKun55/celosia/commit/23595fb). |
| [`decomp_curve.cpp`](../game/overrides/decomp_curve.cpp) | `curveEval` e `motionKeys`, com as operações de ponto flutuante do PS2, a partir de [`11f953c`](https://github.com/HenryKun55/celosia/commit/11f953c). |
| [`decomp_texsearch.cpp`](../game/overrides/decomp_texsearch.cpp) | `texSearchMopByName`, a busca de textura por nome, do [lote `23595fb`](https://github.com/HenryKun55/celosia/commit/23595fb). |
| [`decomp_heap120.cpp`](../game/overrides/decomp_heap120.cpp) | `kernel_sub_001ADDC0` e `kernel_sub_001ADBC0`, rotinas do alocador do jogo de [`4753e08`](https://github.com/HenryKun55/celosia/commit/4753e08). |
| [`native_renderer_gl.cpp`](../PS2Recomp/ps2xRuntime/src/lib/native/native_renderer_gl.cpp) | Ordena os candidatos à remoção do cache OpenGL somente quando o limite de bytes exige remover primeiro as entradas mais antigas. É um trecho de [`53786cf`](https://github.com/HenryKun55/celosia/commit/53786cf). |
| [`main.py`](../tools/desktop/main.py) | Desmapeia as camadas GTK do menu lateral ao fim da animação de fechamento, liberando o clique do mouse nos controles da janela. |

O ensaio de mesclagem indicou conflitos em 24 arquivos, incluindo o renderer, o escalonador EE, a memória e os hooks do jogo. A integração por trechos mantém os caminhos locais de colisão, save states e tela cheia. A mesclagem integral permanece pendente.

## Validação

- O [teste sem ROM](../tools/linux/test_native_decomp.sh) passou com as sete funções registradas. A segunda execução confirmou que `SOTC_DECOMP_SKIP=curveEval` mantém a função recompilada em uso nesse endereço.
- A correção do launcher passou no [teste GTK](../tools/linux/test_launcher_motion.py) e em cliques X11 enviados aos controles da janela, incluindo o menu lateral, a lista de opções nativa e o diálogo de confirmação.
- **Pendente** — teste diferencial com o jogo e execução do CI. Use a lista abaixo para comparar as sete funções com as rotinas recompiladas em execução. `SOTC_DECOMP_SKIP` permite excluir rotinas específicas do registro nativo para executar o caminho recompilado.

```sh
export SOTC_DIFFTEST=iosGetSinf,iosGetCosf,curveEval,motionKeys,texSearchMopByName,kernel_sub_001ADDC0,kernel_sub_001ADBC0
```
