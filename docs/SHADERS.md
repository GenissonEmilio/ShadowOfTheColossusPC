# Shaders e FSR 1 no desktop

Na aba **Gráficos**, use **Importar shader…**. O port aceita um fragment shader
GLSL 330 em UTF-8, com extensão `.frag`, `.fs` ou `.glsl`, até 256 KiB. Ele é
compilado e vinculado no driver local antes de ser selecionado. Se houver erro,
a seleção anterior permanece ativa. O diagnóstico do driver fica em
`last-shader.log` na biblioteca.

O arquivo é copiado para `shaders/` na biblioteca. Sua origem pode ser movida
depois. O shader escolhido é salvo em `shader.json` e usado na próxima abertura.
Se o driver rejeitar um shader durante a abertura, o jogo usa a imagem sem
shader importado e registra a falha em `last-game.log` e `shader-status.txt`.

## Resolução e FSR 1

Use **F1 → Vídeo → Resolução da janela** para escolher um tamanho de janela.
Em tela cheia, o jogo usa a resolução do monitor. **F1 → Qualidade → Resolução**
controla a resolução interna da renderização, a partir de 512 × 448.

**F1 → Vídeo → Upscaling FSR 1** ativa o FSR 1 da AMD. Ele amplia a imagem
quando a área de exibição é maior que a imagem renderizada, nos modos Ajustar
e Esticar. A resolução interna pode ser reduzida para aliviar a renderização.
O modo Nítido mantém o fator de escala inteiro.

## Interface do fragment shader

```glsl
#version 330 core
in vec2 fragTexCoord;
in vec4 fragColor;
uniform sampler2D texture0;
uniform vec4 colDiffuse;
uniform vec2 sourceSize;
uniform vec2 outputSize;
uniform float time;
out vec4 finalColor;
void main() {
    finalColor = texture(texture0, fragTexCoord) * colDiffuse * fragColor;
}
```

- `texture0`: imagem do jogo que chega ao shader. Quando o FSR 1 atua, o
  shader recebe a imagem ampliada.
- `sourceSize`: largura e altura da imagem que chega ao shader, em pixels.
- `outputSize`: largura e altura da imagem apresentada, em pixels físicos,
  sem incluir as faixas pretas. Considera a escala da tela.
- `time`: segundos desde a inicialização do runtime.
- `fragTexCoord`: UV já orientada pelo apresentador; não inverta Y novamente.

Uniforms não utilizados podem ser omitidos. O estágio de vértices padrão é
fornecido por [raylib 5.5](https://github.com/raysan5/raylib/blob/5.5/src/rcore.c).
O pipeline aplica o shader importado em uma passagem depois do FSR 1. O menu
F1, o contador de FPS e as barras pretas são desenhados na resolução da janela.

**Nitidez HD** é um filtro moderado de nitidez sobre a imagem ampliada. Não é
um pacote de texturas HD nem renderização de geometria em resolução superior.
Presets multipass, dependências externas, texturas auxiliares, includes, ReShade
e presets RetroArch precisam ser adaptados para esta interface de passagem única.

## Teste sem ROM

`compiler/bin/sotc-video-tool --validate arquivo.frag --report resultado.json`
valida o shader em uma janela OpenGL oculta. O código de saída é zero apenas
quando a compilação e a vinculação pelo driver funcionam.

`--self-test pasta` abre uma imagem sintética e verifica os mesmos caminhos de
tela cheia, resolução de janela, shader e FSR 1 usados pelo jogo.
`tools/linux/check_video.py pasta`
confere dimensões e pixels do relatório. Esses testes fazem parte do CI.
