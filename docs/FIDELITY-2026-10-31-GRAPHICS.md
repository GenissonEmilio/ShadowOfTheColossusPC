# Investigação gráfica da campanha — 31/10/2026

Referência inicial: Linux v0.2.6, commit
`986643ac00e6b17882b6b050187748095add283a`.

Foram identificados três defeitos de renderização que correspondem a relatos
anteriores. As correções abaixo são adaptações seletivas de código aberto do
projeto Celosia, sob GPL-3.0. Não houve atualização geral da base.

## Origem das correções

- [`9a154f029413d4e674617c11f056baf3a75db8ab`](https://github.com/HenryKun55/celosia/commit/9a154f029413d4e674617c11f056baf3a75db8ab):
  o buffer preto criado por `bossDeathEffectBegin` substitui o placeholder do
  material nos modelos de efeito; `meshDisp` usa projeção × matriz própria da
  grade para manter os tentáculos de morte à frente da câmera.
- [`f06edb1d9f3991ea039eda85ac74fd843b85157b`](https://github.com/HenryKun55/celosia/commit/f06edb1d9f3991ea039eda85ac74fd843b85157b):
  RGB sem textura usa a escala de bytes 0–255 do GS, enquanto a modulação de
  textura mantém a escala 0x80 = 1. Alpha não foi alterado por essa correção.

## Defeitos e adaptação local

**Máscara da derrota.** O hook de `glEnd` já reconhecia as chamadas de desenho
fora da tela, mas descartava o fan preto do efeito. Os modelos com flag bit 17
continuavam usando a textura do material, um placeholder que podia aparecer
colorido. O hook agora rasteriza o pacote GIF recebido numa textura RGBA
256×256 e conserva quatro slots para os quadros em trânsito. A chave acompanha
o conteúdo: dados iguais reutilizam a máscara, mudanças renovam outro slot.
Os dois contextos dos materiais de efeito selecionam essa máscara sem mips
nem deslocamento de UV. Não foram adicionados ajustes nas coordenadas do
personagem, dano ou flags de progresso do jogo.

**Grades em outro espaço de coordenadas.** `meshDisp` lia a matriz do próprio
buffer para as normais, mas desenhava os vértices sempre com world-to-clip da
câmera. Agora usa projeção × view do buffer quando esta difere da global.
Quando os produtos são equivalentes dentro de 0,1%, preserva a matriz global
bit a bit, mantendo o caminho da capa comum. A correção não muda a câmera
controlada pelo jogador nem a apresentação da janela.

**RGB sem textura.** Os shaders dividiam cores de vértice por 128 para
modulação de textura e reutilizavam essa escala em desenhos sem textura,
produzindo médios tons aproximadamente duas vezes mais claros. O fator neutro
sem textura agora resulta em RGB do vértice dividido por 255. A modulação de
textura e a escala de alpha permanecem distintas. Foram adaptados GLSL,
CG/Vita e Metal; a validação local de GPU usa o backend OpenGL do Linux.

## Regressões sintéticas

Os testes usam somente pacotes, matrizes e cores inventados, sem ROM, funções
traduzidas, modelos, texturas ou saves extraídos do jogo.

| Teste | Código/kit da v0.2.6 | Código adaptado |
| --- | --- | --- |
| Fan GIF recebido por `glEndHook` | Falhou: nenhum upload da máscara | Passou: máscara preta 256×256, alpha interno 0x80 e externo zero |
| Reutilização/renovação da máscara | A máscara não existia | Passou: reutiliza conteúdo estável e renova slot na animação |
| `meshDispHook` com matriz própria | Falhou: selecionava a câmera global | Passou: seleciona projeção × view |
| Grade equivalente à capa comum | Passou | Passou: preserva os bits da matriz global |
| Pixel sem textura: esperado RGB 128/64/32 | Falhou: GPU devolveu 255/128/64 | Passou: GPU devolveu 128/64/32 |

As falhas anteriores foram de comportamento, após compilar os mesmos testes
contra o fonte/kit antigos. A suíte de hooks corrigida terminou com código 0
e oito verificações. Os executores são
[`test_campaign_graphics.sh`](../tools/linux/test_campaign_graphics.sh) e
[`test_renderer_colours.sh`](../tools/linux/test_renderer_colours.sh). O teste
de cor passou nos quatro casos com o runtime recompilado do kit
`ad87b0553bdb0f494e73439f`: RGB sem textura, modulação texturada, faixa completa
e alpha. A integração também repetiu os oito checks de hooks com sucesso.
Os testes locais foram serializados com
`codex-build` limitado a 1024 MiB e containers limitados a 900 MiB e duas CPUs.

## Validação pendente

**Ainda não validado em combate.** Os fixtures demonstram os defeitos
específicos, mas não substituem observar derrota, transformação do corpo,
tentáculos e retorno ao templo numa luta concluída. A seleção da máscara nos
materiais também precisa dessa integração com o desenho real. Não se declara
que todos os pontos fracos, raios de Hydrus ou colisões foram corrigidos.
O avanço da campanha é registrado separadamente pelo playthrough com cartão
próprio de testes. Os cartões do jogador permanecem fora dessa sessão.

Uma quarta origem foi estudada, sem importar suas mudanças:
[`30d75994db513027518d1b9e6e8dd2da1bc326c8`](https://github.com/HenryKun55/celosia/commit/30d75994db513027518d1b9e6e8dd2da1bc326c8),
que trata depth test da paisagem da layer 1 e alpha test GS. Essas alterações
precisam de reprodução e regressões próprias antes de ampliar o patch.
Recorder, políticas de descarte, workers e outras mudanças desse upstream
não fazem parte desta adaptação.

Evidências locais: `artifacts/campaign-2026-10-31/graphics/`, ignorado pelo Git.
São preservados os relatórios dos testes, os patches primários e os hashes de
origem, sem publicar conteúdo do disco ou dos cartões.
